// src/runtime/internal/terminal.hpp
#pragma once
#include <termios.h>

namespace terminal {


bool enter_raw(int fd, struct termios* saved);

void restore(int fd, const struct termios* saved);

void relay(int master, int host_in = 0, int host_out = 1);


}