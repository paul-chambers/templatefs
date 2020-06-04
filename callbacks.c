//
// Created by paul on 5/27/20.
//

#include "templatefs.h"

#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#include <sys/file.h> /* flock(2) */

#include <fuse3/fuse.h>

#include "callbacks.h"

#define LOG_ON_ENTRY( fmt, ... )   fuse_log( FUSE_LOG_DEBUG, "%s" fmt, __func__, ##__VA_ARGS__ )

typedef struct
{
    int fd;
    int isTemplate;
} tFHFile;

typedef struct
{
    DIR           * dp;
    struct dirent * entry;
    off_t offset;
} tFHDir;

typedef struct
{
    /* IMPORTANT - leave the union as the first item. Need to be able
     * to cast between tFileHandle, tFHFile, and tFHDir */
    union
    {
        tFHFile file;
        tFHDir  directory;
    };
    enum
    {
        isUnknown = 0, isFile, isDirectory
    } type;

} tFileHandle;

tFHFile * getFileHandle( struct fuse_file_info * fi )
{
    if ( fi == NULL )
        return NULL;
    tFileHandle * fh = (tFileHandle *) fi->fh;
    return (fh != NULL && fh->type == isFile)? &fh->file : NULL;
}

void setFileHandle( struct fuse_file_info * fi, tFHFile * fh )
{
    ((tFileHandle *) fh)->type = isFile;
    fi->fh                     = (uint64_t) fh;
}

tFHDir * getDirHandle( struct fuse_file_info * fi )
{
    if ( fi == NULL )
        return NULL;
    tFileHandle * fh = (tFileHandle *) fi->fh;
    return (fh != NULL && fh->type == isDirectory)? &fh->directory : NULL;
}

void setDirHandle( struct fuse_file_info * fi, tFHDir * fh )
{
    ((tFileHandle *) fh)->type = isDirectory;
    fi->fh                     = (uint64_t) fh;
}

void clearHandle( struct fuse_file_info * fi )
{
    if ((void *) fi->fh != NULL )
    {
        free((void *) fi->fh );
        fi->fh = (uint64_t) NULL;
    }
}

// ------------------------------------------------------------------------------

static inline int isTemplate( char * path )
{
    return ( faccessat( globals.templates.fd, path, R_OK, AT_SYMLINK_NOFOLLOW ) == 0 );
}

// ------------------------------------------------------------------------------
/**
 * Initialize filesystem
 *
 * The return value will passed in the `private_data` field of
 * `struct fuse_context` to all file operations, and as a
 * parameter to the destroy() method. It overrides the initial
 * value provided to fuse_main() / fuse_new().
 */
void * fs_init( struct fuse_conn_info * UNUSED( conn ), struct fuse_config * cfg )
{
    LOG_ON_ENTRY( "(%p,%p)", conn, cfg );

    cfg->use_ino     = 1;
    cfg->nullpath_ok = 1;

    /* Pick up changes from lower filesystem right away. This is also necessary
     * for better hardlink support. When the kernel calls the unlink() handler,
     * it does not know the inode of the to-be-removed entry and therefore can
     * not invalidate the cache of the associated inode - resulting in an
     * incorrect st_nlink value being reported for any remaining hardlinks to
     * this inode. */
    cfg->entry_timeout    = 0;
    cfg->attr_timeout     = 0;
    cfg->negative_timeout = 0;

    return NULL;
}

/** Get file attributes.
 *
 * Similar to stat().  The 'st_dev' and 'st_blksize' fields are
 * ignored. The 'st_ino' field is ignored except if the 'use_ino'
 * mount option is given. In that case it is passed to userspace,
 * but libfuse and the kernel will still assign a different
 * inode for internal use (called the "nodeid").
 *
 * `fi` will always be NULL if the file is not currently open, but
 * may also be NULL if the file is open.
 */
int file_getattr( const char * path, struct stat * stbuf, struct fuse_file_info * fi )
{
    LOG_ON_ENTRY( "(\"%s\", %p, %p)", path, stbuf, fi );

    tFHFile * fh = getFileHandle( fi );

    if ( fh )
    {
        return( (fstat( fh->fd, stbuf ) == -1)? -errno : 0);
    }
    else
    {
        return( (fstatat( globals.mountpoint.fd, &path[1], stbuf, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW ) == -1)? -errno : 0);
    }
}

