//
// Created by paul on 9/11/22.
//

#include "common.h"
#include "templatefs.h"
#include "processTemplate.h"
#include "logStuff.h"

#include <sys/mman.h>

#include <mustach/mustach-wrap.h>
#include <elektra.h>
#include <lua5.4/lua.h>
#include <lua5.4/lualib.h>

/**
 * high level wrapper for mustach - interface for callbacks
 *
 * The functions sel, subsel, enter and next should return 0 or 1.
 *
 * All other functions should normally return MUSTACH_OK (zero).
 *
 * If any function returns a negative value, it means an error that
 * stop the processing and that is reported to the caller.
 *
 * Mustach also has its own error codes. Using the macros
 * MUSTACH_ERROR_USER and MUSTACH_IS_ERROR_USER could help
 * to avoid clashes.
 */

typedef struct sSection {
    struct sSection *   next;
    int                 depth;
    char *              name;
    Key *               selection;
} tSection;

typedef struct {
    KDB *       kdb;
    KeySet *    keySet;

    tSection *  stack;

} tMustachContext;

int sectionPush( tMustachContext * context, int objiter )
{
    int result = 0;

    tSection * section = calloc( 1, sizeof(tSection));
    if ( section == NULL ) {
        result = -ENOMEM;
    } else {
        section->depth     = objiter;

        /* if there is another level below this one in the stack,
         * copy its contents up to populate the new one */
        if ( context->stack != NULL ) {
            section->name      = strdup( context->stack->name );
            section->selection = keyDup( context->stack->selection, KEY_CP_ALL );
        }

        /* push the new section onto the 'stack' (linked list) */
        section->next  = context->stack;
        context->stack = section;
    }

    return result;
}

int sectionPop( tMustachContext * context )
{
    int result = 0;

    if ( context->stack == NULL ) {
        logError( "attempted to leave more times than we entered" );
        result = MUSTACH_ERROR_TOO_DEEP;
    } else {
        tSection * section = context->stack;
        /* 'pop' the stack by unlinking the top (first) item */
        context->stack = section->next;
        /* release resources associated with that section */
        free( section->name );
        keyDel( section->selection );
        /* release the section itself */
        free( section );
    }
    return result;
}

/**
 * @brief
 * If defined (can be NULL), starts the mustach processing of the closure.
 * Called at the very beginning before any mustach processing occurs.
 *
 * @param closure
 * @return normally MUSTACH_OK, negative value on error
 */
int elektraStart( void * closure )
{
    logEntry( "" );

    int result = MUSTACH_OK;

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext * )closure;
        /* always at least ine entry on the stack */
        result = sectionPush( context, -1 );
    }

	return result;
}

/**
 * @brief
 * If defined (can be NULL), stops the mustach processing of the closure,
 * called at the very end after all mustach processing has finished.
 * The status returned by the processing is passed to the stop.
 *
 * @param closure
 * @param status
 */
void elektraStop( void * closure, int status )
{
    logEntry( "%d", status );

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext * )closure;
        /* dispose of the entry at the top of the stack */
        sectionPop( context );
    }
}

/**
 * @brief
 * If defined (can be NULL), compares the value of the currently
 * selected item with the given value and returns a negative value
 * if current value is lesser, a positive value if the current
 * value is greater or zero when values are equals.
 *
 * If 'compare' function pointer is NULL, any comparison in
 * mustach will always fail.
 *
 * @param closure
 * @param value
 * @return negative if less. positive if greater, zero if matches
 */
int elektraCompare( void * closure, const char * value )
{
    logEntry( "\'%s\'", value );

    int result = 0;

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext * )closure;
    }

	return result;
}

/**
 * @brief
 * Selects the item of the given 'name'. If 'name' is NULL
 * Selects the current item. Returns 1 if the selection is
 * effective or else 0 if the selection failed.
 *
 * @param closure
 * @param name
 * @return 1 if successful, 0 if not, negative values indicate an error occurred
 */
int elektraSel( void * closure, const char * name )
{
    logEntry( "\'%s\'", name );

    int result = 1;

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext * )closure;
        if ( context->stack->name != NULL ) {
            free( context->stack->name );
        }
        context->stack->name = strdup( name );
    }

	return result;
}

/**
 * @brief
 * Selects from the currently selected object the value of
 * the field of given name.
 *
 * @param closure
 * @param name
 * @return 1 if successful, 0 if not, negative values indicate an error occurred
 */
int elektraSubsel( void * closure, const char * name )
{
    logEntry( "\'%s\'", name );

    int result = 0;

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext * )closure;
    }

	return result;
}

/**
 * @brief Enters the section of 'name' (if possible)
 *
 * If 1 is returned, the function 'leave' will always be called.
 * Conversely 'leave' is never called when enter returns 0 or
 * a negative value.
 *
 * When 1 is returned, the function must activate the first
 * item of the section.
 *
 * @param closure
 * @param objiter
 * @return 1 if successful, 0 if not, negative values indicate an error occurred
 */
int elektraEnter( void * closure, int objiter )
{
    logEntry( "%d", objiter );

    int result = 1;

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext * )closure;

        sectionPush( context, objiter );
    }

	return result;
}

