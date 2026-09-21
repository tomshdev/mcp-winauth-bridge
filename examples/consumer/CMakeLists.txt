# Proves consumption mode (b): a separate project finds the installed package
# and links the static library. CI builds this on every push.
cmake_minimum_required(VERSION 3.21)
project(mcpwinauth_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(mcpwinauth 1.0 CONFIG REQUIRED)

add_executable(consumer main.cpp)
if(MSVC)
  target_compile_options(consumer PRIVATE /utf-8)
endif()
target_link_libraries(consumer PRIVATE mcpwinauth::bridge)