/**
 * Check file access permissions
 *
 * This will be called for the access() system call.  If the
 * 'default_permissions' mount option is given, this method is not
 * called.
 *
 * This method is not called under Linux kernel versions 2.4.x
 */
int file_access( const char * path, int mask )
{
    LOG_ON_ENTRY( "(\"%s\", %d)", path, mask );

    return( (faccessat( globals.mountpoint.fd, &path[1], mask, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW ) == -1)? -errno : 0);
}

/** Read the target of a symbolic link
 *
 * The buffer should be filled with a null terminated string.  The
 * buffer size argument includes the space for the terminating
 * null character.	If the linkname is too long to fit in the
 * buffer, it should be truncated.	The return value should be 0
 * for success.
 */
int obj_readlink( const char * path, char * buf, size_t size )
{
    int result;

    LOG_ON_ENTRY( "(\"%s\", %p, %d)", path, buf, size );

    result = readlinkat( globals.mountpoint.fd, &path[1], buf, size - 1 );
    if ( result == -1 )
    {
        return -errno;
    }

    buf[result] = '\0';
    return 0;
}

/** Open directory
 *
 * Unless the 'default_permissions' mount option is given,
 * this method should check if opendir is permitted for this
 * directory. Optionally opendir may also return an arbitrary
 * filehandle in the fuse_file_info structure, which will be
 * passed to readdir, releasedir and fsyncdir.
 */
int dir_open( const char * path, struct fuse_file_info * fi )
{
    int fd;

    LOG_ON_ENTRY( "(\"%s\",%p)", path, fi );

    tFHDir * fh = (tFHDir *) malloc( sizeof( tFileHandle ));

    if ( fh == NULL )
    {
        return -ENOMEM;
    }

    if ( strcmp(path,"/") == 0 )
    {
        fd = dup(globals.mountpoint.fd);
        if ( lseek( fd, 0, SEEK_SET ) == -1)
        {
            fuse_log( FUSE_LOG_ERR, "error: lseek failed (%d: %s)", errno, strerror(errno) );
            return -errno;
        }
    }
    else
    {
        fd = openat( globals.mountpoint.fd, &path[1], O_RDONLY );
    }

    if ( fd == -1 )
    {
        fuse_log(FUSE_LOG_ERR, "file descriptor is invalid (%d: %s)", errno, strerror(errno) );
        return -errno;
    }
    else
    {
        fh->dp = fdopendir( fd );
        if ( fh->dp == NULL)
        {
            fuse_log( FUSE_LOG_ERR, "failed to open directory \"%s\"", path );
            int result = -errno;
            free( fh );
            return result;
        }
        fh->offset = 0;
        fh->entry  = NULL;

        setDirHandle( fi, fh );
    }

    return 0;
}

/** Read directory
 *
 * The filesystem may choose between two modes of operation:
 *
 * 1) The readdir implementation ignores the offset parameter, and
 * passes zero to the filler function's offset.  The filler
 * function will not return '1' (unless an error happens), so the
 * whole directory is read in a single readdir operation.
 *
 * 2) The readdir implementation keeps track of the offsets of the
 * directory entries.  It uses the offset parameter and always
 * passes non-zero offset to the filler function.  When the buffer
 * is full (or an error happens) the filler function will return
 * '1'.
 */
int dir_read( const char * UNUSED( path ),
              void * buf,
              fuse_fill_dir_t filler,
              off_t offset,
              struct fuse_file_info * fi,
              enum fuse_readdir_flags flags )
{
    LOG_ON_ENTRY( "(\"%s\",%p)", path, fi );

    tFHDir * fh = getDirHandle( fi );

    if ( fh == NULL )
    {
        return -ENOTDIR;
    }

