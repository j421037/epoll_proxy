cmake_minimum_required(VERSION 3.16)
project(epoll_proxy)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
add_executable(epoll_proxy main.cpp)