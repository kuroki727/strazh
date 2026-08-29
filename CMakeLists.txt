cmake_minimum_required(VERSION 3.16)
project(myinit CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(myinit src/main.cpp)
target_compile_options(myinit PRIVATE -Wall -Wextra)

add_executable(stage1 src/stage1.cpp)
target_compile_options(stage1 PRIVATE -Wall -Wextra)
