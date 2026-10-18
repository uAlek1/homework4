#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <expected>
#include <system_error>

namespace runtime {

    struct Error {
        int stage;      // 1=pipe, 2=mmap 3=clone, 4=map, 5=signal, 6=child, 7=wait
        int err;
        std::string msg;
    };

    struct Request {
        std::string rootfs;
        std::vector<char*> argv;
    };


    std::expected<int, Error> run(const Request& req);

}