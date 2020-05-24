/*
    derived from passthrough_fh.c, part of the libfuse project.
    See https://github.com/libfuse/libfuse/blob/master/example/passthrough_fh.c

    Since the original is distributed under the terms of the GNU GPLv2
    and this counts as a 'derivative work', it is also provided under
    the terms of GPL v2.

    Copyright (c) Paul Chambers, 2020. All rights resulterved.
*/

/** @file
 *
 * This file system mirrors the existing file system hierarchy of the
 * system, starting at the root file system. This is implemented by
 * just "passing through" all requests to the corresultponding user-space
 * libc functions. This implementation is a little more sophisticated
 * than the one in passthrough.c, so performance is not quite as bad.
 *
 * ## Source code ##
 * \include templatefs.c
 */

#define FUSE_USE_VERSION 39

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE

#include <fuse3/fuse.h>

#ifdef HAVE_LIBULOCKMGR
#include <ulockmgr.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>

#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#include <sys/file.h> /* flock(2) */

static void * tmpl_init( struct fuse_conn_info * conn, struct fuse_config * cfg )
{
    (void) conn;
    cfg->use_ino     = 1;
    cfg->nullpath_ok = 1;

    /* Pick up changes from lower filesystem right away. This is
       also necessary for better hardlink support. When the kernel
       calls the unlink() handler, it does not know the inode of
       the to-be-removed entry and can therefore not invalidate
       the cache of the associated inode - resulting in an
       incorrect st_nlink value being reported for any remaining
       hardlinks to this inode. */
    cfg->entry_timeout    = 0;
    cfg->attr_timeout     = 0;
    cfg->negative_timeout = 0;

    return NULL;
}

static int tmpl_getattr( const char * path, struct stat * stbuf, struct fuse_file_info * fi )
{
    if ( fi )
    {
        return( (fstat( fi->fh, stbuf ) == -1)? -errno : 0);
    }
    else
    {
        return( (lstat( path, stbuf ) == -1)? -errno : 0);
    }
}

static int tmpl_access( const char * path, int mask )
{
    return( (access( path, mask ) == -1)? -errno : 0);
}

static int tmpl_readlink( const char * path, char * buf, size_t size )
{
    int result;

    result = readlink( path, buf, size - 1 );
    if ( result == -1 )
    {
        return -errno;
    }

    buf[result] = '\0';
    return 0;
}

struct tmpl_dirp
{
    DIR           * dp;
    struct dirent * entry;
    off_t           offset;
};

static int tmpl_opendir( const char * path, struct fuse_file_info * fi )
{
    int result;
    struct tmpl_dirp * d = malloc( sizeof( struct tmpl_dirp ));

    if ( d == NULL)
    {
        return -ENOMEM;
    }

    d->dp = opendir( path );
    if ( d->dp == NULL)
    {
        result = -errno;
        free( d );
        return result;
    }
    d->offset = 0;
    d->entry  = NULL;

    fi->fh = (unsigned long) d;
    return 0;
}

static inline struct tmpl_dirp * get_dirp( struct fuse_file_info * fi )
{
    return (struct tmpl_dirp *) (uintptr_t) fi->fh;
}

static int tmpl_readdir( const char * path, void * buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info * fi,
                         enum fuse_readdir_flags flags )
{
    (void) path;

    struct tmpl_dirp * d = get_dirp( fi );

