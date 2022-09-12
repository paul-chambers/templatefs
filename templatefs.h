//
// Created by paul on 5/27/20.
//

#ifndef TEMPLATEFS_H
#define TEMPLATEFS_H

#define FUSE_USE_VERSION 39

#include <fuse3/fuse_lowlevel.h>

#undef HAVE_LIBULOCKMGR
#undef HAVE_SETXATTR
#undef HAVE_FSTATAT
#undef HAVE_UTIMENSAT
#undef HAVE_FDATASYNC
#undef HAVE_POSIX_FALLOCATE
#undef HAVE_COPY_FILE_RANGE

#define UNUSED( arg ) __attribute__((unused)) arg

typedef unsigned char byte;

#endif // TEMPLATEFS_H