    if ( offset != fh->offset )
    {
#ifndef __FreeBSD__
        seekdir( fh->dp, offset );
#else
        /* Subtract the one that we add when calling
                   telldir() below */
        seekdir( fh->dp, offset - 1 );
#endif
        fh->entry  = NULL;
        fh->offset = offset;
    }
    while ( 1 )
    {
        struct stat              st;
        off_t                    nextoff;
        enum fuse_fill_dir_flags fill_flags = 0;

        if ( !fh->entry )
        {
            fh->entry = readdir( fh->dp );
            if ( !fh->entry )
            {
                break;
            }
        }
#ifdef HAVE_FSTATAT
        if (flags & FUSE_READDIR_PLUS) {
            int result;

            result = fstatat( dirfd(d->dp), d->entry->d_name, &st, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
            if (result != -1)
            {
                fill_flags |= FUSE_FILL_DIR_PLUS;
            }
        }
#endif
        if ( !(fill_flags & FUSE_FILL_DIR_PLUS))
        {
            memset( &st, 0, sizeof( st ));
            st.st_ino  = fh->entry->d_ino;
            st.st_mode = fh->entry->d_type << 12;
        }
        nextoff = telldir( fh->dp );
#ifdef __FreeBSD__
        /* Under FreeBSD, telldir() may return 0 the first time
                   it is called. But for libfuse, an offset of zero
                   means that offsets are not supported, so we shift
                   everything by one. */
        nextoff++;
#endif
        if ( filler( buf, fh->entry->d_name, &st, nextoff, fill_flags ))
        {
            break;
        }

        fh->entry  = NULL;
        fh->offset = nextoff;
    }

    return 0;
}

/** Release directory */
int dir_release( const char * UNUSED( path ), struct fuse_file_info * fi )
{
    LOG_ON_ENTRY( "(\"%s\",%p)", path, fi );

    tFHDir * fh = getDirHandle( fi );
    if ( fh != NULL && fh->dp != NULL )
    {
        closedir( fh->dp );
        fh->dp = NULL;
    }
    clearHandle( fi );

    return 0;
}

/** Create a file node
 *
 * This is called for creation of all non-directory, non-symlink
 * nodes.  If the filesystem defines a create() method, then for
 * regular files that will be called instead.
 */
int obj_mknod( const char * path, mode_t mode, dev_t rdev )
{
    LOG_ON_ENTRY( "(\"%s\",%d,%d)", path, mode, rdev );

    if ( S_ISFIFO( mode ))
    {
        return( (mkfifoat( globals.mountpoint.fd, &path[1], mode ) == -1)? -errno : 0);
    }
    else
    {
        return( (mknodat( globals.mountpoint.fd, &path[1], mode, rdev ) == -1)? -errno : 0);
    }
}

/** Create a directory
 *
 * Note that the mode argument may not have the type specification
 * bits set, i.e. S_ISDIR(mode) can be false.  To obtain the
 * correct directory type bits use  mode|S_IFDIR
 */
int dir_create( const char * path, mode_t mode )
{
    LOG_ON_ENTRY( "(\"%s\",%d)", path, mode );

    return( (mkdirat( globals.mountpoint.fd, &path[1], mode ) == -1)? -errno : 0);
}

/** Remove a file */
int file_unlink( const char * path )
{
    LOG_ON_ENTRY( "(\"%s\")", path );

    return( (unlinkat( globals.mountpoint.fd, &path[1], 0 ) == -1)? -errno : 0);
}

/** Remove a directory */
int dir_remove( const char * path )
{
    LOG_ON_ENTRY( "(\"%s\")", path );

    return( (unlinkat( globals.mountpoint.fd, &path[1], AT_REMOVEDIR ) == -1)? -errno : 0);
}

/** Create a symbolic link */
int obj_symlink( const char * from, const char * to )
{
    LOG_ON_ENTRY( "(\"%s\",\"%s\")", from, to );

    return( (symlinkat( from, globals.mountpoint.fd, &to[1] ) == -1)? -errno : 0);
}

/** Rename a file
 *
 * *flags* may be `RENAME_EXCHANGE` or `RENAME_NOREPLACE`. If
 * RENAME_NOREPLACE is specified, the filesystem must not
 * overwrite *newname* if it exists and return an error
 * instead. If `RENAME_EXCHANGE` is specified, the filesystem
 * must atomically exchange the two files, i.e. both must
 * exist and neither may be deleted.
 */
int obj_rename( const char * from, const char * to, unsigned int flags )
{
    LOG_ON_ENTRY( "(\"%s\",\"%s\",%u)", from, to, flags );

    /* When we have renameat2() in libc, then we can implement flags */
    if ( flags )
    {
        return -EINVAL;
    }

    return( (renameat( globals.mountpoint.fd, &from[1], globals.mountpoint.fd, &to[1] ) == -1)? -errno : 0);
}

/** Create a hard link to a file */
int file_link( const char * from, const char * to )
{
    /* ToDo: fixup from and to */
    LOG_ON_ENTRY( "(\"%s\",\"%s\")", from, to );

    return( (linkat( globals.mountpoint.fd, &from[1], globals.mountpoint.fd, &to[1], 0 ) == -1)? -errno : 0);
}

/** Change the permission bits of a file
 *
 * `fi` will always be NULL if the file is not currenlty open, but
 * may also be NULL if the file is open.
 */
int obj_chmod( const char * path, mode_t mode, struct fuse_file_info * fi )
{
    LOG_ON_ENTRY( "(\"%s\",%p,%p)", path, mode, fi );

    tFHFile * fh = getFileHandle( fi );
    if ( fh )
    {
        return( (fchmod( fh->fd, mode ) == -1)? -errno : 0);
    }
    else
    {
        return( (fchmodat( globals.mountpoint.fd, &path[1], mode, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW ) == -1)? -errno : 0);
    }
}

/** Change the owner and group of a file
 *
 * `fi` will always be NULL if the file is not currenlty open, but
 * may also be NULL if the file is open.
 *
 * Unless FUSE_CAP_HANDLE_KILLPRIV is disabled, this method is
 * expected to reset the setuid and setgid bits.
 */
int obj_chown( const char * path, uid_t uid, gid_t gid, struct fuse_file_info * fi )
{
    LOG_ON_ENTRY( "(\"%s\",%u,%u,%p)", path, uid, gid, fi );

    tFHFile * fh = getFileHandle( fi );
    if ( fh )
    {
        return( (fchown( fh->fd, uid, gid ) == -1)? -errno : 0);
    }
    else
    {
        return( (fchownat( globals.mountpoint.fd, &path[1], uid, gid, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW ) == -1)? -errno : 0);
    }
}

/** Change the size of a file
 *
 * `fi` will always be NULL if the file is not currenlty open, but
 * may also be NULL if the file is open.
 *
 * Unless FUSE_CAP_HANDLE_KILLPRIV is disabled, this method is
 * expected to reset the setuid and setgid bits.
 */
int file_truncate( const char * path, off_t size,
                   struct fuse_file_info * fi )
{
    LOG_ON_ENTRY( "(\"%s\",%u,%p)", path, size, fi );