    if ( offset != d->offset )
    {
#ifndef __FreeBSD__
        seekdir( d->dp, offset );
#else
        /* Subtract the one that we add when calling
                   telldir() below */
        seekdir( d->dp, offset - 1 );
#endif
        d->entry  = NULL;
        d->offset = offset;
    }
    while ( 1 )
    {
        struct stat              st;
        off_t                    nextoff;
        enum fuse_fill_dir_flags fill_flags = 0;

        if ( !d->entry )
        {
            d->entry = readdir( d->dp );
            if ( !d->entry )
            {
                break;
            }
        }
#ifdef HAVE_FSTATAT
        if (flags & FUSE_READDIR_PLUS) {
            int result;

            result = fstatat( dirfd(d->dp), d->entry->d_name, &st, AT_SYMLINK_NOFOLLOW);
            if (result != -1)
            {
                fill_flags |= FUSE_FILL_DIR_PLUS;
            }
        }
#endif
        if ( !(fill_flags & FUSE_FILL_DIR_PLUS))
        {
            memset( &st, 0, sizeof( st ));
            st.st_ino  = d->entry->d_ino;
            st.st_mode = d->entry->d_type << 12;
        }
        nextoff = telldir( d->dp );
#ifdef __FreeBSD__
        /* Under FreeBSD, telldir() may return 0 the first time
                   it is called. But for libfuse, an offset of zero
                   means that offsets are not supported, so we shift
                   everything by one. */
        nextoff++;
#endif
        if ( filler( buf, d->entry->d_name, &st, nextoff, fill_flags ))
        {
            break;
        }

        d->entry  = NULL;
        d->offset = nextoff;
    }

    return 0;
}

static int tmpl_releasedir( const char * path, struct fuse_file_info * fi )
{
    (void) path;

    struct tmpl_dirp * d = get_dirp( fi );
    closedir( d->dp );
    free( d );

    return 0;
}

static int tmpl_mknod( const char * path, mode_t mode, dev_t rdev )
{
    if ( S_ISFIFO( mode ) )
    {
        return( (mkfifo( path, mode ) == -1)? -errno : 0);
    }
    else
    {
        return( (mknod( path, mode, rdev ) == -1)? -errno : 0);
    }
}

static int tmpl_mkdir( const char * path, mode_t mode )
{
    return( (mkdir( path, mode ) == -1)? -errno : 0);
}

static int tmpl_unlink( const char * path )
{
    return ( (unlink( path ) == -1)? -errno : 0);
}

static int tmpl_rmdir( const char * path )
{
    return ( (rmdir( path ) == -1)? -errno : 0 );
}

static int tmpl_symlink( const char * from, const char * to )
{
    return ( (symlink( from, to ) == -1)? -errno : 0);
}

static int tmpl_rename( const char * from, const char * to, unsigned int flags )
{
    /* When we have renameat2() in libc, then we can implement flags */
    if ( flags )
    {
        return -EINVAL;
    }

    return ( (rename( from, to ) == -1)? -errno : 0);
}

static int tmpl_link( const char * from, const char * to )
{
    return ( (link( from, to ) == -1)? -errno : 0);
}

static int tmpl_chmod( const char * path, mode_t mode, struct fuse_file_info * fi )
{
    if ( fi )
    {
        return ( (fchmod( fi->fh, mode ) == -1)? -errno : 0);
    }
    else
    {
        return ( (chmod( path, mode ) == -1)? -errno : 0);
    }
}

static int tmpl_chown( const char * path, uid_t uid, gid_t gid,
                       struct fuse_file_info * fi )
{
    if ( fi )
    {
        return( (fchown( fi->fh, uid, gid ) == -1)? -errno : 0);
    }
    else
    {
        return( (lchown( path, uid, gid ) == -1)? -errno : 0);
    }
}

static int tmpl_truncate( const char * path, off_t size,
                          struct fuse_file_info * fi )
{
    if ( fi )
    {
        return( ( ftruncate( fi->fh, size ) == -1)? -errno : 0);
    }
    else
    {
        return( ( truncate( path, size ) == -1)? -errno : 0);
    }
}

#ifdef HAVE_UTIMENSAT
static int tmpl_utimens( const char *path,
                         const struct timespec ts[2],
                         struct fuse_file_info *fi)
{
    /* don't use utime/utimes since they follow symlinks */
    if (fi)
    {
        return( (futimens(fi->fh, ts) == -1)? -errno : 0);
    }
    else
    {
        return( (utimensat(0, path, ts, AT_SYMLINK_NOFOLLOW) == -1)? -errno : 0);
    }
}
#endif

