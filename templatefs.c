/*
    derived from passthrough_fh.c, part of the libfuse project.
    See https://github.com/libfuse/libfuse/blob/master/example/passthrough_fh.c

    The original is distributed under the terms of the GNU GPLv2.
    Since this counts as a 'derivative work', it is also provided
    under the terms of GPL v2.

    Copyright (c) Paul Chambers, 2020. All rights reserved.
*/

/** @file
 *
 * This file system mirrors the existing file system hierarchy of the
 * system, starting at the root file system. This is implemented by
 * just "passing through" all requests to the corresponding user-space
 * libc functions. This implementation is a little more sophisticated
 * than the one in passthrough.c, so performance is not quite as bad.
 *
 * ## Source code ##
 * \include templatefs.c
 */

#include "templatefs.h"

#define _GNU_SOURCE

#ifdef HAVE_LIBULOCKMGR
#include <ulockmgr.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>

#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>
#include <fuse3/fuse_common.h>
#include <fuse3/fuse_opt.h>
#include <dlfcn.h>

#include "callbacks.h"

#define PACKAGE_VERSION "1.0"

#define UNUSED(arg) __attribute__((unused)) arg

const char * myName = NULL;

struct fuse_cmdline_opts fuseOpts;

struct tmpl_options
{
    const char *  source;
    int           debug;
    int           writeback;
    int           flock;
    int           xattr;
} tmplOpts;

#define MOUNT_OPT( t, p, v ) { t, offsetof(struct tmpl_options, p), v }

const struct fuse_opt tmplCmdLineOptions[] =
{
        MOUNT_OPT( "source=%s",    source,    0 ),
        MOUNT_OPT( "writeback",    writeback, 1 ),
        MOUNT_OPT( "no_writeback", writeback, 0 ),
        MOUNT_OPT( "flock",        flock,     1 ),
        MOUNT_OPT( "no_flock",     flock,     0 ),
        MOUNT_OPT( "xattr",        xattr,     1 ),
        MOUNT_OPT( "no_xattr",     xattr,     0 ),

        FUSE_OPT_END
};

// ------------------------------------------------------------------------------

unsigned int depth = 0;
void *symbols = NULL;

const char leader[] = "...............................";

__attribute__((no_instrument_function))
void __cyg_profile_func_enter( void * func, void * caller )
{
    Dl_info info;
    int result = 0;

    if (symbols != NULL)
    {
        result = dladdr( func, &info );
    }
    if ( result != 0 && info.dli_sname != NULL )
    {
        syslog( LOG_DEBUG, "%.*s{ %s", 2 * depth, leader, info.dli_sname );
    } else {
        syslog( LOG_DEBUG, "%.*s{ %p", 2 * depth, leader, func );
    }

    ++depth;
}

__attribute__((no_instrument_function))
void __cyg_profile_func_exit( void * func, void * caller )
{
    Dl_info info;
    int result = 0;

    --depth;

    info.dli_sname = NULL;
    if ( symbols != NULL)
    {
        result = dladdr( func, &info );
    }
    if ( result != 0 && info.dli_sname != NULL )
    {
        syslog( LOG_DEBUG, "%.*s  %s }", 2 * depth, leader, info.dli_sname );
    }
    else
    {
        syslog( LOG_DEBUG, "%.*s  %p }", 2 * depth, leader, func );
    }
}

// ------------------------------------------------------------------------------

#define FUSE_HELPER_OPT( t, p ) \
    { t, offsetof(struct fuse_cmdline_opts, p), 1 }

static const struct fuse_opt tmpl_helper_opts[] = {
        FUSE_HELPER_OPT( "-h", show_help ),
        FUSE_HELPER_OPT( "--help", show_help ),
        FUSE_HELPER_OPT( "-V", show_version ),
        FUSE_HELPER_OPT( "--version", show_version ),
        FUSE_HELPER_OPT( "-d", debug ),
        FUSE_OPT_KEY( "-d", FUSE_OPT_KEY_KEEP ),
        FUSE_HELPER_OPT( "-d", foreground ),
        FUSE_HELPER_OPT( "debug", debug ),
        FUSE_HELPER_OPT( "debug", foreground ),
        FUSE_OPT_KEY( "debug", FUSE_OPT_KEY_KEEP ),
        FUSE_HELPER_OPT( "-f", foreground ),
        FUSE_HELPER_OPT( "-s", singlethread ),
        FUSE_HELPER_OPT( "fsname=", nodefault_subtype ),
        FUSE_OPT_KEY( "fsname=", FUSE_OPT_KEY_KEEP ),
#ifndef __FreeBSD__
        FUSE_HELPER_OPT( "subtype=", nodefault_subtype ),
        FUSE_OPT_KEY( "subtype=", FUSE_OPT_KEY_KEEP ),
#endif
        FUSE_HELPER_OPT( "clone_fd", clone_fd ),
        FUSE_HELPER_OPT( "max_idle_threads=%u", max_idle_threads ),
        FUSE_OPT_END
};

