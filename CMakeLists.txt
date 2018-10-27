cmake_minimum_required(VERSION 3.12)
project(hw2_handout)

set(CMAKE_CXX_STANDARD 14)

include_directories(.)
include_directories(cmake-build-debug)
include_directories(tiny)

set(SOURCE_FILES
        csapp.h
        csapp.c
        proxy.c
        )

add_executable(Proxy proxy.c csapp.c)