#include "terminal.hpp"
#include <unistd.h>
#include <sys/epoll.h>
#include <cerrno>

namespace {

    bool write_all(int fd, const char* buf, ssize_t n) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = ::write(fd, buf + off, n - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            off += w;
        }
        return true;
    }

}

bool terminal::enter_raw(int fd, struct termios* saved) {
    if (!::isatty(fd)) 
        return false;
    if (::tcgetattr(fd, saved) < 0) 
        return false;

    struct termios raw = *saved;
    ::cfmakeraw(&raw);

    return ::tcsetattr(fd, TCSANOW, &raw) == 0;
}

void terminal::restore(int fd, const struct termios* saved) {
    if (saved)
        ::tcsetattr(fd, TCSANOW, saved);
}

void terminal::relay(int master, int host_in, int host_out) {
    int epfd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) 
        return;

    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = host_in;
    if (::epoll_ctl(epfd, EPOLL_CTL_ADD, host_in, &ev) < 0) {
        ::close(epfd); return;
    }

    ev.events = EPOLLIN;
    ev.data.fd = master;
    if (::epoll_ctl(epfd, EPOLL_CTL_ADD, master, &ev) < 0) {
        ::close(epfd); return;
    }

    char buf[4096];
    struct epoll_event events[2];

    for (;;) {
        int nfds = ::epoll_wait(epfd, events, 2, -1);
        if (nfds < 0) {
            if (errno == EINTR) 
                continue;
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            if (fd == host_in) {
                ssize_t n = ::read(host_in, buf, sizeof(buf));
                if (n <= 0) { 
                    ::close(epfd); 
                    return; 
                }
                if (!write_all(master, buf, n)) { 
                    ::close(epfd); 
                    return; 
                }
            } else if (fd == master) {
                ssize_t n = ::read(master, buf, sizeof(buf));
                if (n <= 0) { 
                    ::close(epfd); 
                    return; 
                }
                if (!write_all(host_out, buf, n)) { 
                    ::close(epfd); 
                    return; }
            }
        }
    }

    ::close(epfd);
}