    if ( fi == NULL )
    {
        fuse_log( FUSE_LOG_ERR, "attempt to truncate \"%s\" with null fuse_file_info", path);
        return -EINVAL;
    }
    else
    {
        tFHFile * fh = getFileHandle( fi );
        if (fh == NULL)
        {
            fuse_log( FUSE_LOG_ERR, "attempt to truncate \"%s\" with invalid fileHandle", path );
            return -EINVAL;
        }

        return ((ftruncate( fh->fd, size ) == -1)? -errno : 0);
    }
}

#ifdef HAVE_UTIMENSAT
/**
 * Change the access and modification times of a file with
 * nanosecond resolution
 *
 * This supersedes the old utime() interface.  New applications
 * should use this.
 *
 * `fi` will always be NULL if the file is not currenlty open, but
 * may also be NULL if the file is open.
 *
 * See the utimensat(2) man page for details.
 */
int file_utimens( const char *path,
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
        return( (utimensat(0, path, ts, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW) == -1)? -errno : 0);
    }
}
#endif

/**
 * Create and open a file
 *
 * If the file does not exist, first create it with the specified
 * mode, and then open it.
 *
 * If this method is not implemented or under Linux kernel
 * versions earlier than 2.6.15, the mknod() and open() methods
 * will be called instead.
 */
int file_create( const char * path, mode_t mode, struct fuse_file_info * fi )
{
    LOG_ON_ENTRY( "(\"%s\",%u,%p)", path, mode, fi );

    tFHFile * fh = (tFHFile *) calloc( 1, sizeof( tFileHandle ));

    if ( fh == NULL )
    {
        return -ENOMEM;
    }

    int fd = openat( globals.mountpoint.fd, &path[1], fi->flags, mode );
    if ( fd == -1 )
    {
        return -errno;
    }

    fh->fd = fd;
    setFileHandle( fi, fh );

    return 0;
}

