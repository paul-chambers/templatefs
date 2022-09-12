cmake_minimum_required(VERSION 3.16)
project(templatefs C)

set(CMAKE_C_STANDARD 11)

include_directories(.)

find_library(DLFCN dl)
find_library(FUSE3 fuse3)
find_library(PTHREAD pthread)
find_library(LUA lua5.4)
find_library(MUSTACH mustach)

find_package(Elektra REQUIRED)

if (ELEKTRA_FOUND)
    message (STATUS "Elektra ${ELEKTRA_VERSION} found")
    include_directories (${ELEKTRA_INCLUDE_DIR})
else (ELEKTRA_FOUND)
    message (FATAL_ERROR "Elektra not found")
endif (ELEKTRA_FOUND)

add_executable(templatefs
        templatefs.c templatefs.h
        fuseOperations.c fuseOperations.h
        processTemplate.c processTemplate.h common.h)

target_link_libraries(templatefs ${DLFCN} ${FUSE3} ${PTHREAD} ${LUA} ${MUSTACH} ${ELEKTRA_LIBRARIES})

target_compile_options(templatefs PRIVATE -g)
target_link_options(templatefs PRIVATE -rdynamic)