int tmpl_helper_opt_proc( void * data, const char * arg, int key,
                                 struct fuse_args * outargs )
{
    (void) outargs;
    struct fuse_cmdline_opts * opts = data;

    switch ( key )
    {
    case FUSE_OPT_KEY_NONOPT:
        if ( !opts->mountpoint )
        {
            char mountpoint[PATH_MAX] = "";
            if ( realpath( arg, mountpoint ) == NULL)
            {
                fuse_log( FUSE_LOG_ERR,
                          "fuse: bad mount point `%s': %s\n",
                          arg, strerror(errno));
                return -1;
            }
            return fuse_opt_add_opt( &opts->mountpoint, mountpoint );
        }
        else
        {
            fuse_log( FUSE_LOG_ERR, "fuse: invalid argument `%s'\n", arg );
            return -1;
        }

    default:
        /* Pass through unknown options */
        return 1;
    }
}

/* Under FreeBSD, there is no subtype option so this
   function actually sets the fsname */
int add_default_subtype( const char * progname, struct fuse_args * args )
{
    int  res;
    char * subtype_opt;

    const char * basename = strrchr( progname, '/' );
    if ( basename == NULL)
        basename = progname;
    else if ( basename[1] != '\0' )
        basename++;

    subtype_opt = (char *) malloc( strlen( basename ) + 64 );
    if ( subtype_opt == NULL)
    {
        fuse_log( FUSE_LOG_ERR, "fuse: memory allocation failed\n" );
        return -1;
    }
#ifdef __FreeBSD__
    sprintf(subtype_opt, "-ofsname=%s", basename);
#else
    sprintf( subtype_opt, "-o subtype=%s", basename );
#endif
    res = fuse_opt_add_arg( args, subtype_opt );
    free( subtype_opt );
    return res;
}


// ------------------------------------------------------------------------------


int processTmplOpts( void * data, const char * arg, int key, struct fuse_args * outargs )
{
    return 1;
}

__attribute__((no_instrument_function))
void log_to_syslog( enum fuse_log_level level, const char * fmt, va_list ap )
{
    vsyslog( level, fmt, ap );
}

#define dumpArgs(args) dumpArgs2( __LINE__, args )
void dumpArgs2( unsigned int lineNum, struct fuse_args * args )
{
    fuse_log( FUSE_LOG_DEBUG, "At line %d. allocated: %d", lineNum, args->allocated);
    fuse_log( FUSE_LOG_DEBUG, "argc: %d", args->argc );
    for (int i = 0; i < args->argc; ++i )
    {
        fuse_log( FUSE_LOG_DEBUG, "%d: \"%s\"", i, args->argv[i] );
    }
}

/* work around a problem with libfuse - bug? */
struct fuse_args * fuse_args_init( int argc, char ** argv )
{
    struct fuse_args * result = calloc( 1, sizeof(struct fuse_args) );
    if (result == NULL)
    {
        fuse_log(FUSE_LOG_DEBUG, "unable to allocate args structure");
    } else {
        result->argc = argc;
        unsigned int size = argc * sizeof( const char * );
        result->argv = calloc(1, size);
        if (result->argv != NULL)
        {
            for (int i = 0; i < argc; ++i)
            {
                result->argv[i] = strdup(argv[i]);
            }
            result->allocated = 1;
        }
    }

    return result;
}

/**
 * @brief main entry point
 * @param argc  count of command line arguments
 * @param argv  array of character pointers to the command line arguments
 * @return exit code
 */