/**
 * @brief
 * Activates the next item of the section if it exists.
 * Returns 1 when the next item has been activated.
 * Returns 0 when there are no more items to activate.
 *
 * @param closure
 * @return 1 if successful, 0 if not, negative values indicate an error occurred
 */
int elektraNext( void * closure )
{
    logEntry( "" );

    int result = 0;

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext * )closure;
    }

	return result;
}



/**
 * @brief Leaves the last entered section
 * @param closure
 * @return normally MUSTACH_OK, negative value on error
 */
int elektraLeave( void * closure )
{
    logEntry( "" );

    int result = MUSTACH_OK;

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext *) closure;
        result = sectionPop( context );
    }

    return result;
}

/**
 * @brief return the value of key in sbuf
 *
 *  Returns in 'sbuf' the value of the current selection if 'key' is zero. Otherwise,
 *  when 'key' is not zero, return in 'sbuf' the name of key of the current selection,
 *  or if no such key exists, the empty string. Must return 1 if possible or 0 when
 *  not possible, or a negative error code.
 *
 * @param closure
 * @param sbuf
 * @param key
 * @return 1 if successful, 0 if not, negative values indicate an error occurred
 */
int elektraGet( void * closure, struct mustach_sbuf * sbuf, int key )
{
    logEntry( "%d", key );

    int result = 0;

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext *) closure;
        switch ( key )
        {
        case 0: /* return the value */
            result = 1;
            break;

        case 1: /* return key name */
            result = 1;
            break;

        default:
            logError( "invalid key value %d passed to get()", key );
            sbuf->value  = NULL;
            sbuf->length = 0;
            result = 0;
            break;
        }
    }

	return result;
}

struct mustach_wrap_itf elektraMustachItf = {
    .start   = elektraStart,
    .stop    = elektraStop,
    .compare = elektraCompare,
    .sel     = elektraSel,
    .subsel  = elektraSubsel,
    .enter   = elektraEnter,
    .next    = elektraNext,
    .leave   = elektraLeave,
    .get     = elektraGet
};

void cleanupElektra( tMustachContext * context )
{
    if (context != NULL ){
        if ( context->keySet != NULL ) {
            ksDel( context->keySet );
        }
        if ( context->kdb != NULL ) {
            kdbClose( context->kdb, NULL );
        }
    }
}

/**
 * @brief prep libelektra for retrieval
 * @param context
 * @return zero if everything worked, negative if an error occurred
 */
int initElektra( tMustachContext * context )
{
    int result = -EINVAL;

    if ( context != NULL ) {
        char * keyName = strdup( "system:/config" );
        Key  * parent  = keyNew( keyName, KEY_END );

        context->kdb = kdbOpen( NULL, parent );
        logDebug( "kdb = %p for \'%s\'", context->kdb, keyName );

        if ( context->kdb == NULL ) {
            logError( "unable to open libelektra" );
            result = -EFAULT;
        } else {
            context->keySet = ksNew( 0, KS_END );
            if ( context->keySet == NULL ) {
                logError( "failed to create a KeySet" );
                result = -EADDRNOTAVAIL;
            } else {
                /* It's necessary to preload the keySet.
                 * No idea why, but errors occur if you don't. */
                kdbGet( context->kdb, context->keySet, parent );
                result = 0;
            }
        }

        if ( result < 0 ) {
            cleanupElektra( context );
        }
    }
    return result;
}


/**
 * @brief process the template file
 *
 * uses mustach_wrap_mem, which renders the mustache template into a memory buffer
 * using an abstract wrapper of interface 'itf' and 'closure'.
 *
 * @template: the template string to instanciate
 * @length:   length of the template or zero if unknown and template null terminated
 * @itf:      the interface of the abstract wrapper
 * @closure:  the closure of the abstract wrapper
 * @result:   the pointer receiving the result when 0 is returned
 * @size:     the size of the returned result
 *
 * Returns 0 in case of success, -1 with errno set in case of system error
 * a other negative value in case of error.
 */

int processTemplate( int fd, byte ** buffer, size_t * size )
{
    int result = -ENOMEM;

    tMustachContext * context = calloc( 1, sizeof( tMustachContext ));
    if ( context != NULL ) {
        struct stat st;
        result = fstat( fd, &st );
        if ( result == -1 ) {
            result = -errno;
        } else {
            /* efficient way to feed the template file into mustach */
            void * template = mmap( NULL,
                                    st.st_size,
                                    PROT_READ,
                                    MAP_PRIVATE,
                                    fd,
                                    0 );

            if ( template != NULL ) {
                if ( initElektra( context ) == 0 ) {

                    /* now parse the template to generate the content to cache */
                    result = mustach_wrap_mem( template,
                                               st.st_size,
                                               &elektraMustachItf,
                                               (void *) context,
                                               Mustach_With_AllExtensions,
                                               (char **) buffer,
                                               size );

                    cleanupElektra( context );
                }
                munmap( template, st.st_size );
            }
        }
        free( context );
    }
    return result;
}
