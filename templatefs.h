//
// Created by paul on 5/27/20.
//

#ifndef TEMPLATEFS_H
#define TEMPLATEFS_H

#define FUSE_USE_VERSION 39

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

#include <fuse3/fuse_lowlevel.h>

#undef HAVE_LIBULOCKMGR
#undef HAVE_SETXATTR
#undef HAVE_FSTATAT
#undef HAVE_UTIMENSAT
#undef HAVE_FDATASYNC
#undef HAVE_POSIX_FALLOCATE
#undef HAVE_COPY_FILE_RANGE

#define UNUSED( arg ) __attribute__((unused)) arg

typedef struct fuse_cmdline_opts tCommonOptions;

typedef struct
{
    char * templates;
} tTemplateOptions;

typedef struct {
    char  * path;
    DIR   * dir;
    int     fd;
} tFSTree;

typedef struct
{
    const char * myName;        // the name used to start this executable

    tFSTree      mountpoint;    // the absolute path to the mount point
    tFSTree      templates;  // absolute path to the top of the template hierarchy

    struct
    {
        tCommonOptions common;
        tTemplateOptions template;
    } options;
} tGlobals;

extern tGlobals globals;

#endif // TEMPLATEFS_H
