//
// Created by paul on 7/24/22.
//

#define  _GNU_SOURCE  /* dladdr is a gcc extension */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdarg.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>

#include "logStuff.h"

#ifdef UNUSED
#elif defined(__GNUC__)
# define UNUSED(x) UNUSED_ ## x __attribute__((unused))
#elif defined(__LCLINT__)
# define UNUSED(x) /*@unused@*/ x
#else
# define UNUSED(x) (void)x
#endif

const char * gPriorityAsStr[] =
    {
        [kLogEmergency] = "Emergemcy",  /* 0: system is unusable */
        [kLogAlert]	    = "Alert",      /* 1: action must be taken immediately */
        [kLogCritical]	= "Critical",   /* 2: critical conditions */
        [kLogError]	    = "Err",        /* 3: error conditions */
        [kLogWarning]	= "Warning",    /* 4: warning conditions */
        [kLogNotice]	= "Notice",     /* 5: normal but significant condition */
        [kLogInfo]	    = "Info",       /* 6: informational */
        [kLogDebug]	    = "Debug",      /* 7: debug-level messages */
        [kLogFunctions] = ""            /* function call output */
    };

struct {
    eLogDestination destination;
    eLogMode        mode;
} gLogSetting[kLogMaxPriotity];

eLogDestination gLogDestination;

const char *    gMyName;
const char *    gLogFilePath = NULL;
FILE *          gLogFile;

void *          gDLhandle = NULL;
tBool           gFunctionTraceEnabled = off;
int             gCallDepth = 1;

static char * leader = "..........................................................................................";

#ifdef __GNUC__
#define DISABLE_FUNCTION_INSTRUMENTATION  __attribute__((no_instrument_function))
#else
#define DISABLE_FUNCTION_INSTRUMENTATION
#endif

/* {{{{{{{{ DO NOT INSTRUMENT THE INSTRUMENTATION! {{{{{{{{ */

void initLogStuff( const char *name )    DISABLE_FUNCTION_INSTRUMENTATION;

void _log( const char * inFile,
           unsigned int atLine,
           const char * inFunction,
           error_t      error,
           unsigned int priority,
           const char *format, ... )    DISABLE_FUNCTION_INSTRUMENTATION;

void _logToTheVoid( eLogPriority priority, const char *msg )   DISABLE_FUNCTION_INSTRUMENTATION;
void _logToSyslog(  eLogPriority priority, const char *msg )   DISABLE_FUNCTION_INSTRUMENTATION;
void _logToFile(    eLogPriority priority, const char *msg )   DISABLE_FUNCTION_INSTRUMENTATION;
void _logToStderr(  eLogPriority priority, const char *msg )   DISABLE_FUNCTION_INSTRUMENTATION;

void _profileHelper( void *leftAddr, const char *middle, void *rightAddr )  DISABLE_FUNCTION_INSTRUMENTATION;

const char * addressToString( void * addr, char * scratch )    DISABLE_FUNCTION_INSTRUMENTATION;

#ifdef __GNUC__
void __cyg_profile_func_enter( void *this_fn, void *call_site )     DISABLE_FUNCTION_INSTRUMENTATION;
void __cyg_profile_func_exit( void *this_fn, void *call_site )      DISABLE_FUNCTION_INSTRUMENTATION;
#endif

/* }}}}}}}} DO NOT INSTRUMENT THE INSTRUMENTATION! }}}}}}}} */


void _logToTheVoid( eLogPriority UNUSED(priority), const char * UNUSED(msg))  { /* just return */ }

void _logToSyslog( eLogPriority priority, const char * msg )         { syslog( priority, "%s", msg ); }

void _logToFile( eLogPriority UNUSED(priority), const char *msg )    { fprintf(gLogFile, "%s\n", msg); }

void _logToStderr( eLogPriority UNUSED(priority), const char *msg )  { fprintf(stderr, "%s\n", msg); }

