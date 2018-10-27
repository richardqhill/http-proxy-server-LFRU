cmake_minimum_required(VERSION 3.12)
project(hw2_handout)

set(CMAKE_CXX_STANDARD 14)

include_directories(.)
include_directories(cmake-build-debug)
include_directories(tiny)

add_executable(
        csapp.c
        csapp.h
        driver.sh
        free-port.sh
        nop-server.py
        port-for-user.pl
        proxy
        proxy.c
 )