/** Open a file
 *
 * Open flags are available in fi->flags. The following rules apply:
 *
 *  - Creation (O_CREAT, O_EXCL, O_NOCTTY) flags will be
 *    filtered out / handled by the kernel.
 *
 *  - Access modes (O_RDONLY, O_WRONLY, O_RDWR, O_EXEC, O_SEARCH)
 *    should be used by the filesystem to check if the operation is
 *    permitted.  If the ``-o default_permissions`` mount option is
 *    given, this check is already done by the kernel before calling
 *    open() and may thus be omitted by the filesystem.
 *
 *  - When writeback caching is enabled, the kernel may send
 *    read requests even for files opened with O_WRONLY. The
 *    filesystem should be prepared to handle this.
 *
 *  - When writeback caching is disabled, the filesystem is
 *    expected to properly handle the O_APPEND flag and ensure
 *    that each write is appending to the end of the file.
 *
 *  - When writeback caching is enabled, the kernel will
 *    handle O_APPEND. However, unless all changes to the file
 *    come through the kernel this will not work reliably. The
 *    filesystem should thus either ignore the O_APPEND flag
 *    (and let the kernel handle it), or return an error
 *    (indicating that reliably O_APPEND is not available).
 *
 * Filesystem may store an arbitrary file handle (pointer, index, etc) in fi->fh, and
 * use this in other all other file operations (read, write, flush, release, fsync).
 *
 * Filesystem may also implement stateless file I/O and not store anything in fi->fh.
 *
 * There are also some flags (direct_io, keep_cache) which the filesystem may set in fi,
 * to change the way the file is opened. See fuse_file_info structure in <fuse_common.h>
 * for more details.
 *
 * If this request is answered with an error code of ENOSYS and FUSE_CAP_NO_OPEN_SUPPORT
 * is set in  `fuse_conn_info.capable`, this is treated as success and future calls to
 * open will also succeed without being send to the filesystem process.
 *
 */
int file_open( const char * path, struct fuse_file_info * fi )
{
    int  fd;

    LOG_ON_ENTRY( "(\"%s\", %p)", path, fi );

    tFHFile * fh = (tFHFile *) calloc( 1, sizeof( tFileHandle ));
    if ( fh == NULL )
    {
        return -ENOMEM;
    }

    fh->isTemplate = isTemplate( path );

    if ( fh->isTemplate )
    {
        fuse_log( FUSE_LOG_DEBUG,"opening template \"%s\"", path );
        fd = openat( globals.templates.fd, &path[1], fi->flags );

        /* ToDo: process template file */
    }
    else
    {
        fuse_log( FUSE_LOG_DEBUG, "opening file \"%s\"", path );
        fd = openat( globals.mountpoint.fd, &path[1], fi->flags );
    }
    if ( fd == -1 )
    {
        return -errno;
    }

    fh->fd = fd;
    setFileHandle( fi, fh );
    return 0;
}

/** Read data from an open file
 *
 * Read should return exactly the number of bytes requested except
 * on EOF or error, otherwise the rest of the data will be
 * substituted with zeroes.	 An exception to this is when the
 * 'direct_io' mount option is specified, in which case the return
 * value of the read system call will reflect the return value of
 * this operation.
 */
int file_read( const char * UNUSED( path ),
               char * buf,
               size_t size,
               off_t offset,
               struct fuse_file_info * fi )
{
    LOG_ON_ENTRY( "(\"%s\", %p)", path, fi );

    tFHFile * fh = getFileHandle( fi );
    if ( fh == NULL )
    {
        return -ENFILE;
    }

    int result = pread( fh->fd, buf, size, offset );
    return( (result == -1)? -errno : result);
}

/** Store data from an open file in a buffer
 *
 * Similar to the read() method, but data is stored and
 * returned in a generic buffer.
 *
 * No actual copying of data has to take place, the source
 * file descriptor may simply be stored in the buffer for
 * later data transfer.
 *
 * The buffer must be allocated dynamically and stored at the
 * location pointed to by bufp.  If the buffer contains memory
 * regions, they too must be allocated using malloc().  The
 * allocated memory will be freed by the caller.
 */
int file_read_buf( const char * UNUSED( path ),
                   struct fuse_bufvec ** bufp,
                   size_t size,
                   off_t offset,
                   struct fuse_file_info * fi )
{
    struct fuse_bufvec * src;

    LOG_ON_ENTRY( "(\"%s\", %p)", path, fi );

    tFHFile * fh = getFileHandle( fi );
    if ( fh == NULL )
    {
        return -ENFILE;
    }

    src = (struct fuse_bufvec *) calloc( 1, sizeof( struct fuse_bufvec ));
    if ( src == NULL )
    {
        return -ENOMEM;
    }

    *src = FUSE_BUFVEC_INIT( size );

