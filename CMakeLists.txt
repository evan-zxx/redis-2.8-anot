cmake_minimum_required(VERSION 2.6.4)

project(de-redis)

FILE(GLOB_RECURSE LOCAL_CPP_FILES "src/*.c")

set(CMAKE_CXX_COMPILER gcc)


add_executable(de-redis
        ${LOCAL_CPP_FILES}
        )