__attribute__((no_instrument_function))
int main( int argc, char * argv[] )
{
    int res;
    struct fuse_args * args;
    struct fuse * fuse;
    struct fuse_cmdline_opts fuseOpts;

    myName = strdup( basename(argv[0] ));
    openlog( myName, LOG_PID, LOG_DAEMON);
    fuse_set_log_func( log_to_syslog );

    fuse_log( FUSE_LOG_INFO, "%s started", myName );

    symbols = dlopen(NULL, RTLD_NOW );
    if ( symbols == NULL)
    {
        fuse_log( FUSE_LOG_ERR, "unable to load symbols (%s)", dlerror());
    }
    else
    {
        fuse_log( FUSE_LOG_DEBUG, "symbols loaded at %p", symbols );
    }

    if ( fuse_version() < FUSE_USE_VERSION )
    {
        fprintf(stderr, "The FUSE API version (%d) is older than %s requires (%d).\nAborting...",
                fuse_version(),
                myName,
                FUSE_USE_VERSION);
        exit (-1);
    }

    /* work around some odd behaviors in libfuse - perhaps bugs? */
    args = fuse_args_init( argc, argv );

    /* first, let libfuse parse the common options from the command line */
    if ( fuse_parse_cmdline( args, &fuseOpts ) == -1 )
    {
        fuse_log( FUSE_LOG_CRIT, "error: failed to parse command line" );
        res = 1;
    }
    else
    {
        memset( &tmplOpts, 0, sizeof( tmplOpts ));
        /* now extract options that are specific to templatefs */
        if ( fuse_opt_parse( args, &tmplOpts, tmplCmdLineOptions, processTmplOpts ) == -1)
        {
            fuse_log( FUSE_LOG_CRIT, "error: failed to parse templatefs options");
            res = 8;
        } else {
            if ( fuseOpts.show_version )
            {
                printf( "%s version %s\n", myName, PACKAGE_VERSION );

                fuse_version();

                res = 0;
            }
            else if ( fuseOpts.show_help )
            {
                if ( args->argv[0][0] != '\0' )
                {
                    printf( "usage: %s [options] <mountpoint>\n\n", myName );
                }
                printf( "FUSE options:\n" );

                /* ToDo: add templatefs-specific options here */

                fuse_lib_help( args );
                res = 0;
            }
            else if ( !fuseOpts.mountpoint )
            {
                fuse_log( FUSE_LOG_CRIT, "error: no mountpoint specified" );
                res = 2;
            }
            else
            {
                fuse_log( FUSE_LOG_INFO, "Mountpoint is \"%s\"", fuseOpts.mountpoint );

                fuse_log( FUSE_LOG_DEBUG, "fuse_new(%p,%p,%u,%p)",
                          args,
                          &templatefsOperations,
                          sizeof( templatefsOperations ),
                          NULL);

                fuse = fuse_new( args, &templatefsOperations, sizeof( templatefsOperations ), NULL);

                if ( fuse == NULL)
                {
                    fuse_log( FUSE_LOG_CRIT, "error: fuse_new failed" );
                    res = 3;
                }
                else
                {
                    if ( fuse_mount( fuse, fuseOpts.mountpoint ) != 0 )
                    {
                        fuse_log( FUSE_LOG_CRIT, "error: fuse_mount failed" );
                        res = 4;
                    }
                    else
                    {
                        if ( fuse_daemonize( fuseOpts.foreground ) != 0 )
                        {
                            fuse_log( FUSE_LOG_CRIT, "error: fuse_daemonize failed" );
                            res = 5;
                        }
                        else
                        {
                            struct fuse_session * se = fuse_get_session( fuse );

                            if ( fuse_set_signal_handlers( se ) != 0 )
                            {
                                fuse_log( FUSE_LOG_CRIT, "error: fuse_set_signal_handlers failed" );
                                res = 6;
                            }
                            else
                            {
                                if ( fuseOpts.singlethread )
                                {
                                    res = fuse_loop( fuse );
                                }
                                else
                                {
                                    struct fuse_loop_config loop_config;

                                    loop_config.clone_fd         = fuseOpts.clone_fd;
                                    loop_config.max_idle_threads = fuseOpts.max_idle_threads;
                                    res = fuse_loop_mt( fuse, &loop_config );
                                }
                                if ( res != 0 )
                                {
                                    fuse_log( FUSE_LOG_CRIT, "error: fuse_loop failed" );
                                    res = 7;
                                }
                                fuse_remove_signal_handlers( se );
                            }
                        }
                        fuse_unmount( fuse );
                    }
                    fuse_destroy( fuse );
                }
                free( fuseOpts.mountpoint );
            }
        }
    }
    fuse_opt_free_args( args );

    return res;
}
