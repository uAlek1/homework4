#include <memory>
#include <sys/mman.h>

namespace _detail {

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