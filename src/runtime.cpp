#include "runtime.hpp"

#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <memory>

namespace runtime {

    namespace _detail
    {
        constexpr size_t kStack = 1 << 20;
        constexpr std::uint64_t kDefaultFlags =  CLONE_NEWPID | /*CLONE_NEWNS  |*/ CLONE_NEWUSER |
                                                CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET;

        template <typename F>
        auto retry(F&& f) -> decltype(f()) {
            for (;;) {
                auto r = f();
                if (r < 0 && errno == EINTR) 
                    continue;
                
                return r;
            }
        }

        struct MunmapDeleter {
            size_t n;
            void operator()(char* p) const noexcept {
                if (p) 
                    ::munmap(p, n);
            }
        };

        std::unique_ptr<char, MunmapDeleter> make_stack(size_t n) {
            void* p = ::mmap(nullptr, n, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);

            return std::unique_ptr<char, MunmapDeleter>{ p == MAP_FAILED ? nullptr : static_cast<char*>(p), MunmapDeleter{n}};
        }
    }
    
    namespace {

        struct ChildArgs {
            const char* rootfs;
            char* const* argv;
            int sync_fd;
            int err_fd;
        };

        void die(int err_fd, int e) 
        {
            int saved = e;
            (void)!::write(err_fd, &saved, sizeof(saved));
            ::_exit(127);
        }

        int child_main(void* p) 
        {
            auto* a = static_cast<ChildArgs*>(p);

            char tok = 0;
            ssize_t n = _detail::retry([&](){ return ::read(a->sync_fd, &tok, 1); });
            if (n != 1 || tok != 'G')
                die(a->err_fd, n < 0 ? errno : EPIPE);

            const char* new_hostname = "my_runtime";
            if (::sethostname(new_hostname, std::strlen(new_hostname)) != 0)
                die(a->err_fd, errno);

            char path[512];
            std::snprintf(path, sizeof(path), "%s/proc", a->rootfs);
            if (::mkdir(path, 0555) != 0 && errno != EEXIST)
                die(a->err_fd, errno);

            std::snprintf(path, sizeof(path), "%s/dev", a->rootfs);
            if (::mkdir(path, 0755) != 0 && errno != EEXIST)
                die(a->err_fd, errno);

            
            std::snprintf(path, sizeof(path), "%s/proc", a->rootfs);
            if (::mount("proc", path, "proc", 0, nullptr) != 0)
                die(a->err_fd, errno);

            std::snprintf(path, sizeof(path), "%s/dev", a->rootfs);
            if (::mount("tmpfs", path, "tmpfs", 0, "mode=0755") != 0)
                die(a->err_fd, errno);

            if (::chroot(a->rootfs) != 0)
                die(a->err_fd, errno);
            if (::chdir("/") != 0)
                die(a->err_fd, errno);

            ::execvp(a->argv[0], a->argv);
            die(a->err_fd, errno);
        }

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

        struct Pipe {
            int r_ = -1, w_ = -1;

            Pipe() {
                int fds[2];
                if (::pipe2(fds, O_CLOEXEC) != 0) 
                    return;
                r_ = fds[0]; w_ = fds[1];
            }

            ~Pipe() {
                if (r_ >= 0) 
                    ::close(r_);
                if (w_ >= 0) 
                    ::close(w_);
            }

            Pipe(const Pipe&) = delete;
            Pipe& operator=(const Pipe&) = delete;

            bool ok() const noexcept { return r_ >= 0 && w_ >= 0; }
        };

    }


    std::expected<int, Error> run(const Request& req) 
    {
        if (req.rootfs.empty() || req.argv.empty())
            return std::unexpected(Error{0, EINVAL, "rootfs/argv empty"});

        Pipe sync, err;
        if (!sync.ok() || !err.ok()) 
            return std::unexpected(Error{1, errno, "pipe create"});

        std::vector<char*> argv = req.argv;
        argv.push_back(nullptr);

        auto stack = _detail::make_stack(_detail::kStack);
        if (!stack) 
            return std::unexpected(Error{2, errno, "mmap stack"});

        ChildArgs args{req.rootfs.c_str(), argv.data(), sync.r_, err.w_};

        pid_t pid = ::clone(child_main, 
                            static_cast<char*>(stack.get()) + _detail::kStack, 
                            _detail::kDefaultFlags | SIGCHLD, 
                            &args);
        if (pid < 0) 
            return std::unexpected(Error{3, errno, "clone"});
        ::close(err.w_); err.w_ = -1; 

        if (!map_user(pid)) {
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

        int status = 0;
        pid_t r = _detail::retry([&]() { return ::waitpid(pid, &status, 0); });
        if (r < 0)
            return std::unexpected(Error{7, errno, "waitpid"});

        if (WIFEXITED(status))   return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return std::unexpected(Error{7, 0, "abnormal status"});
    }

} 