/* use function pointers to invoke the logging destination */
typedef void (*fpLogTo)( unsigned int priority, const char * msg );
fpLogTo gLogOutputFP[kLogMaxDestination] =
    {
        [kLogToTheVoid] = &_logToTheVoid,
        [kLogToSyslog]  = &_logToSyslog,
        [kLogToFile]    = &_logToFile,
        [kLogToStderr]  = &_logToStderr
    };

/*
 * the macros eventually expand to invoke this help function
 */

void _log( const char *inPath,
           unsigned int atLine,
           const char * UNUSED(inFunction),
           error_t error,
           unsigned int priority,
           const char *format, ...)
{
    va_list vaptr;
    char    msg[1024];
    int     prefixLen;

    if ( gLogSetting[ priority ].destination != kLogToTheVoid )
    {
        va_start( vaptr, format );

        prefixLen = 0;
        /* if the destination isn't syslog, prefix the message with the priority */
        if ( priority <= kLogDebug && gLogSetting[priority].destination != kLogToSyslog)
        {
            prefixLen = snprintf( msg, sizeof( msg ), "%s: ", gPriorityAsStr[ priority ] );
        }

        prefixLen += vsnprintf( &msg[ prefixLen ], sizeof( msg ) - prefixLen, format, vaptr );

        if (error != 0)
        {
            prefixLen += snprintf( &msg[ prefixLen ], sizeof( msg ) - prefixLen, " (%d: %s)",
                                   error, strerror( error ) );
        }

        if ( gLogSetting[ priority ].mode == kLogWithLocation )
        {
            const char * inFile = strrchr(inPath, '/');
            if ( inFile++ == NULL )
            { inFile = inPath; }
            snprintf( &msg[ prefixLen ], sizeof( msg ) - prefixLen, " @ %s:%d", inFile, atLine );
        }

        ( gLogOutputFP[ gLogSetting[ priority ].destination ] )( priority, msg );

        va_end( vaptr );
    }
}

void _logEntry( unsigned int priority,
                const char *format, ...)
{
    va_list vaptr;
    char    msg[1024];
    int     prefixLen;

    if ( gLogSetting[ priority ].destination != kLogToTheVoid )
    {
        va_start( vaptr, format );

        prefixLen = 0;
        /* if the destination isn't syslog, prefix the message with the priority */
        if ( priority <= kLogDebug && gLogSetting[priority].destination != kLogToSyslog)
        {
            prefixLen = snprintf( msg, sizeof( msg ), "%s: ", gPriorityAsStr[ priority ] );
        }

        prefixLen += vsnprintf( &msg[ prefixLen ], sizeof( msg ) - prefixLen, format, vaptr );

        ( gLogOutputFP[ gLogSetting[ priority ].destination ] )( priority, msg );

        va_end( vaptr );
    }
}

void logTextBlock( eLogPriority priority, const char * textBlock, size_t textLen )
{
    unsigned int counter;
    char line[4096];

    const char * textEnd = &textBlock[textLen];
    if ( textLen == 0 )
    {
        textEnd = &textBlock[32767];
    }

    const char * t   = textBlock;
          char * dst = line;

    counter = 1;
    tBool newline = 1;
    while ( *t != '\0' && t < textEnd  )
    {
        if (newline)
        {
            newline = 0;

            int len = snprintf( line, sizeof( line ), "%3u: ", counter++ );
            if ( len > 0 )
            {
                dst = &line[len];
            }
        }

        switch ( *t )
        {
        case '\n':
        case '\r':
            newline = 1;
            do { ++t; } while ( *t == '\n' || *t == '\r' );
            break;

        case '\t':
            {
                unsigned int pos = dst - &line[5];
                while ( (++pos % 8) != 0 )
                {
                    *dst++ = ' ';
                }
            }
            ++t;
            break;

        default:
            *dst++ = *t++;
            break;
        }

        if ( newline || *t == '\0' )
        {
            *dst = '\0';
            ( gLogOutputFP[gLogSetting[priority].destination] )( priority, line );
        }
    }
}

