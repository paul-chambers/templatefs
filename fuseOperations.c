//
// Created by paul on 5/27/20.
//

#include "common.h"
#include "templatefs.h"
#include "logStuff.h"

#include <stdbool.h>
#include <sys/file.h>   /* flock(2) */
#include <sys/epoll.h>
#include <sys/wait.h>

#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#include <fuse3/fuse.h>

#include "fuseOperations.h"
#include "processTemplate.h"

typedef struct {
    char * path;
    DIR  * dir;
    int    fd;
} tFSTree;

typedef struct {
    tFSTree  mountpoint;    ///< absolute path to the mount point
    tFSTree  templates;     ///< absolute path to the top of the template hierarchy
} tPrivateData;

typedef struct {
    const char *  path;          ///< absolute path to the file
    int           fd;            ///< file descriptor. -1 if fd not open/valid */
    bool          isTemplate;    ///< true if there's a template file to process, else pass request through
    bool          isExecutable;  ///< true if the templatee file is _executable_

    byte *        contents;      ///< the cached result of processing the template file
    size_t        length;        ///< length of the cached file contents
} tFHFile;

typedef struct {
    DIR *            dp;
    struct dirent *  entry;
    off_t            offset;
} tFHDir;

typedef struct {
    /* * * UNION MUST BE FIRST * * */
    /* tFHHandle. tFHFile, and tFHDir are cast back and
     * forth, based on the value of the type field */
    union {
        tFHFile file;
        tFHDir  directory;
    };

    /* how the union should be accessed */
    enum {
        notInitialized = 0,  ///< hasn't been set yet
        isFile,              ///< use the 'file' field of the union
        isDirectory          ///< use the 'directory' field of the union
    } type;                  ///< indicates which field of the union to use

} tFileHandle;

/**
 * @brief a buffer that expands as it fills
 * @see newElasticBuffer()
 * @see releaseElasticBuffer()
 * @see makeRoom()
 * @see getSpacePtr()
 * @see increaseAvailable()
 */
typedef struct {
    char *   data;       ///< pointer to the data, caution: may move when realloc'd
    ssize_t  available;  ///< amount of data currently in the buffer
    ssize_t  remaining;  ///< amount of unused space lwft
    ssize_t  headroom;   ///< the amount of 'space' that's added
} tElasticBuffer;

// ------------------------------------------------------------------------------

/**
 * @brief allocate a new, empty buffer
 * @param size      initial size of buffer
 * @param headroom  amount to ensure we have ready to fill
 * @return the buffer structure, or NULL if failed.
 */
tElasticBuffer * newElasticBuffer( size_t size, size_t headroom )
{
    tElasticBuffer * result = calloc(1, sizeof(tElasticBuffer));
    if ( result != NULL ) {
        result->data      = calloc( 1, size);
        result->available = 0;
        if ( result->data != NULL ) {
            result->remaining = size;
        }
        result->headroom = headroom;
    }
    return result;
}

/**
 * @brief release the resources in use by the elastic buffer
 * @param buf the elastic buffer
 */
void releaseElasticBuffer( tElasticBuffer * buf )
{
    free( buf->data );
    free( buf );
}

/**
 * @brief ensure that there's at least 'headroom' bytes available in the buffer
 *
 * Caution: calling realloc() may move the data buffer.
 *
 * @param buf  the elastic buffer to check/expand
 * @return the number of bytes now available
 */
static size_t makeRoom( tElasticBuffer * buf )
{
    if ( buf->remaining < buf->headroom ) {
        buf->remaining += buf->headroom * 2;
        buf->data       = realloc( buf->data, buf->available + buf->remaining );
    }
    return buf->remaining;
}

/**
 * @brief
 * @param buf
 * @return
 */
char * getSpacePtr( tElasticBuffer * buf )
{
    /* Caution: if makeRoom causes a realloc, make sure it
     * happens *before* doing any pointer arithmetic */
    makeRoom( buf );
    return buf->data + buf->available;
}

/**
 * @brief increase
 * @param buf
 * @param additional
 */
void increaseAvailable( tElasticBuffer * buf, size_t additional )
{
    buf->available += additional;
    buf->remaining -= additional;
    makeRoom(buf);
}

/**
 * @brief substitute -errno if result == -1
 *
 * In a nutshell, most linux filesystem functions invariably return -1 on error,
 * and return the actual error that occurred as a positive integer in 'errno'.
 * libfuse adopts the (much saner) convention of returning -errno rather than -1
 * when an error is being reported.
 *
 * This convention crops up throughout operation functions. So define it
 * as a common inline function.
 *
 * @param result the result of a 'standard' filesystem function.
 * @return if result == -1, then return -errno, otherwise just pass back result unmodified.
 */

static inline int fixupResult( int result )
{
    if ( result == -1 )
    {
        result = -errno;
    }
    return result;
}

// ------------------------------------------------------------------------------

void setHandle( struct fuse_file_info * fi, tFileHandle * fh )
{
    if ( fi != NULL ) {
        fi->fh = (uint64_t) fh;
    }
}

