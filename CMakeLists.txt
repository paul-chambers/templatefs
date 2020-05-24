cmake_minimum_required(VERSION 3.16)
project(templatefs C)

set(CMAKE_C_STANDARD 11)

include_directories(.)

find_library(FUSE fuse3)
find_library(PTHREAD pthread)

add_executable(templatefs templatefs.c config.h passthrough_helpers.h)

target_link_libraries(templatefs ${FUSE} ${PTHREAD})