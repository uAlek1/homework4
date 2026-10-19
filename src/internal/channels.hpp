#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>

struct PtyChannel {
    int master = -1;
    int slave  = -1;

    PtyChannel() {
        master = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0) 
            return;

        if (::grantpt(master) < 0 || ::unlockpt(master) < 0) {
            ::close(master); 
            master = -1;
            return;
        }

        const char* name = ::ptsname(master);
        if (!name) { 
            ::close(master); 
            master = -1; 
            return; 
        }

        slave = ::open(name, O_RDWR | O_NOCTTY);
        if (slave < 0) { 
            ::close(master); 
            master = -1; 
            return; 
        }

        struct winsize ws{};
        if (::ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
            ::ioctl(master, TIOCSWINSZ, &ws);
    }

    ~PtyChannel() {
        if (slave  >= 0) 
            ::close(slave);
        if (master >= 0) 
            ::close(master);
    }

    PtyChannel(const PtyChannel&) = delete;
    PtyChannel& operator=(const PtyChannel&) = delete;

    bool ok() const noexcept { return master >= 0 && slave >= 0; }
};



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