cmake_minimum_required(VERSION 3.16)
project(templatefs C)

set(CMAKE_C_STANDARD 11)

include_directories(.)

find_library(DLFCN dl)
find_library(FUSE3 fuse3)
find_library(PTHREAD pthread)

add_executable(templatefs templatefs.c templatefs.h callbacks.c callbacks.h )

target_link_libraries(templatefs ${DLFCN} ${FUSE3} ${PTHREAD})

target_compile_options(templatefs PRIVATE -g)
target_link_options(templatefs PRIVATE -rdynamic)
