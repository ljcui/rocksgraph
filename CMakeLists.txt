cmake_minimum_required(VERSION 3.15)
project(rocksgraph)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(rg-server
    main.cc
)