static int tmpl_create( const char * path, mode_t mode,
                        struct fuse_file_info * fi )
{
    int fd = open( path, fi->flags, mode );
    if ( fd == -1 )
    {
        return -errno;
    }

    fi->fh = fd;
    return 0;
}

static int tmpl_open( const char * path, struct fuse_file_info * fi )
{
    int fd = open( path, fi->flags );
    if ( fd == -1 )
    {
        return -errno;
    }

    fi->fh = fd;
    return 0;
}

static int tmpl_read( const char * path,
                      char * buf,
                      size_t size,
                      off_t offset,
                      struct fuse_file_info * fi )
{
    (void) path;

    int result = pread( fi->fh, buf, size, offset );
    return( ( result == -1 )? -errno : result);
}

static int tmpl_read_buf( const char * path,
                          struct fuse_bufvec ** bufp,
                          size_t size,
                          off_t offset,
                          struct fuse_file_info * fi )
{
    struct fuse_bufvec * src;

    (void) path;

    src = malloc( sizeof( struct fuse_bufvec ));
    if ( src == NULL)
    {
        return -ENOMEM;
    }

    *src = FUSE_BUFVEC_INIT( size );

    src->buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
    src->buf[0].fd    = fi->fh;
    src->buf[0].pos   = offset;

    *bufp = src;

    return 0;
}

static int tmpl_write( const char * path, const char * buf, size_t size,
                       off_t offset, struct fuse_file_info * fi )
{
    int result;

    (void) path;

    result = pwrite( fi->fh, buf, size, offset );
    if ( result == -1 )
    {
        result = -errno;
    }

    return result;
}

static int tmpl_write_buf( const char * path, struct fuse_bufvec * buf,
                           off_t offset, struct fuse_file_info * fi )
{
    struct fuse_bufvec dst = FUSE_BUFVEC_INIT( fuse_buf_size( buf ));

    (void) path;

    dst.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
    dst.buf[0].fd    = fi->fh;
    dst.buf[0].pos   = offset;

    return fuse_buf_copy( &dst, buf, FUSE_BUF_SPLICE_NONBLOCK );
}

static int tmpl_statfs( const char * path, struct statvfs * stbuf )
{
    return( (statvfs( path, stbuf ) == -1)? -errno : 0);
}

static int tmpl_flush( const char * path, struct fuse_file_info * fi )
{
    (void) path;

    /* This is called from every close on an open file, so call the
       close on the underlying filesystem.	But since flush may be
       called multiple times for an open file, this must not really
       close the file.  This is important if used on a network
       filesystem like NFS which flush the data/metadata on close() */
    return ( (close( dup( fi->fh )) == -1)? -errno : 0);
}

static int tmpl_release( const char * path, struct fuse_file_info * fi )
{
    (void) path;
    close( fi->fh );

    return 0;
}

static int tmpl_fsync( const char * path, int isdatasync, struct fuse_file_info * fi )
{
    int result;
    (void) path;

#ifndef HAVE_FDATASYNC
    (void) isdatasync;
#else
    if (isdatasync)
    {
        result = fdatasync(fi->fh);
    }
    else
#endif
    {
        result = fsync( fi->fh );
    }

    return( (result == -1)? -errno : 0 );
}

#ifdef HAVE_POSIX_FALLOCATE
static int tmpl_fallocate( const char *path,
                           int mode,
                           off_t offset,
                           off_t length,
                           struct fuse_file_info *fi)
{
    (void)path;

    if (mode) return -EOPNOTSUPP;

    return -posix_fallocate(fi->fh, offset, length);
}
#endif

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int tmpl_setxattr( const char *path,
                          const char *name,
                          const char *value,
                          size_t size,
                          int flags)
{
    return( (lsetxattr(path, name, value, size, flags) == -1)? -errno : 0);
}

