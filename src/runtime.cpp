#include "runtime.hpp"

#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <cstdio>


#include <expected>
#include <sys/stat.h>
#include "internal/terminal.hpp"
#include "internal/utils.hpp"
#include "internal/channels.hpp"

namespace runtime
{

    struct ChildArgs {
        const char* rootfs;
        char* const* argv;
        int sync_fd;
        int err_fd;
        int pty_slave_fd;
    };

    namespace child
    {
        void die(int err_fd, int e) 
        {
            int saved = e;
            (void)!::write(err_fd, &saved, sizeof(saved));
            ::_exit(127);
        }

        void mount_at ( const char* rootfs, const char* sub, mode_t mode,
                        const char* fstype, unsigned long flags, const char* data,
                        int err_fd )
        {
            char path[512];
            std::snprintf(path, sizeof(path), "%s%s", rootfs, sub);

            if (::mkdir(path, mode) != 0 && errno != EEXIST)
                die(err_fd, errno);

            if (::mount(fstype, path, fstype, flags, data) != 0)
                die(err_fd, errno);
        }

        void attach_pty(int slave_fd, int err_fd) {
            if (::setsid() < 0)
                die(err_fd, errno);
            if (::ioctl(slave_fd, TIOCSCTTY, 0) < 0)
                die(err_fd, errno);
            if (::dup2(slave_fd, STDIN_FILENO)  < 0 ||
                ::dup2(slave_fd, STDOUT_FILENO) < 0 ||
                ::dup2(slave_fd, STDERR_FILENO) < 0)
                die(err_fd, errno);
            if (slave_fd > STDERR_FILENO)
                ::close(slave_fd);
        }

        int run(void* p) 
        {
            auto* a = static_cast<ChildArgs*>(p);

            char tok = 0;
            ssize_t n = _detail::retry([&](){ return ::read(a->sync_fd, &tok, 1); });
            if (n != 1 || tok != 'G')
                die(a->err_fd, n < 0 ? errno : EPIPE);

            const char* new_hostname = "my_runtime";
            if (::sethostname(new_hostname, std::strlen(new_hostname)) != 0)
                die(a->err_fd, errno);

            mount_at(a->rootfs, "/proc", 0555, "proc",  0, nullptr,     a->err_fd);
            mount_at(a->rootfs, "/dev",  0755, "tmpfs", 0, "mode=0755", a->err_fd);         

            if (a->pty_slave_fd >= 0)
                attach_pty(a->pty_slave_fd, a->err_fd);

            if (::chroot(a->rootfs) != 0)
                die(a->err_fd, errno);
            if (::chdir("/") != 0)
                die(a->err_fd, errno);

            ::execvp(a->argv[0], a->argv);
            die(a->err_fd, errno);
        }
    }

    namespace parent
    {
        bool write_proc(const char* path, const char* data) {
            int fd = ::open(path, O_WRONLY | O_CLOEXEC);
            if (fd < 0) 
                return false;
            size_t len = std::strlen(data);
            ssize_t w = ::write(fd, data, len);
            ::close(fd);
            return w == (ssize_t)len;
        }

        bool map_user(pid_t pid) {
            char path[64], buf[64];
            std::snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
            std::snprintf(buf, sizeof(buf), "0 %u 1", ::getuid());
            if (!write_proc(path, buf)) {
                std::fprintf(stderr, "map_user: uid_map failed: %s\n", std::strerror(errno));
                return false;
            }
            std::snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
            if (::access(path, F_OK) == 0) {
                if (!write_proc(path, "deny")) {
                    std::fprintf(stderr, "map_user: setgroups failed: %s\n", std::strerror(errno));
                    return false;
                }
            }
            std::snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
            std::snprintf(buf, sizeof(buf), "0 %u 1", ::getgid());
            if (!write_proc(path, buf)) {
                std::fprintf(stderr, "map_user: gid_map failed: %s\n", std::strerror(errno));
                return false;
            }
            return true;
        }
    }



    constexpr size_t kStack = 1 << 20;
    constexpr std::uint64_t kDefaultFlags =  CLONE_NEWPID | CLONE_NEWNS  | CLONE_NEWUSER |
                                             CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET;

    std::expected<int, Error> run(const Request& req) 
    {
        if (req.rootfs.empty() || req.argv.empty())
            return std::unexpected(Error{0, EINVAL, "rootfs/argv empty"});

        Pipe sync, err;
        if (!sync.ok() || !err.ok()) 
            return std::unexpected(Error{1, errno, "pipe create"});
        PtyChannel pty;
        if (!pty.ok())
            return std::unexpected(Error{1, errno, "pty open"});

        std::vector<char*> argv = req.argv;
        argv.push_back(nullptr);

        auto stack = _detail::make_stack(kStack);
        if (!stack) 
            return std::unexpected(Error{2, errno, "mmap stack"});

        ChildArgs args{req.rootfs.c_str(), argv.data(), sync.r_, err.w_, pty.slave};

        pid_t pid = ::clone(child::run, 
                            static_cast<char*>(stack.get()) + kStack, 
                            kDefaultFlags | SIGCHLD, 
                            &args);
        if (pid < 0) 
            return std::unexpected(Error{3, errno, "clone"});
        ::close(err.w_); err.w_ = -1;
        ::close(pty.slave); pty.slave = -1; 

        if (!parent::map_user(pid)) {
            int e = errno;
            ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0);
            return std::unexpected(Error{4, e, "map user"});
        }

        char tok = 'G';
        if (::write(sync.w_, &tok, 1) != 1) {
            int e = errno;
            ::kill(pid, SIGKILL); 
            ::waitpid(pid, nullptr, 0);
            return std::unexpected(Error{5, e, "signal"});
        }
        ::close(sync.w_); sync.w_ = -1; 

        int child_err = 0;
        ssize_t n = _detail::retry([&](){return ::read(err.r_, &child_err, sizeof(child_err));});
        if (n < 0) {
            int e = errno;
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
            return std::unexpected(Error{6, e, "read err"});
        }
        if (n == sizeof(child_err) && child_err != 0) {
            ::waitpid(pid, nullptr, 0);
            return std::unexpected(Error{6, child_err, "child"});
        }

        struct termios saved{};
        bool raw = terminal::enter_raw(STDIN_FILENO, &saved);
        struct Restore {
            int fd;
            struct termios saved;
            bool active;
            ~Restore() { 
                if (active) 
                    terminal::restore(fd, &saved); 
            }
        } restore{STDIN_FILENO, saved, raw};
        terminal::relay(pty.master);

        int status = 0;
        pid_t r = _detail::retry([&]() { return ::waitpid(pid, &status, 0); });
        if (r < 0)
            return std::unexpected(Error{7, errno, "waitpid"});

        if (WIFEXITED(status))   return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return std::unexpected(Error{7, 0, "abnormal status"});
    }
}