tFHFile * getFileHandle( struct fuse_file_info * fi )
{
    tFileHandle * fh = NULL;
    if ( fi != NULL ) {
        fh = (tFileHandle *) fi->fh;
    }
    return ( fh != NULL && fh->type == isFile ) ? &fh->file : NULL;
}

void setFileHandle( struct fuse_file_info * fi, tFHFile * fileHandle )
{
    if ( fileHandle != NULL ) {
        tFileHandle * fh =  (tFileHandle *)fileHandle;
        fh->type = isFile;
        setHandle( fi, fh );
    }
}

tFHDir * getDirHandle( struct fuse_file_info * fi )
{
    tFileHandle * fh = NULL;
    if ( fi != NULL ) {
        fh = (tFileHandle *) fi->fh;
    }
    return ( fh != NULL && fh->type == isDirectory ) ? &fh->directory : NULL;
}

void setDirHandle( struct fuse_file_info * fi, tFHDir * dirHandle )
{
    if ( dirHandle != NULL ) {
        tFileHandle * fh = (tFileHandle *)dirHandle;
        fh->type = isDirectory;
        setHandle( fi, fh );
    }
}

void releaseHandle( struct fuse_file_info * fi )
{
    if ( (void *)fi->fh != NULL ) {
        free( (void *)fi->fh );
        fi->fh = (uint64_t) NULL;
    }
}

// ------------------------------------------------------------------------------
/**
 * @brief retrieve the tPrivateData structure we stashed in the fuse context earlier.
 * See also initPrivateData()
 * @return pointer to the private data structure, or NULL on error.
 */
tPrivateData * getPrivateData( void )
{
    tPrivateData * result = NULL;

    struct fuse_context * fc = fuse_get_context();
    if ( fc != NULL ) {
        result = fc->private_data;
    }

    return result;
}

int getMountpointFD( void )
{
    int result = -1;

    tPrivateData * privateData = getPrivateData();
    if ( privateData != NULL )
    {
        result = privateData->mountpoint.fd;
    }

    return result;
}

int getTemplateFD( void )
{
    int result = -1;

    tPrivateData * privateData = getPrivateData();
    if ( privateData != NULL )
    {
        result = privateData->templates.fd;
    }

    return result;
}

static inline bool hasTemplate( const char * path )
{
    bool result = ( faccessat( getTemplateFD(),
                               &path[1],
                               R_OK,
                               AT_SYMLINK_NOFOLLOW ) == 0 );
    errno = 0;
    return ( result );
}

static inline bool isExecutable( const char * path )
{
    bool result = ( faccessat( getTemplateFD(),
                               &path[1],
                               X_OK,
                               AT_SYMLINK_NOFOLLOW ) == 0 );
    errno = 0;
    return ( result );
}

int setupFSTree( tFSTree * tree, const char * path )
{
    int result = 0;

    tree->path = realpath( path, NULL );
    if ( tree->path == NULL || access( tree->path, F_OK ) != 0 ) {
        logCritical( "fatal: path \'%s\' is invalid", tree->path );
        result = -errno;
    } else {
        tree->fd  = -1;
        tree->dir = opendir( tree->path );
        if ( tree->dir != NULL ) {
            tree->fd = dirfd( tree->dir );
        }
        if ( tree->fd == -1 ) {
            result = -errno;
        }
    }

    return result;
}


