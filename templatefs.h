//
// Created by paul on 5/27/20.
//

#ifndef TEMPLATEFS_H
#define TEMPLATEFS_H

#undef HAVE_LIBULOCKMGR
#undef HAVE_SETXATTR
#undef HAVE_FSTATAT
#undef HAVE_UTIMENSAT
#undef HAVE_FDATASYNC
#undef HAVE_POSIX_FALLOCATE
#undef HAVE_COPY_FILE_RANGE

#define FUSE_USE_VERSION 39
#include <fuse3/fuse_lowlevel.h>

typedef struct {
    char * templates;
} tTemplateOptions;

typedef struct {
    const char  *             myName;   // the name used to start this executable
    char * const *            envp;     // the environment passed into main()

    struct fuse_cmdline_opts  options;  // standard FUSE command line options
    tTemplateOptions          template; // additional templatefs-specific cmd line options
} tGlobals;

extern tGlobals globals;

#endif // TEMPLATEFS_H