    src->buf[0].flags = (FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK);
    src->buf[0].fd    = fh->fd;
    src->buf[0].pos   = offset;

    *bufp = src;

    return 0;
}

/** Write data to an open file
 *
 * Write should return exactly the number of bytes requested
 * except on error.	 An exception to this is when the 'direct_io'
 * mount option is specified (see read operation).
 *
 * Unless FUSE_CAP_HANDLE_KILLPRIV is disabled, this method is
 * expected to reset the setuid and setgid bits.
 */
int file_write( const char * UNUSED( path ), const char * buf, size_t size,
                off_t offset, struct fuse_file_info * fi )
{
    int result;

    LOG_ON_ENTRY( "(\"%s\",%p,%u,%u,%p)", path, buf, size, offset, fi );

    tFHFile * fh = getFileHandle( fi );
    if ( fh == NULL )
    {
        return -ENFILE;
    }

    result = pwrite( fh->fd, buf, size, offset );
    if ( result == -1 )
    {
        result = -errno;
    }

    return result;
}

/** Write contents of buffer to an open file
 *
 * Similar to the write() method, but data is supplied in a
 * generic buffer.  Use fuse_buf_copy() to transfer data to
 * the destination.
 *
 * Unless FUSE_CAP_HANDLE_KILLPRIV is disabled, this method is
 * expected to reset the setuid and setgid bits.
 */
int file_write_buf( const char * UNUSED( path ), struct fuse_bufvec * buf, off_t offset, struct fuse_file_info * fi )
{
    struct fuse_bufvec dst = FUSE_BUFVEC_INIT( fuse_buf_size( buf ));

    LOG_ON_ENTRY( "(\"%s\",%p,%u,%p)", path, buf, offset, fi );

    tFHFile * fh = getFileHandle( fi );
    if ( fh == NULL )
    {
        return -ENFILE;
    }

    dst.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
    dst.buf[0].fd    = fh->fd;
    dst.buf[0].pos   = offset;

    return fuse_buf_copy( &dst, buf, FUSE_BUF_SPLICE_NONBLOCK );
}

/** Get file system statistics
 *
 * The 'f_favail', 'f_fsid' and 'f_flag' fields are ignored
 */
int obj_statfs( const char * path, struct statvfs * stbuf )
{
    LOG_ON_ENTRY( "(\"%s\",%p)", path, stbuf );

    return( (statvfs( path, stbuf ) == -1)? -errno : 0);
}

/** Possibly flush cached data
 *
 * BIG NOTE: This is not equivalent to fsync().  It's not a
 * request to sync dirty data.
 *
 * Flush is called on each close() of a file descriptor, as opposed to
 * release which is called on the close of the last file descriptor for
 * a file.  Under Linux, errors returned by flush() will be passed to
 * userspace as errors from close(), so flush() is a good place to write
 * back any cached dirty data. However, many applications ignore errors
 * on close(), and on non-Linux systems, close() may succeed even if flush()
 * returns an error. For these reasons, filesystems should not assume
 * that errors returned by flush will ever be noticed or even
 * delivered.
 *
 * NOTE: The flush() method may be called more than once for each
 * open().  This happens if more than one file descriptor refers to an
 * open file handle, e.g. due to dup(), dup2() or fork() calls.  It is
 * not possible to determine if a flush is final, so each flush should
 * be treated equally.  Multiple write-flush sequences are relatively
 * rare, so this shouldn't be a problem.
 *
 * Filesystems shouldn't assume that flush will be called at any
 * particular point.  It may be called more times than expected, or not
 * at all.
 *
 * [close]: http://pubs.opengroup.org/onlinepubs/9699919799/functions/close.html
 */
int file_flush( const char * UNUSED( path ), struct fuse_file_info * fi )
{
    LOG_ON_ENTRY( "(\"%s\",%p)", path, fi );

    tFHFile * fh = getFileHandle( fi );
    if ( fh == NULL )
    {
        return -ENFILE;
    }

    /* This is called from every close on an open file, so call the close on the
     * underlying filesystem. But since flush may be called multiple times for
     * an open file, this must not really close the file.  This is important if
     * used on a network filesystem like NFS which flush the data/metadata on
     * close() */
    return( (close( dup( fh->fd )) == -1)? -errno : 0);
}

