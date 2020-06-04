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

#ifdef HAVE_LIBULOCKMGR
#include <ulockmgr.h>
#endif

#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>
#include <fuse3/fuse_common.h>
#include <fuse3/fuse_opt.h>
#ifdef INSTRUMENT_FUNCTIONS
#include <dlfcn.h>
#endif

#include "callbacks.h"

#define PACKAGE_VERSION "1.0"

#define UNUSED(arg) __attribute__((unused)) arg

tGlobals globals;


// ------------------------------------------------------------------------------

#define MOUNT_OPT( t, p, v ) { t, offsetof(tTemplateOptions, p), v }

const struct fuse_opt tmplCmdLineOptions[] =
{
        MOUNT_OPT( "templates=%s",    templates,    0 ),

        FUSE_OPT_END
};

int processTmplOpts( void * data, const char * arg, int key, struct fuse_args * outargs )
{
    return 1;
}

// ------------------------------------------------------------------------------

#ifdef INSTRUMENT_FUNCTIONS
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
#endif

// ------------------------------------------------------------------------------

#if 0
#define dumpArgs( args ) dumpArgs2( __LINE__, args )
void dumpArgs2( unsigned int lineNum, struct fuse_args * args )
{
    fuse_log( FUSE_LOG_DEBUG, "At line %d. allocated: %d", lineNum, args->allocated );
    fuse_log( FUSE_LOG_DEBUG, "argc: %d", args->argc );
    for ( int i = 0; i < args->argc; ++i )
    {
        fuse_log( FUSE_LOG_DEBUG, "%d: \"%s\"", i, args->argv[i] );
    }
}
#endif

int lightFuse( struct fuse_args * args )
{
    int result;
    struct fuse * fuse;

    fuse_log( FUSE_LOG_DEBUG, "fuse_new(%p,%p,%u,%p)",
              args,
              &templatefsOperations,
              sizeof( templatefsOperations ),
              NULL);

    fuse = fuse_new( args, &templatefsOperations, sizeof( templatefsOperations ), NULL);

    if ( fuse == NULL)
    {
        fuse_log( FUSE_LOG_CRIT, "error: fuse_new failed" );
        result = 3;
    }
    else if ( fuse_mount( fuse, globals.mountpoint.path ) != 0 )
    {
        fuse_log( FUSE_LOG_CRIT, "error: fuse_mount failed" );
        result = 4;
    }
    else
    {
        if ( fuse_daemonize( globals.options.common.foreground ) != 0 )
        {
            fuse_log( FUSE_LOG_CRIT, "error: fuse_daemonize failed" );
            result = 5;
        }
        else
        {
            struct fuse_session * se = fuse_get_session( fuse );

            if ( fuse_set_signal_handlers( se ) != 0 )
            {
                fuse_log( FUSE_LOG_CRIT, "error: fuse_set_signal_handlers failed" );
                result = 6;
            }
            else
            {
                if ( globals.options.common.singlethread )
                {
                    result = fuse_loop( fuse );
                }
                else
                {
                    struct fuse_loop_config loop_config;

                    loop_config.clone_fd         = globals.options.common.clone_fd;
                    loop_config.max_idle_threads = globals.options.common.max_idle_threads;
                    result = fuse_loop_mt( fuse, &loop_config );
                }
                if ( result != 0 )
                {
                    fuse_log( FUSE_LOG_CRIT, "error: fuse_loop failed" );
                    result = 7;
                }
                fuse_remove_signal_handlers( se );
            }
        }
        fuse_unmount( fuse );
    }
    fuse_destroy( fuse );

    return result;
}

__attribute__((no_instrument_function))
void log_to_syslog( enum fuse_log_level level, const char * fmt, va_list ap )
{
    vsyslog( level, fmt, ap );
}

int setupFSTree( tFSTree * tree, char * path )
{
    int result;

    tree->path = realpath( path, NULL);
    if ( tree->path == NULL || access( tree->path, F_OK ) != 0 )
    {
        fuse_log( FUSE_LOG_CRIT,
                  "error: path \"%s\" is invalid", tree->path );
        result = -errno;
    }
    else
    {
        fuse_log( FUSE_LOG_INFO,
                  "path is \"%s\"", tree->path );

        tree->fd = -1;
        tree->dir = opendir( tree->path );
        if ( tree->dir != NULL)
        {
            tree->fd = dirfd( tree->dir );
        }
        if ( tree->fd == -1 )
        {
            result = -errno;
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
    int              result;
    struct fuse_args args = FUSE_ARGS_INIT( argc, argv );

    static const char defaultTmplDir[] = "/templates";


    globals.myName = strdup( basename(argv[0] ));
    openlog( globals.myName, LOG_PID, LOG_DAEMON);
    fuse_set_log_func( log_to_syslog );

    fuse_log( FUSE_LOG_INFO, "%s started", globals.myName );

#ifdef INSTRUMENT_FUNCTIONS
    symbols = dlopen(NULL, RTLD_NOW );
    if ( symbols == NULL)
    {
        fuse_log( FUSE_LOG_ERR, "unable to load symbols (%s)", dlerror());
    }
    else
    {
        fuse_log( FUSE_LOG_DEBUG, "symbols loaded at %p", symbols );
    }
#endif

    if ( fuse_version() < FUSE_USE_VERSION )
    {
        fprintf(stderr, "The FUSE API version (%d) is older than %s requires (%d).\nAborting...",
                fuse_version(),
                globals.myName,
                FUSE_USE_VERSION);
        exit (-1);
    }

    /* first, let libfuse parse the common options from the command line */
    if ( fuse_parse_cmdline( &args, &globals.options.common ) == -1 )
    {
        fuse_log( FUSE_LOG_CRIT, "error: failed to parse command line" );
        result = 1;
    }
    else
    {
        memset( &globals.options.template, 0, sizeof( tTemplateOptions ));
        /* now extract options that are specific to templatefs */
        if ( fuse_opt_parse( &args, &globals.options.template, tmplCmdLineOptions, processTmplOpts ) == -1)
        {
            fuse_log( FUSE_LOG_CRIT, "error: failed to parse templatefs options");
            result = 8;
        }
        else
        {
            if ( globals.options.common.show_version )
            {
                printf( "%s version %s\n", globals.myName, PACKAGE_VERSION );

                fuse_version();

                result = 0;
            }
            else if ( globals.options.common.show_help )
            {
                if ( args.argv[0][0] != '\0' )
                {
                    printf( "usage: %s [options] <mountpoint>\n\n", globals.myName );
                }
                printf( "FUSE options:\n" );

                /* ToDo: add templatefs-specific options here */

                fuse_lib_help( &args );
                result = 0;
            }
            else
            {
                if ( !globals.options.common.mountpoint )
                {
                    fuse_log( FUSE_LOG_CRIT, "error: no mountpoint specified" );
                    result = 2;
                }
                else
                {
                    setupFSTree( &globals.mountpoint, globals.options.common.mountpoint );
                    setupFSTree( &globals.templates,  globals.options.template.templates );

                    result = lightFuse( &args );

                    free( globals.mountpoint.path );
                    free( globals.templates.path );
                }
            }
        }
    }
    fuse_opt_free_args( &args );

    return result;
}
