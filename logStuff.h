/*
    logging macros, for my personal sanity...
*/

#ifndef UCIFS_LOGSTUFF_H
#define UCIFS_LOGSTUFF_H

#include <syslog.h>
#include <errno.h>
#include <dlfcn.h>

typedef enum { no = 0, off = 0,
              yes = 1,  on = 1 } tBool;

typedef int error_t;

typedef enum {
    kLogEmergency   = LOG_EMERG,    /* 0: system is unusable */
    kLogAlert       = LOG_ALERT,	/* 1: action must be taken immediately */
    kLogCritical    = LOG_CRIT,		/* 2: critical conditions */
    kLogError       = LOG_ERR,		/* 3: error conditions */
    kLogWarning 	= LOG_WARNING,	/* 4: warning conditions */
    kLogNotice      = LOG_NOTICE,	/* 5: normal but significant condition */
    kLogInfo        = LOG_INFO,		/* 6: informational */
    kLogDebug       = LOG_DEBUG, 	/* 7: debug-level messages */
    kLogFunctions   = 8,            /* (used for function entry/exit logging */
    kLogMaxPriotity = 9
} eLogPriority;

typedef enum {
    kLogToTheVoid,
    kLogToSyslog,
    kLogToFile,
    kLogToStderr,
    kLogMaxDestination
} eLogDestination;

typedef enum {
    kLogNothing = 0,
    kLogNormal,
    kLogWithLocation
} eLogMode;


/* set up the logging mechanisms. Call once, very early. */
void    initLogStuff( const char *name );

/* tidy up the current logging mechanism */
void    stopLoggingStuff( void );

void    setLogStuffDestination( eLogPriority priority, eLogDestination logDestination, eLogMode logMode );

/* start/stop logging function entry & exit */
void    logFunctionTrace( tBool onOff );

/* output a block of textBlock as a series of log lines, each with a line number prefixed */
void logTextBlock( eLogPriority priority, const char * textBlock, size_t textLen );

/* private helper, which the preprocessor macros expand to. Please don't use directly! */
void    _logEntry( unsigned int priority, const char * format, ... )
__attribute__((no_instrument_function));
//__attribute__((__format__ (__printf__, 2, 4)));

/* private helper, which the preprocessor macros expand to. Please don't use directly! */
void    _log( const char * inFile,
              unsigned int atLine,
              const char * inFunction,
              error_t      error,
              unsigned int priority,
              const char * format,
              ... )
__attribute__((__format__ (__printf__, 6, 7)))
__attribute__((no_instrument_function));

#define log( priority, ... ) \
    do { _log( __FILE__, __LINE__, __func__, errno, priority, __VA_ARGS__ ); } while (0)

#define logEmergency(...)  log( kLogEmergency, __VA_ARGS__ )
#define logAlert(...)      log( kLogAlert,     __VA_ARGS__ )
#define logCritical(...)   log( kLogCritical,  __VA_ARGS__ )
#define logError(...)      log( kLogError,     __VA_ARGS__ )
#define logWarning(...)    log( kLogWarning,   __VA_ARGS__ )
#define logNotice(...)     log( kLogNotice,    __VA_ARGS__ )
#define logInfo(...)       log( kLogInfo,      __VA_ARGS__ )

#ifdef DEBUG
#define logDebug(...)      log( kLogDebug, __VA_ARGS__ )
#define logEntry(fmt, ...) do { _logEntry( kLogDebug, "%s(" fmt ")", __func__, ##__VA_ARGS__ ); } while (0)
#define logCheckpoint()    do { _log( __FILE__, __LINE__, __func__, 0, kLogDebug, "reached" ); } while (0)
#else
#define logDebug(...)      do {} while (0)
#define logEntry(...)      do {} while (0)
#define logCheckpoint()    do {} while (0)
#endif

#endif //UCIFS_LOGSTUFF_H