/** Release an open file
 *
 * Release is called when there are no more references to an open
 * file: all file descriptors are closed and all memory mappings
 * are unmapped.
 *
 * For every open() call there will be exactly one release() call
 * with the same flags and file handle.  It is possible to
 * have a file opened more than once, in which case only the last
 * release will mean, that no more reads/writes will happen on the
 * file.  The return value of release is ignored.
 */
int file_release( const char * UNUSED( path ), struct fuse_file_info * fi )
{
    LOG_ON_ENTRY( "(\"%s\",%p)", path, fi );

    tFHFile * fh = getFileHandle( fi );
    if ( fh == NULL )
    {
        return -ENFILE;
    }

    close( fh->fd );
    clearHandle( fi );

    return 0;
}

/** Synchronize file contents
 *
 * If the datasync parameter is non-zero, then only the user data
 * should be flushed, not the meta data.
 */
int file_fsync( const char * UNUSED( path ), int UNUSED( isdatasync ), struct fuse_file_info * fi )
{
    int result;

    LOG_ON_ENTRY( "(\"%s\",%u,%p)", path, isdatasync, fi );

    tFHFile * fh = getFileHandle( fi );
    if ( fh == NULL )
    {
        return -ENFILE;
    }

#ifdef HAVE_FDATASYNC
    if (isdatasync)
    {
        result = fdatasync(fh->fd);
    }
    else
#endif
    {
        result = fsync( fh->fd );
    }

    return( (result == -1)? -errno : 0);
}

#ifdef HAVE_POSIX_FALLOCATE
/**
 * Allocates space for an open file
 *
 * This function ensures that required space is allocated for specified
 * file.  If this function returns success then any subsequent write
 * request to specified range is guaranteed not to fail because of lack
 * of space on the file system media.
 */
int file_fallocate( const char *UNUSED(path),
                         int mode,
                         off_t offset,
                         off_t length,
                         struct fuse_file_info *fi )
{
    if (mode)
    {
        return -EOPNOTSUPP;
    }

    tFHFile * fh = getFileHandle( fi );
    if ( fh == NULL)
    {
        return -ENFILE;
    }

    return -posix_fallocate(fh->fd, offset, length);
}
#endif

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */

/** Set extended attributes */
int file_setxattr( const char *path,
                        const char *name,
                        const char *value,
                        size_t size,
                        int flags)
{
    return( (lsetxattr(path, name, value, size, flags) == -1)? -errno : 0);
}

/** Get extended attributes */
int file_getxattr( const char *path,
                        const char *name,
                        char *value,
                        size_t size)
{
    return( (lgetxattr(path, name, value, size) == -1)? -errno : 0);
}

/** List extended attributes */
int file_listxattr(const char *path, char *list, size_t size)
{
    return( (llistxattr(path, list, size) == -1)? -errno : 0);
}

/** Remove extended attributes */
int file_removexattr(const char *path, const char *name)
{
    return( (lremovexattr(path, name) == -1)? -errno : 0);
}
#endif /* HAVE_SETXATTR */

#ifdef HAVE_LIBULOCKMGR
/**
 * Perform POSIX file locking operation
 *
 * The cmd argument will be either F_GETLK, F_SETLK or F_SETLKW.
 *
 * For the meaning of fields in 'struct flock' see the man page
 * for fcntl(2).  The l_whence field will always be set to
 * SEEK_SET.
 *
 * For checking lock ownership, the 'fuse_file_info->owner'
 * argument must be used.
 *
 * For F_GETLK operation, the library will first check currently
 * held locks, and if a conflicting lock is found it will return
 * information without calling this method.	 This ensures, that
 * for local locks the l_pid field is correctly filled in.	The
 * results may not be accurate in case of race conditions and in
 * the presence of hard links, but it's unlikely that an
 * application would rely on accurate GETLK results in these
 * cases.  If a conflicting lock is not found, this method will be
 * called, and the filesystem may fill out l_pid by a meaningful
 * value, or it may leave this field zero.
 *
 * For F_SETLK and F_SETLKW the l_pid field will be set to the pid
 * of the process performing the locking operation.
 *
 * Note: if this method is not implemented, the kernel will still
 * allow file locking to work locally.  Hence it is only
 * interesting for network filesystems and similar.
 */
int file_lock(const char *UNUSED(path), struct fuse_file_info *fi, int cmd, struct flock *lock)
{
    tFHFile * fh = getFileHandle( fi );
    if ( fh == NULL)
    {
        return -ENFILE;
    }

    return ulockmgr_op(fh->fd, cmd, lock, &fi->lock_owner, sizeof(fi->lock_owner));
}
#endif