static int tmpl_getxattr( const char *path,
                          const char *name,
                          char *value,
                          size_t size)
{
    return( (lgetxattr(path, name, value, size) == -1)? -errno : 0);
}

static int tmpl_listxattr(const char *path, char *list, size_t size)
{
    return( (llistxattr(path, list, size) == -1)? -errno : 0);
}

static int tmpl_removexattr(const char *path, const char *name)
{
    return( (lremovexattr(path, name) == -1)? -errno : 0);
}
#endif /* HAVE_SETXATTR */

#ifdef HAVE_LIBULOCKMGR
static int tmpl_lock(const char *path, struct fuse_file_info *fi, int cmd,
                    struct flock *lock)
{
    (void)path;

    return ulockmgr_op(fi->fh, cmd, lock, &fi->lock_owner,
                       sizeof(fi->lock_owner));
}
#endif

static int tmpl_flock( const char * path, struct fuse_file_info * fi, int op )
{
    (void) path;

    return( (flock( fi->fh, op ) == -1)? -errno : 0);
}

#ifdef HAVE_COPY_FILE_RANGE
static ssize_t tmpl_copy_file_range( const char *path_in,
                                     struct fuse_file_info *fi_in, off_t off_in,
                                     const char *path_out,
                                     struct fuse_file_info *fi_out, off_t off_out,
                                     size_t len, int flags)
{
    ssize_t result;
    (void)path_in;
    (void)path_out;

    result = copy_file_range(fi_in->fh, &off_in, fi_out->fh, &off_out, len, flags);
    return( (result == -1)? -errno : 0);
}
#endif

static off_t tmpl_lseek( const char * path, off_t off, int whence,
                         struct fuse_file_info * fi )
{
    off_t result;
    (void) path;

    result = lseek( fi->fh, off, whence );
    return( ( result == -1 )? -errno: result );
}

static const struct fuse_operations tmpl_oper = {
        .init       = tmpl_init,
        .getattr    = tmpl_getattr,
        .access     = tmpl_access,
        .readlink   = tmpl_readlink,
        .opendir    = tmpl_opendir,
        .readdir    = tmpl_readdir,
        .releasedir = tmpl_releasedir,
        .mknod      = tmpl_mknod,
        .mkdir      = tmpl_mkdir,
        .symlink    = tmpl_symlink,
        .unlink     = tmpl_unlink,
        .rmdir      = tmpl_rmdir,
        .rename     = tmpl_rename,
        .link       = tmpl_link,
        .chmod      = tmpl_chmod,
        .chown      = tmpl_chown,
        .truncate   = tmpl_truncate,
#ifdef HAVE_UTIMENSAT
        .utimens    = tmpl_utimens,
#endif
        .create     = tmpl_create,
        .open       = tmpl_open,
        .read       = tmpl_read,
        .read_buf   = tmpl_read_buf,
        .write      = tmpl_write,
        .write_buf  = tmpl_write_buf,
        .statfs     = tmpl_statfs,
        .flush      = tmpl_flush,
        .release    = tmpl_release,
        .fsync      = tmpl_fsync,
#ifdef HAVE_POSIX_FALLOCATE
        .fallocate  = tmpl_fallocate,
#endif
#ifdef HAVE_SETXATTR
        .setxattr   = tmpl_setxattr,
        .getxattr   = tmpl_getxattr,
        .listxattr  = tmpl_listxattr,
        .removexattr = tmpl_removexattr,
#endif
#ifdef HAVE_LIBULOCKMGR
        .lock       = tmpl_lock,
#endif
        .flock      = tmpl_flock,
#ifdef HAVE_COPY_FILE_RANGE
        .copy_file_range = tmpl_copy_file_range,
#endif
        .lseek      = tmpl_lseek,
};

int main( int argc, char * argv[] )
{
    umask( 0 );
    return fuse_main( argc, argv, &tmpl_oper, NULL );
}