void * initPrivateData( const char * mountPath, const char * templatePath )
{
    logEntry( "\'%s\',\'%s\'", mountPath, templatePath );

    tPrivateData * result = calloc( 1, sizeof( tPrivateData ) );
    if ( result != NULL ) {
        setupFSTree( &result->mountpoint, mountPath );
        setupFSTree( &result->templates, templatePath );
    }

    return (void *) result;
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
void * initFsOp( struct fuse_conn_info * UNUSED( conn ), struct fuse_config * cfg )
{
    logEntry( "%p,%p", conn, cfg );

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

    /* Note: we've already set up private data, so pass that back, or it'll be lost */
    return (void *)getPrivateData();
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
int getFileAttrOp( const char * path, struct stat * stbuf, struct fuse_file_info * fi )
{
    int result;
    logEntry( "\'%s\', %p, %p", path, stbuf, fi );

    tFHFile * fh = getFileHandle( fi );

    bool isTemplate;

    /* ToDo: handle templates better - particularly timestamps */

    if ( fh == NULL ) {
        isTemplate = hasTemplate( path );
    } else {
        isTemplate = fh->isTemplate;
    }

    if ( fh == NULL ) {
        int rootFD;

        if ( isTemplate ) {
            rootFD = getTemplateFD();
        } else {
            rootFD = getMountpointFD();
        }

        result = fixupResult( fstatat( rootFD,
                                             &path[ 1 ],
                                             stbuf,
                                             AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW ) );
    } else {
        result = fixupResult( fstat( fh->fd, stbuf ) );
    }

    if ( isTemplate ) {

        /* if it's a template, report it as read-only/not executable */
        mode_t mask = S_IWUSR | S_IWGRP | S_IWOTH;
        if ( !S_ISDIR( stbuf->st_mode ) ) {
            /* if it's not a directory, clear the exec bits too */
            mask = mask | S_IXUSR | S_IXGRP | S_IXOTH;
        }
        stbuf->st_mode = stbuf->st_mode & ~mask;

        /* return the length of the cached contents */
        if ( fh != NULL && fh->contents != NULL ) {
            stbuf->st_size = fh->length;
        }
    }

    return result;
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
int fileAccessOp( const char * path, int mask )
{
    logEntry( "\'%s\', %d", path, mask );

    return fixupResult( faccessat( getMountpointFD(),
                                         &path[ 1 ],
                                         mask,
                                         AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW ) );
}

/** Read the target of a symbolic link
 *
 * The data should be filled with a null terminated string.  The
 * data size argument includes the space for the terminating
 * null character.	If the linkname is too long to fit in the
 * data, it should be truncated.	The return value should be 0
 * for success.
 */
int readSymlinkOp( const char * path, char * buf, size_t size )
{
    int result;

    logEntry( "\'%s\', %p, %d", path, buf, size );

    result = fixupResult( readlinkat( getMountpointFD(),
                                      &path[ 1 ],
                                      buf,
                                      size - 1 ) );
    if ( result >= 0 ) {
        buf[ result ] = '\0';
        result = 0;
    }

    return result;
}

/** Open directory
 *
 * Unless the 'default_permissions' mount option is given,
 * this method should check if opendir is permitted for this
 * directory. Optionally opendir may also return an arbitrary
 * filehandle in the fuse_file_info structure, which will be
 * passed to readdir, releasedir and fsyncdir.
 */
int openDirOp( const char * path, struct fuse_file_info * fi )
{
    int fd;

    logEntry( "\'%s\',%p", path, fi );

    tFHDir * dh = (tFHDir *) malloc( sizeof( tFileHandle ) );

    if ( dh == NULL ) {
        return -ENOMEM;
    }

    if ( strcmp( path, "/" ) == 0 ) {
        fd = dup( getMountpointFD() );
        if ( lseek( fd, 0, SEEK_SET ) == -1 ) {
            fuse_log( FUSE_LOG_ERR, "error: lseek failed (%d: %s)", errno, strerror( errno ) );
            return -errno;
        }
    } else {
        fd = openat( getMountpointFD(), &path[ 1 ], O_RDONLY );
    }

    if ( fd == -1 ) {
        fuse_log( FUSE_LOG_ERR, "file descriptor is invalid (%d: %s)", errno,
                  strerror( errno ) );
        return -errno;
    } else {
        dh->dp = fdopendir( fd );
        if ( dh->dp == NULL ) {
            fuse_log( FUSE_LOG_ERR, "failed to open directory \'%s\'", path );
            free( dh );
            return -errno;;
        }
        dh->offset = 0;
        dh->entry  = NULL;

        setDirHandle( fi, dh );
    }

    return 0;
}

/** Read directory
 *
 * @brief return the contents of the directory pointed to by path
 *
 * A FUSEs filesystem may choose between two implementations for readdir.
 * This implementation keeps track of the offsets of the directory
 * entries.  It uses the offset parameter and always  passes non-zero
 * offset to the filler function.  When the data is full (or an error
 * happens) the filler function will return '1'.
 *
 * @param path
 * @param buf
 * @param filler
 * @param offset
 * @param fi
 * @param flags
 * @return
*/
int readDirOp( const char * UNUSED( path ),
               void * buf,
               fuse_fill_dir_t filler,
               off_t offset,
               struct fuse_file_info * fi,
               enum fuse_readdir_flags flags )
{
    (void)flags;

    /* ToDo: merge entries from the mount point and corresponding template directory
     * Otherwise a template file that does not have a corresponding file under the
     * mountpoint will never be visible */
    logEntry( "\'%s\',%p", path, fi );

    tFHDir * dh = getDirHandle( fi );

    if ( dh == NULL ) {
        return -ENOTDIR;
    }

    if ( offset != dh->offset ) {
#ifndef __FreeBSD__
        seekdir( dh->dp, offset );
#else
        /* Subtract the one that we add when calling telldir() below */
        seekdir( dh->dp, offset - 1 );
#endif
        dh->entry  = NULL;
        dh->offset = offset;
    }
    do {
        struct stat              st;
        off_t                    nextoff;
        enum fuse_fill_dir_flags fill_flags = 0;

        if ( !dh->entry ) {
            dh->entry = readdir( dh->dp );
            if ( !dh->entry ) {
                break;
            }
        }
#ifdef HAVE_FSTATAT
        if (flags & FUSE_READDIR_PLUS) {
            int result;

            result = fstatat( dirfd(d->dp),
                              d->entry->d_name,
                              &st,
                              AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
            if (result != -1)
            {
                fill_flags |= FUSE_FILL_DIR_PLUS;
            }
        }
#endif
        if ( !( fill_flags & FUSE_FILL_DIR_PLUS ) ) {
            memset( &st, 0, sizeof( st ) );
            st.st_ino  = dh->entry->d_ino;
            st.st_mode = dh->entry->d_type << 12;
        }
        nextoff = telldir( dh->dp );
#ifdef __FreeBSD__
        /* Under FreeBSD, telldir() may return 0 the first time it is called.
         * But for libfuse, an offset of zero means that offsets are not
         * supported, so we shift everything by one. */
        nextoff++;
#endif
        if ( filler( buf,
                     dh->entry->d_name,
                     &st,
                     nextoff,
                     fill_flags ) ) {
            break;
        }

        dh->entry  = NULL;
        dh->offset = nextoff;

    } while ( 1 );

    return 0;
}

/**
 * @brief Release directory
 * @param path
 * @param fi
 * @return
 */
int releaseDirOp( const char * UNUSED( path ), struct fuse_file_info * fi )
{
    logEntry( "\'%s\',%p", path, fi );

    tFHDir * dh = getDirHandle( fi );
    if ( dh != NULL && dh->dp != NULL ) {
        closedir( dh->dp );
        dh->dp = NULL;
    }
    releaseHandle( fi );

    return 0;
}

/**
 * @brief Create a file node
 *
 * This is called for creation of all non-directory, non-symlink
 * nodes.  If the filesystem defines a create() method, then for
 * regular files that will be called instead.
 *
 * @param path
 * @param mode
 * @param rdev
 * @return
 */
int mkNodOp( const char * path, mode_t mode, dev_t rdev )
{
    logEntry( "\'%s\',%d,%d", path, mode, rdev );

    if ( S_ISFIFO( mode ) ) {
        return fixupResult( mkfifoat( getMountpointFD(),
                                      &path[ 1 ],
                                      mode ) );
    } else {
        return fixupResult( mknodat( getMountpointFD(),
                                     &path[ 1 ],
                                     mode,
                                     rdev ) );
    }
}

/**
 * @brief Create a directory
 *
 * Note that the mode argument may not have the type specification
 * bits set, i.e. S_ISDIR(mode) can be false.  To obtain the
 * correct directory type bits use  mode|S_IFDIR
 *
 * @param path
 * @param mode
 * @return
 */
int createDirOp( const char * path, mode_t mode )
{
    logEntry( "\'%s\',%d", path, mode );

    return fixupResult( mkdirat( getMountpointFD(), &path[ 1 ], mode ) );
}

/**
 * @brief Remove a file
 *
 * @param path
 * @return
 */
int fileUnlinkOp( const char * path )
{
    logEntry( "\'%s\'", path );

    return fixupResult( unlinkat( getMountpointFD(), &path[ 1 ], 0 ) );
}

/**
 * @brief Remove a directory
 * @param path
 * @return
 */
int removeDirOp( const char * path )
{
    logEntry( "\'%s\'", path );

    return fixupResult( unlinkat(getMountpointFD(),
                                 &path[ 1 ],
                                 AT_REMOVEDIR ) );
}

/**
 * @brief Create a symbolic link
 * @param from
 * @param to
 * @return
 */
int createSymlinkOp( const char * from, const char * to )
{
    logEntry( "\'%s\',\'%s\'", from, to );

    return fixupResult( symlinkat( from, getMountpointFD(), &to[ 1 ] ) );
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
int renameFsObjOp( const char * from, const char * to, unsigned int flags )
{
    logEntry( "\'%s\',\'%s\',%u", from, to, flags );
    return fixupResult( renameat2( getMountpointFD(),
                                         &from[ 1 ],
                                         getMountpointFD(),
                                         &to[ 1 ],
                                         flags) );
}

/** Create a hard link to a file */
int linkFileOp( const char * from, const char * to )
{
    /* ToDo: fixup from and to */
    logEntry( "\'%s\',\'%s\'", from, to );

    return fixupResult( linkat( getMountpointFD(),
                                      &from[ 1 ],
                                      getMountpointFD(),
                                      &to[ 1 ],
                                      0 ) );
}

/**
 * @brief Change the permission bits of a file
 *
 * `fi` will always be NULL if the file is not currenlty open, but
 * may also be NULL if the file is open.
 */
int chmodFileOp( const char * path, mode_t mode, struct fuse_file_info * fi )
{
    logEntry( "\'%s\',%p,%p", path, mode, fi );

    tFHFile * fh = getFileHandle( fi );
    if ( fh != NULL ) {
        return fixupResult( fchmod( fh->fd, mode ) );
    } else {
        return fixupResult( fchmodat( getMountpointFD(),
                            &path[ 1 ],
                             mode,
                            AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW ) );
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
int chownFileOp( const char * path, uid_t uid, gid_t gid, struct fuse_file_info * fi )
{
    logEntry( "\'%s\',%u,%u,%p", path, uid, gid, fi );

    tFHFile * fh = getFileHandle( fi );
    if ( fh ) {
        return fixupResult( fchown( fh->fd, uid, gid ) );
    } else {
        return fixupResult( fchownat( getMountpointFD(),
                           &path[ 1 ],
                           uid,
                           gid,
                           AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW ) );
    }
}

/** Change the size of a file
 *
 * `fi` will always be NULL if the file is not currently open, but
 * may also be NULL if the file is open.
 *
 * Unless FUSE_CAP_HANDLE_KILLPRIV is disabled, this method is
 * expected to reset the setuid and setgid bits.
 */
int truncateFileOp( const char * path, off_t size, struct fuse_file_info * fi )
{
    int result = 0;
    logEntry( "\'%s\',%u,%p", path, size, fi );

    if ( fi == NULL ) {
        fuse_log( FUSE_LOG_WARNING, "truncating \'%s\' with null fuse_file_info", path );
        result = fixupResult( truncate( path, size ) );
    } else {
        tFHFile * fh = getFileHandle( fi );
        if ( fh == NULL ) {
            fuse_log( FUSE_LOG_ERR, "attempt to truncate \'%s\' with invalid fileHandle",
                      path );
            result = -EINVAL;
        } else {
            if ( fh->isTemplate ) {
                /* a template file is treated as 'read-only', and truncating isn't allowed */
                result = -EPERM;
            } else {
                result = fixupResult( ftruncate( fh->fd, size ) );
            }
        }
    }

    return result;
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
int utimensOp( const char *path,
               const struct timespec ts[2],
               struct fuse_file_info *fi)
{
    /* don't use utime/utimes since they follow symlinks */
    if (fi)
    {
        return fixupResult( futimens(fi->fh, ts) );
    }
    else
    {
        return fixupResult( utimensat(0, path, ts, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW) );
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
 *
 * Note: file will always be created under the mount point, we won't be
 *       asked to create files that reside in the template hierarchy
 */
int createFileOp( const char * path, mode_t mode, struct fuse_file_info * fi )
{
    logEntry( "\'%s\',%u,%p", path, mode, fi );

    tFHFile * fh = (tFHFile *) calloc( 1, sizeof( tFileHandle ) );

    if ( fh == NULL ) {
        return -ENOMEM;
    }

    int fd = fixupResult( openat( getMountpointFD(),
                                  &path[ 1 ],
                                  fi->flags,
                                  mode ) );
    if ( fd >= 0 ) {
        fh->path = strdup( path );
        fh->fd = fd;
    }
    setFileHandle( fi, fh );
    return 0;
}

/**
 * @brief
 * @param fd
 * @param buffer
 * @param size
 * @return
 */
int executeTemplate( tFHFile *fh, byte ** buffer, size_t * size )
{
    int result = 0;
    tPrivateData * privateData;
    char * argv[3];

    int       stdoutPipe[2];
    int       stderrPipe[2];
    const int kPipeReadIdx  = 0;
    const int kPipeWriteIdx = 1;

    if ( pipe( stdoutPipe ) == -1 ) {
        logError("unable to create pipe for stdout");
        result = -errno;
    } else if ( pipe( stderrPipe ) == -1 ) {
        logError("unable to create pipe for stderr");
        result = -errno;
    } else {
        privateData = getPrivateData();

        pid_t pid = fork();
        switch (pid)
        {
        case -1: /* the fork() failed */
            logError( "failed to execute template");
            result = -errno;
            break;

        case 0: /* we are the child */
            if ( privateData != NULL ) {
                /* set up the argv array */
                asprintf( &argv[ 0 ], "%s%s", privateData->templates.path, fh->path );
                asprintf( &argv[ 1 ], "%s%s", privateData->mountpoint.path, fh->path );
                argv[ 2 ] = NULL;

                dup2( stdoutPipe[ kPipeWriteIdx ], STDOUT_FILENO);
                close( stdoutPipe[ kPipeReadIdx ] );
                fprintf( stdout, "child wrote to stdout\n" );

                dup2( stderrPipe[ kPipeWriteIdx ], STDERR_FILENO);
                close( stderrPipe[ kPipeReadIdx ] );
                fprintf( stderr, "child wrote to stderr\n" );

                logDebug( "child: execve( %s, %s )", argv[ 0 ], argv[ 1 ] );
                execve( argv[ 0 ], argv, globals.envp );
                /* !!! not expected to return !!! */
                result = -EXIT_FAILURE;
            }
            break;

        default:
            close( stdoutPipe[ kPipeWriteIdx ] );
            close( stderrPipe[ kPipeWriteIdx ] );

            static struct epoll_event events[2];
            int epfd = epoll_create(4);

            events[0].events  = EPOLLIN;
            events[0].data.fd = stdoutPipe[ kPipeReadIdx ];
            epoll_ctl( epfd, EPOLL_CTL_ADD, events[0].data.fd, &events[0] );

            events[1].events  = EPOLLIN;
            events[1].data.fd = stderrPipe[ kPipeReadIdx ];
            epoll_ctl( epfd, EPOLL_CTL_ADD, events[1].data.fd, &events[1] );

            tElasticBuffer * stdoutBuf = newElasticBuffer( 16384, 2048 );
            tElasticBuffer * stderrBuf = newElasticBuffer( 16384, 2048 );
            /* event loop to drain the pipes. Also watches for
             * EPOLLHUP, which implies the child exited */
            bool streamEOF = false;
            do {
                int eventCount = epoll_wait( epfd, events, 2, 10000 );

                for ( int i = 0; i < eventCount; i++ ) {
                    int eventFD = events[ i ].data.fd;
                    if ( events[ i ].events & EPOLLIN ) {
                        tElasticBuffer * buff = NULL;
                        if ( eventFD == stdoutPipe[ kPipeReadIdx ] ) {
                            buff = stdoutBuf;
                        } else if ( eventFD == stderrPipe[ kPipeReadIdx ] ) {
                            buff = stderrBuf;
                        } else {
                            logError( "epoll shows data available on"
                                      " unknown file descriptor %d", eventFD );
                            /* throw away the data, or we'll continue to get the same event */
                            lseek( eventFD, 0, SEEK_END );
                        }
                        if ( buff != NULL ) {
                            size_t readSize = read( eventFD,
                                                    getSpacePtr( stderrBuf ),
                                                    makeRoom( stderrBuf ) );
                            increaseAvailable( stderrBuf, readSize );
                        }
                    }
                    if ( events[ i ].events & ( EPOLLHUP | EPOLLERR ) ) {
                        /* either no more data (child probably exited) or an error
                         * either way, exit from the epoll loop */
                        streamEOF = true;
                    }
                }
            } while ( !streamEOF );

            if ( waitpid( pid, &result, 0 ) == -1 ) {
                result = -errno;
            }
            *buffer = stdoutBuf->data;
            *size   = stdoutBuf->available;
            free( stdoutBuf ); /* but don't free stdoutBuf->data! */

            if ( stderrBuf->available > 0 ) {
                logWarning( "stderr output from %s%s",
                            privateData->templates.path,
                            fh->path );
                logTextBlock( kLogWarning,
                              stderrBuf->data,
                              stderrBuf->available );
            }
            releaseElasticBuffer( stderrBuf );

            close( epfd );
            break;
        }
        close( stdoutPipe[kPipeReadIdx] );
        close( stderrPipe[kPipeReadIdx] );
    }


#ifdef DEBUG
    logDebug( "%s exit code: %d", fh->path, result );
#else
    if ( result != 0 ) {
        logError( "%s exit code: %d", fh->path, result );
    }
#endif

    return result;
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
int openFileOp( const char * path, struct fuse_file_info * fi )
{
    int result = 0;
    int fd;

    logEntry( "\'%s\', %p", path, fi );

    if ( fi == NULL ) {
        fuse_log( FUSE_LOG_ERR, "can't open a file using a null file_info structure" );
        result = -EINVAL;
    } else {
        tFHFile * fh = getFileHandle( fi );
        if ( fh == NULL ) {
            fh = (tFHFile *) calloc( 1, sizeof( tFileHandle ) );
            if ( fh == NULL ) {
                result = -ENOMEM;
            } else {
                setFileHandle( fi, fh );
            }
        }
        if ( fh != NULL ) {
            fh->path         = strdup( path );
            fh->isTemplate   = hasTemplate( path );
            fh->isExecutable = isExecutable( path );

            int rootfd;
            if ( fh->isTemplate ) {
                logDebug( "have%s template", fh->isExecutable ? " executable" : "" );
                rootfd = getTemplateFD();
            } else {
                logDebug( "regular file" );
                rootfd = getMountpointFD();
            }

            fd = fixupResult( openat( rootfd, &path[ 1 ], fi->flags ) );
            if ( fd < 0 ) {
                result = fd;
            } else {
                fh->path = strdup( path );
                fh->fd = fd;
                if ( fh->isTemplate ) {
                    if ( fh->isExecutable ) {
                        result = executeTemplate( fh, &fh->contents, &fh->length );
                    } else {
                        result = processTemplate( fh->fd, &fh->contents, &fh->length );
                    }
                }
            }
        }
    }

    return result;
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
int readFileOp( const char * UNUSED( path ),
                char * buf,
                size_t size,
                off_t offset,
                struct fuse_file_info * fi )
{
    int result = 0;
    logEntry( "\'%s\', %p, %ul, %ul, %p", path, buf, size, offset, fi );

    tFHFile * fh = getFileHandle( fi );
    if ( fh == NULL ) {
        result = -ENFILE;
    } else {
        if ( fh->isTemplate ) {
            if (fh->contents == NULL || (size_t)offset >= fh->length ) {
                result = -EOF;
            } else {
                /* if trying to read more data than we have, trim the size */
                if ( offset + size > fh->length ) {
                    size = fh->length - offset;
                }
                memcpy( buf, &fh->contents[offset], size );
                result = size;
            }
        } else {
            result = fixupResult( pread( fh->fd, buf, size, offset ) );
        }
    }

    return result;
}

/** Store data from an open file in a data
 *
 * Similar to the read() method, but data is stored and
 * returned in a generic data.
 *
 * No actual copying of data has to take place, the source
 * file descriptor may simply be stored in the data for
 * later data transfer.
 *
 * The data must be allocated dynamically and stored at the
 * location pointed to by bufp.  If the data contains memory
 * regions, they too must be allocated using malloc().  The
 * allocated memory will be freed by the caller.
 */
int readFileBufOp( const char * UNUSED( path ),
                   struct fuse_bufvec ** bufp,
                   size_t size,
                   off_t offset,
                   struct fuse_file_info * fi )
{
    int result = 0;;
    tFHFile * fh = NULL;
    logEntry( "\'%s\', %p", path, fi );

    if ( fi == NULL ) {
        result = -ENFILE;
    } else {
        fh = getFileHandle( fi );
        if ( fh == NULL ) {
            result = -ENFILE;
        } else {
            struct fuse_bufvec * src;
            src = (struct fuse_bufvec *) calloc( 1, sizeof( struct fuse_bufvec ) );
            if ( src == NULL ) {
                result = -ENOMEM;
            } else {
                *src = FUSE_BUFVEC_INIT( size );

                src->buf[ 0 ].flags = ( FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK );
                src->buf[ 0 ].fd    = fh->fd;
                src->buf[ 0 ].pos   = offset;

                *bufp = src;
            }
        }
    }

    return result;
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
int writeFileOp( const char * UNUSED( path ), const char * buf, size_t size,
                 off_t offset, struct fuse_file_info * fi )
{
    int result;

    logEntry( "\'%s\',%p,%u,%u,%p", path, buf, size, offset, fi );

    tFHFile * fh = getFileHandle( fi );
    if ( fh == NULL ) {
        result = -ENFILE;
    } else {
        if ( fh->isTemplate ) {
            /* fail if attempting to write to a template file - they are read-only */
            result = -EPERM;
        } else {
            /* the file is just a 'regular' file, so pass the write through */
            result = fixupResult( pwrite( fh->fd, buf, size, offset ) );
        }
    }

    return result;
}

/** Write contents of data to an open file
 *
 * Similar to the write() method, but data is supplied in a
 * generic data.  Use fuse_buf_copy() to transfer data to
 * the destination.
 *
 * Unless FUSE_CAP_HANDLE_KILLPRIV is disabled, this method is
 * expected to reset the setuid and setgid bits.
 */
int writeFileBufOp( const char * UNUSED( path ), struct fuse_bufvec * buf, off_t offset,
                    struct fuse_file_info * fi )
{
    struct fuse_bufvec dst = FUSE_BUFVEC_INIT( fuse_buf_size( buf ) );

    logEntry( "\'%s\',%p,%u,%p", path, buf, offset, fi );

    tFHFile * fh = getFileHandle( fi );
    if ( fh == NULL ) {
        return -ENFILE;
    }

    dst.buf[ 0 ].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
    dst.buf[ 0 ].fd    = fh->fd;
    dst.buf[ 0 ].pos   = offset;

    return fuse_buf_copy( &dst, buf, FUSE_BUF_SPLICE_NONBLOCK );
}

/**
 * @brief Get file system statistics
 *
 * The 'f_favail', 'f_fsid' and 'f_flag' fields are ignored
 */
int getFsStatsOp( const char * path, struct statvfs * stbuf )
{
    logEntry( "\'%s\',%p", path, stbuf );

    return fixupResult( statvfs( path, stbuf ) );
}

/**
 * @brief flush cached data (possibly)
 *
 * NOTE: This is not equivalent to fsync(). It's not a request to sync dirty data.
 *
 * Flush is called on each close() of a file descriptor, as opposed to release
 * which is called on the close of the last file descriptor for a file. Under
 * Linux, errors returned by flush() will be passed to userspace as errors from
 * close(), so flush() is a good place to write back any cached dirty data.
 *
 * However, many applications ignore errors on close(), and on non-Linux
 * systems, close() may succeed even if flush() returns an error. For these
 * reasons, filesystems should not assume that errors returned by flush will
 * ever be noticed or even delivered.
 *
 * NOTE: The flush() method may be called more than once for each open().
 *       This happens if more than one file descriptor refers to an open
 *       file handle, e.g. due to dup(), dup2() or fork() calls.  It is
 *       not possible to determine if a flush is final, so each flush
 *       should  be treated equally.  Multiple write-flush sequences are
 *       relatively rare, so this shouldn't be a problem.
 *
 * Filesystems shouldn't assume that flush will be called at any particular
 * point.  It may be called more times than expected, or not at all.
 *
 * [close]: http://pubs.opengroup.org/onlinepubs/9699919799/functions/close.html
 */
int flushFileOp( const char * UNUSED( path ), struct fuse_file_info * fi )
{
    int result = 0;
    logEntry( "\'%s\',%p", path, fi );

    tFHFile * fh = getFileHandle( fi );
    if ( fh == NULL ) {
        result = -ENFILE;
    } else {
        /* if it's a template, nothing needs to be done */
        if ( !fh->isTemplate ) {
            /* Every close on an open file calls flush, so call the close on the
             * underlying filesystem. But since flush may be called multiple
             * times for an open file, this *must not* actually close the file.
             * This is important if used on a network filesystem like NFS which
             * flushes the data/metadata on close() */
            result = fixupResult( close( dup( fh->fd ) ) );
        }
    }

    return result;
}

/**
 * @brief Release an open file
 *
 * Release is called when there are no more references to an open
 * file: all file descriptors are closed and all memory mappings
 * are unmapped.
 *
 * For every open() call there will be exactly one release() call
 * with the same flags and file handle.
 *
 * @param path
 * @param fi
 * @return The return value is ignored (but we set it correctly anyway)
 */
int releaseFileOp( const char * UNUSED( path ), struct fuse_file_info * fi )
{
    logEntry( "\'%s\',%p", path, fi );
    int result = -ENFILE;

    tFHFile * fh = getFileHandle( fi );
    if ( fh != NULL ) {
        result = fixupResult( close( fh->fd ) );

        /* we have cached contents, discard them */
        if ( fh->contents != NULL ) {
            free( fh->contents );
            fh->contents = NULL;
        }
        fh->length = 0;

        releaseHandle( fi );
    }

    return result;
}

/** Synchronize file contents
 *
 * If the datasync parameter is non-zero, then only the user data
 * should be flushed, not the meta data.
 */
int fsyncFileOp( const char * UNUSED( path ),
                 int UNUSED( isdatasync ),
                 struct fuse_file_info * fi )
{
    int result = -ENFILE;

    logEntry( "\'%s\',%u,%p", path, isdatasync, fi );

    tFHFile * fh = getFileHandle( fi );
    if ( fh != NULL ) {
#ifdef HAVE_FDATASYNC
        if (isdatasync)
        {
            result = fixupResult( fdatasync(fh->fd) );
        }
        else
#endif
        {
            result = fixupResult( fsync( fh->fd ) );
        }
    }

    return result;
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
int fallocateOp( const char *UNUSED( path),
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
int setXAttrOp( const char *path,
                const char *name,
                const char *value,
                size_t size,
                int flags)
{
    return fixupResult(lsetxattr(path, name, value, size, flags) == -1) );
}

/** Get extended attributes */
int getXAttrOp( const char *path,
                const char *name,
                char *value,
                size_t size)
{
    return fixupResult( (lgetxattr(path, name, value, size) == -1) );
}

/** List extended attributes */
int listXAttrOp( const char *path, char *list, size_t size)
{
    return fixupResult( llistxattr(path, list, size) );
}

/** Remove extended attributes */
int removeXAttrOp( const char *path, const char *name)
{
    return fixupResult(lremovexattr(path, name) );
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
int lockFileOp( const char *UNUSED( path), struct fuse_file_info *fi, int cmd, struct flock *lock)
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
 * Nonblocking requests will be indicated by ORing LOCK_NB to the above operations
 *
 * For more information see the flock(2) manual page.
 *
 * Additionally fi->owner will be set to a value unique to this open file.
 * This same value will be supplied to ->release() when the file is released.
 *
 * Note: if this method is not implemented, the kernel will still allow file
 * locking to work locally. Hence it is only interesting for network
 * filesystems and similar.
 */
int flockFileOp( const char * UNUSED( path ), struct fuse_file_info * fi, int op )
{
    logEntry( "\'%s\',%p,%d", path, fi, op );
    int result = -ENFILE;

    tFHFile * fh = getFileHandle( fi );
    if ( fh != NULL ) {
        result = fixupResult( flock( fh->fd, op ) );
    }

    return result;
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
ssize_t fileCopyRangeOp( const char * UNUSED( path_in),
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


    return fixupResult( copy_file_range(fh_in->fd,
                                        &off_in,
                                        fh_out->fd,
                                        &off_out,
                                        len,
                                        flags) );
}
#endif

/** Find next data or hole after the specified offset */
off_t lseekFileOp( const char * UNUSED( path ),
                   off_t off,
                   int whence,
                   struct fuse_file_info * fi )
{
    logEntry( "\'%s\',%u,%d,%p", path, off, whence, fi );
    off_t result = -ENFILE;

    tFHFile * fh = getFileHandle( fi );
    if ( fh != NULL ) {
        if ( !fh->isTemplate ) {
            result = fixupResult( lseek( fh->fd, off, whence ) );
        }
    }

    return result;
}

const struct fuse_operations templatefsOperations = {
    .init            = initFsOp,
    .getattr         = getFileAttrOp,
    .access          = fileAccessOp,
    .readlink        = readSymlinkOp,
    .opendir         = openDirOp,
    .readdir         = readDirOp,
    .releasedir      = releaseDirOp,
    .mknod           = mkNodOp,
    .mkdir           = createDirOp,
    .symlink         = createSymlinkOp,
    .unlink          = fileUnlinkOp,
    .rmdir           = removeDirOp,
    .rename          = renameFsObjOp,
    .link            = linkFileOp,
    .chmod           = chmodFileOp,
    .chown           = chownFileOp,
    .truncate        = truncateFileOp,
#ifdef HAVE_UTIMENSAT
    .utimens         = utimensOp,
#endif
    .create          = createFileOp,
    .open            = openFileOp,
    .read            = readFileOp,
//  .read_buf        = readFileBufOp,
    .write           = writeFileOp,
//  .write_buf       = writeFileBufOp,
    .statfs          = getFsStatsOp,
    .flush           = flushFileOp,
    .release         = releaseFileOp,
    .fsync           = fsyncFileOp,
#ifdef HAVE_POSIX_FALLOCATE
    .fallocate       = fallocateOp,
#endif
#ifdef HAVE_SETXATTR
    .setxattr        = setXAttrOp,
    .getxattr        = getXAttrOp,
    .listxattr       = listXAttrOp,
    .removexattr     = removeXAttrOp,
#endif
#ifdef HAVE_LIBULOCKMGR
    .lock            = lockFileOp,
#endif
    .flock           = flockFileOp,
#ifdef HAVE_COPY_FILE_RANGE
    .copy_file_range = fileCopyRangeOp,
#endif
    .lseek           = lseekFileOp,
};
