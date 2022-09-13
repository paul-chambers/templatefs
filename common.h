//
// Created by paul on 9/11/22.
//

#ifndef TEMPLATEFS_COMMON_H
#define TEMPLATEFS_COMMON_H

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <unistd.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>

#define UNUSED(arg) __attribute__((unused)) arg

/* #define LOG_ON_ENTRY( fmt, ... ) \
    fuse_log( FUSE_LOG_DEBUG, "%s" fmt, __func__, ##__VA_ARGS__ )
*/

#endif //TEMPLATEFS_COMMON_H
