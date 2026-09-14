cmake_minimum_required(VERSION 3.16)
project(strazh CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(myinit main.cpp)
target_compile_options(myinit PRIVATE -Wall -Wextra)

add_executable(stage1 stage1.cpp)
target_compile_options(stage1 PRIVATE -Wall -Wextra)

add_executable(supervisor supervisor.cpp)
target_compile_options(supervisor PRIVATE -Wall -Wextra)

add_executable(svctl svctl.cpp)
target_compile_options(svctl PRIVATE -Wall -Wextra)
