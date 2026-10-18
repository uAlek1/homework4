#include "runtime.hpp"
#include <cstdio>
#include <cstring>
#include <string>

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --rootfs <path> <cmd> [args...]\n"
        "  --rootfs <path>   root filesystem for the container\n",
        prog);
}

int main(int argc, char** argv) {
    runtime::Request req;
    int i = 1;
    if (std::strcmp(argv[i++], "--rootfs") == 0) 
    {
        req.rootfs = argv[i++];
    } 
    else 
    {
        std::fprintf(stderr, "unknown option: %s\n", argv[i]);
        usage(argv[0]);
        return 2;
    }

    for (; i < argc; ++i) 
        req.argv.emplace_back(argv[i]);

    auto result = runtime::run(req);
    if (!result) {
        auto& e = result.error();
        std::fprintf(stderr, "[runtime] stage=%d: %s (errno=%d)\n",
                     e.stage, e.msg.c_str(), e.err);
        return 1;
    }
    return *result;
}