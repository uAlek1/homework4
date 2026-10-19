cmake_minimum_required(VERSION 3.10.0)
project(runtime VERSION 0.1.0 LANGUAGES C CXX)


set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


add_executable(runtime  src/main.cpp
                        src/runtime.cpp
                        src/internal/terminal.cpp)

target_include_directories(runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