void initLogStuff( const char * name )
{
    gMyName = "<not set>";
    if ( name != NULL )
    {
        /* strip off the path, if there is one */
        const char * p = strrchr( name, '/');
        if ( p++ == NULL )
        {
            p = name;
        }

        gMyName = strdup( p );
    }
    openlog( gMyName, LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);

    // initialize globals to something safe until startLogStuff has been invoked

    for ( int i = 0; i < kLogMaxPriotity; ++i )
    {
        gLogSetting[i].mode        = kLogWithLocation;
        gLogSetting[i].destination = kLogToStderr;
    }

    gDLhandle = dlopen(NULL, RTLD_LAZY);

    logFunctionTrace( off );
}

void setLogStuffDestination( eLogPriority logPrio, eLogDestination logDest, eLogMode logMode )
{
    for ( eLogPriority i = 0; i <= logPrio; ++i )
    {
        gLogSetting[i].mode        = logMode;
        gLogSetting[i].destination = logDest;
    }
}

void setLogStuffFileDestination( const char * logFile )
{
    if ( gLogFilePath != NULL)
    {
        free( (void *)gLogFilePath );
        gLogFilePath = NULL;
    }

    if ( logFile != NULL)
    {
        gLogFilePath = strdup( logFile );

        gLogFile = fopen( gLogFilePath, "a" );

        if ( gLogFile != NULL )
        {
        }
        else {
            logError( "Unable to log to \"%s\" (%d: %s), redirecting to stderr",
                      gLogFilePath, errno, strerror( errno ));
        }
    }
}

void stopLoggingStuff( void )
{
    for ( int i = kLogMaxPriotity; i <= 0; --i)
    {
        gLogOutputFP[i] = NULL;
    }

    switch (gLogDestination)
    {
    case kLogToSyslog:
        closelog();
        break;

    case kLogToFile:
        fclose( gLogFile );
        gLogFile = NULL;
        break;

    default:
        // don't do anything for the other cases
        break;
    }
}

const char * addressToString( void *addr, char *scratch)
{
    const char   *result;
    Dl_info info;

    result = NULL;
    if (gDLhandle != NULL )
    {
        dladdr(addr, &info);
        result = info.dli_sname;
    }
    if (result == NULL)
    {
        sprintf( scratch, "%p", addr);
        result = scratch;
    }
    return result;
}

#ifdef __GNUC__

/* helper function for logging function entry and exit */
void _profileHelper( void *leftAddr, const char *middle, void *rightAddr)
{
    // some scratch space, in case addressToString needs it. avoids needless malloc churn
    char leftAddrAsStr[64];
    char rightAddrAsStr[64];
    char msg[256];


    fpLogTo logDestFP = gLogOutputFP[gLogSetting[kLogFunctions].destination];

    if ( gFunctionTraceEnabled && logDestFP != NULL )
    {
        addressToString( leftAddr,  leftAddrAsStr  );
        addressToString( rightAddr, rightAddrAsStr );
        snprintf( msg, sizeof(msg), "%.*s %s() %s %s()",
                  gCallDepth, leader, leftAddrAsStr, middle, rightAddrAsStr );
        (*logDestFP)( kLogFunctions, msg);
    }
}

/*
    if the -finstrument-functions option is used with gcc, it inserts calls
    to these functions at the beginning and end of every compiled function.
*/

/* just landed in a function */
void __cyg_profile_func_enter(void *this_fn, void *call_site)
{
    _profileHelper( call_site, "called", this_fn );
    ++gCallDepth;
}

/* about to leave a function */
void __cyg_profile_func_exit(void *this_fn, void *call_site)
{
    if (--gCallDepth < 1) gCallDepth = 1;
    _profileHelper( this_fn, "returned to", call_site );
}

/*
    If just left on, this generates so much logging that it's rarely useful.
    Please use logFunctionTrace to turn on tracing only around the code
    or situation you care about.
*/
void logFunctionTrace( tBool onOff )
{
    gFunctionTraceEnabled = onOff;
}

#endif