/**
 * Perform BSD file locking operation
 *
 * The op argument will be either LOCK_SH, LOCK_EX or LOCK_UN
 *
 * Nonblocking requests will be indicated by ORing LOCK_NB to
 * the above operations
 *
 * For more information see the flock(2) manual page.
 *
 * Additionally fi->owner will be set to a value unique to
 * this open file.  This same value will be supplied to
 * ->release() when the file is released.
 *
 * Note: if this method is not implemented, the kernel will still
 * allow file locking to work locally.  Hence it is only
 * interesting for network filesystems and similar.
 */
int file_flock( const char * UNUSED( path ), struct fuse_file_info * fi, int op )
{
    LOG_ON_ENTRY( "(\"%s\",%p,%u)", path, fi, op );

    tFHFile * fh = getFileHandle( fi );
    if ( fh == NULL )
    {
        return -ENFILE;
    }

    return ( (flock( fh->fd, op ) == -1)? -errno : 0);
}

#ifdef HAVE_COPY_FILE_RANGE
/**
 * Copy a range of data from one file to another
 *
 * Performs an optimized copy between two file descriptors without the
 * additional cost of transferring data through the FUSE kernel module
 * to user space (glibc) and then back into the FUSE filesystem again.
 *
 * In case this method is not implemented, applications are expected to
 * fall back to a regular file copy.   (Some glibc versions did this
 * emulation automatically, but the emulation has been removed from all
 * glibc release branches.)
 */
ssize_t file_copy_range( const char * UNUSED(path_in),
                         struct fuse_file_info *fi_in, off_t off_in,
                         const char * UNUSED(path_out),
                         struct fuse_file_info *fi_out, off_t off_out,
                         size_t len, int flags)
{
    ssize_t result;
    tFHFile * fh_in = getFileHandle( fi_in );
    if ( fh_in == NULL)
    {
        return -ENFILE;
    }
    tFHFile * fh_out = getFileHandle( fi_out );
    if ( fh_out == NULL)
    {
        return -ENFILE;
    }


    result = copy_file_range(fh_in->fd, &off_in, fh_out->fd, &off_out, len, flags);
    return ( (result == -1)? -errno : 0);
}
#endif

/** Find next data or hole after the specified offset */
off_t file_lseek( const char * UNUSED( path ), off_t off, int whence, struct fuse_file_info * fi )
{
    off_t result;

    LOG_ON_ENTRY( "(\"%s\",%u,%u,%p)", path, off, whence, fi );

    tFHFile * fh = getFileHandle( fi );
    if ( fh == NULL )
    {
        return -ENFILE;
    }

    result = lseek( fh->fd, off, whence );

    return ( (result == -1)? -errno : result);
}

const struct fuse_operations templatefsOperations = {
        .init            = fs_init,
        .getattr         = file_getattr,
        .access          = file_access,
        .readlink        = obj_readlink,
        .opendir         = dir_open,
        .readdir         = dir_read,
        .releasedir      = dir_release,
        .mknod           = obj_mknod,
        .mkdir           = dir_create,
        .symlink         = obj_symlink,
        .unlink          = file_unlink,
        .rmdir           = dir_remove,
        .rename          = obj_rename,
        .link            = file_link,
        .chmod           = obj_chmod,
        .chown           = obj_chown,
        .truncate        = file_truncate,
#ifdef HAVE_UTIMENSAT
        .utimens         = file_utimens,
#endif
        .create          = file_create,
        .open            = file_open,
        .read            = file_read,
        .read_buf        = file_read_buf,
        .write           = file_write,
        .write_buf       = file_write_buf,
        .statfs          = obj_statfs,
        .flush           = file_flush,
        .release         = file_release,
        .fsync           = file_fsync,
#ifdef HAVE_POSIX_FALLOCATE
        .fallocate       = file_fallocate,
#endif
#ifdef HAVE_SETXATTR
        .setxattr        = file_setxattr,
        .getxattr        = file_getxattr,
        .listxattr       = file_listxattr,
        .removexattr     = file_removexattr,
#endif
#ifdef HAVE_LIBULOCKMGR
        .lock            = file_lock,
#endif
        .flock           = file_flock,
#ifdef HAVE_COPY_FILE_RANGE
        .copy_file_range = file_copy_range,
#endif
        .lseek           = file_lseek,
};
