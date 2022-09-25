//
// Created by paul on 9/11/22.
//

/* Mote: the API for the C mustache engine uses 'mustach' throughout.
 *       For consistency's sake, I've also used 'mustach' in identifiers */

#include "common.h"
#include "templatefs.h"
#include "processTemplate.h"
#include "logStuff.h"

#include <sys/mman.h>

#include <mustach/mustach-wrap.h>
#include <sys/wait.h>
#include <ctype.h>

#include <elektra.h>

/**
 * high level wrapper for mustache - interface for callbacks
 *
 * The functions sel, subsel, enter and next should return 0 or 1.
 *
 * All other functions should normally return MUSTACH_OK (zero).
 *
 * If any function returns a negative value, it means an error that
 * stop the processing and that is reported to the caller.
 *
 * mustache also has its own error codes. Using the macros
 * MUSTACH_ERROR_USER and MUSTACH_IS_ERROR_USER could help
 * to avoid clashes.
 */

typedef struct sSection {
    struct sSection *   next;
    Key *               arraySelection; // only used if isArray is true
    Key *               selection;
    int                 depth;
    bool                isArray;
    elektraCursor       cursor;
} tSection;

typedef struct {
    KDB *       kdb;
    KeySet *    keySet;
    Key *       parent;

    tSection *  stack;

} tMustachContext;

/* The 'stack' is important to preserve the outer array state when arrays are nested */
/**
 * @brief
 * @param context
 * @param objiter
 * @return
 */
int sectionPush( tMustachContext * context, int objiter )
{
    logEntry( "%p,%d", context, objiter );
    int result = 0;

    tSection * section = calloc( 1, sizeof(tSection) );
    if ( context != NULL ) {
        if ( section == NULL ) {
            result = -ENOMEM;
        } else {
            section->depth = objiter;

            /* if there is another level below this one in the
             * stack, copy its contents up to the new one. */
            tSection * topOfStack = context->stack;
            if ( topOfStack != NULL ) {
                section->arraySelection = keyDup( topOfStack->arraySelection, KEY_CP_ALL );
                section->selection      = keyDup( topOfStack->selection, KEY_CP_ALL );
                section->isArray        = topOfStack->isArray;
                section->cursor         = topOfStack->cursor;
            } else {
                /* first entry in the stack, so initialize some fields */
                section->selection = keyNew( "system:/config", KEY_END );
                section->isArray   = false;
            }

            /* push the new section onto the 'stack' (linked list) */
            section->next  = context->stack;
            context->stack = section;
        }
    }

    return result;
}

int sectionPop( tMustachContext * context )
{
    logEntry( "%p", context );
    int result = 0;

    if ( context != NULL ) {
        tSection * section = context->stack;
        if ( section == NULL ) {
            logError( "attempted to leave more times than we entered" );
            result = MUSTACH_ERROR_TOO_DEEP;
        } else {
            /* 'pop' the stack by unlinking the top (first) item */
            context->stack = section->next;
            /* release resources associated with that section */
            if ( section->selection ) {
                keyDel( section->selection );
            }
            if ( section->arraySelection ) {
                keyDel( section->arraySelection );
            }
            /* release the section itself */
            free( section );
        }
    }

    return result;
}

/**
 * @brief starts the mustache processing of the closure.
 * Called at the very beginning before any mustache processing occurs.
 *
 * @param closure Note: can be NULL
 * @return normally MUSTACH_OK, negative value on error
 */
int elektraStart( void * closure )
{
    logEntry( "" );

    int result = MUSTACH_OK;

    if ( closure != NULL ) {
        /* always at least one entry on the stack */
        result = sectionPush( (tMustachContext * )closure, -1 );
    }

	return result;
}

/**
 * @brief stops the mustache processing of the closure
 * called at the very end after all mustache processing has finished.
 * The status returned by the processing is passed to the stop.
 *
 * @param closure Note: can be NULL
 * @param status
 */
void elektraStop( void * closure, int status )
{
    logEntry( "%d", status );

    if ( closure != NULL ) {
        /* dispose of the entry at the top of the stack */
        sectionPop( (tMustachContext * )closure );
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
 * mustache will always fail.
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
        (void) context;
    }

	return result;
}

/**
 * @brief
 * @param context
 * @return 1 if successful, 0 if not, negative values indicate an error occurred
 */
int selectNextArrayKey( tSection * section, KeySet * keySet )
{
    int result = 0;

    if ( section != NULL && section->isArray ) {
        Key * key = NULL;
        do {
            ++section->cursor;
            if ( key != NULL ) {
                keyDel( key );
            }
            key = ksAtCursor( keySet, section->cursor );

            /* if the key is valid and still below the array key... */
            if ( key != NULL && keyIsBelow( section->arraySelection, key ) ) {
                /* ...then key matches only if it is directly below the array key */
                result = keyIsDirectlyBelow( section->arraySelection, key );
            } else {
                /* ...otherwise we are now pointing just passed the last child
                 * of the array key and should exit the do-while loop */
                break;
            }
        } while ( result == 0 );

        if ( result == 1 && key != NULL ) {
            section->selection = key;

            logDebug( "next key in array is \'%s\'", keyName( key ) );
        }
    }

    return result;
}

/**
 * @brief update all the key-related fields in the section.
 * Also selects the first child key if the key represents an array.
 * @param context
 * @param section
 * @return 1 if successful, 0 if not, negative values indicate errors
 */
int updateSelection( tMustachContext * context, tSection * section )
{
    int result = 0;

    logDebug( "selecting %s", keyName( section->selection ) );
    if ( context != NULL ) {
        section->selection = ksLookup( context->keySet,
                                       section->selection,
                                       KDB_O_NONE );
        if ( section->selection != NULL ) {
            result = 1;

            const Key * metaArray = keyGetMeta( section->selection, "array" );
            section->isArray = (metaArray != NULL);
            if (section->isArray) {
                /* remember the base key of the array. section->selection
                 * will move through the direct children of this key */
                section->arraySelection = keyDup( section->selection, KEY_CP_ALL );
                /* Select the first item - find the electraCursor value or the base key */
                section->cursor = ksSearch( context->keySet, section->arraySelection );
                if ( section->cursor < 0 ) {
                    logError( "failed to locate the selection");
                    section->cursor = 0;
                    result = -EKEYREJECTED;
                } else {
                    logDebug( "array cursor = %ld", section->cursor );
                    result = selectNextArrayKey( context->stack, context->keySet );
                }
            }
        }
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

    int result = 0;

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext *) closure;
        tSection        * section = context->stack;

        bool append = false;
        /* absolute path, no namespace */
        if ( name[ 0 ] != '/' ) {
            /* check to see which seperator occurs first, if any */
            char * sep = strpbrk( name, ":/" );
            /* if there's no separator, or it's not an initial
             * colon terminating a namespace, we can append */
            if ( sep == NULL || *sep != ':' ) {
                append = true;
            }
        }

        if ( append ) {
            /* refresh the selected key with the parent's selection
             * This is important when appending to array index keys */
            tSection * parent = section->next;

            /* if there's another level above on the stack, append to that */
            if ( parent != NULL ) {
                /* recover any resources used by the current selection key */
                if ( section->selection != NULL ) {
                    keyDel( section->selection );
                }
                section->selection = keyDup( parent->selection, KEY_CP_ALL );
            }

            result = (int) keyAddBaseName( section->selection, name );
            if ( result >= 0 ) {
                result = updateSelection( context, section );
            } else {
                logError( "keyAddBaseName %s to %s failed",
                          name, keyName(section->selection) );
                result = 0;
            }
        } else {
            if ( section->selection != NULL ) {
                keyDel( section->selection );
            }
            section->selection = keyNew( name, KEY_END );
            result = updateSelection( context, section );
        }
    }

    logDebug( "%s returned %d", __func__, result );
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
//      tMustachContext * context = (tMustachContext * )closure;
    }

    logDebug( "%s returned %d", __func__, result );
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
        sectionPush( (tMustachContext * )closure, objiter );
    }

    logDebug( "%s returned %d", __func__, result );
	return result;
}

/**
 * @brief return the value of type in sbuf
 *
 *  Returns in 'sbuf' the value of the current selection if 'type' is zero. Otherwise,
 *  when 'type' is not zero, return in 'sbuf' the name of type of the current selection,
 *  or if no such type exists, the empty string. Must return 1 if possible or 0 when
 *  not possible, or a negative error code.
 *
 * @param closure
 * @param sbuf
 * @param type
 * @return 1 if successful, 0 if not, negative values indicate an error occurred
 */
int elektraGet( void * closure, struct mustach_sbuf * sbuf, int type )
{
    logEntry( "%d", type );

    int result = 0;

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext *) closure;
        tSection * section = context->stack;

        if ( section != NULL ) {
            if ( type == 0 ) {
                /* return the value of the type */
                ssize_t len = keyGetValueSize( section->selection );
                if ( keyIsBinary( section->selection ) ) {
                    union {
                        short  integerValue;
                        long   longValue;
                    } binaryValue;

                    keyGetBinary( section->selection,
                                  &binaryValue,
                                  sizeof(binaryValue) );
                    switch (len) {
                    case sizeof(short):
                        result = asprintf( (char **) &sbuf->value,
                                           "%d",
                                           binaryValue.integerValue );
                        break;

                    case sizeof(long):
                        result = asprintf( (char **) &sbuf->value,
                                           "%ld",
                                            binaryValue.longValue );
                        break;

                    default:
                        logError( "unsupported length of binary value: %ld bytes", len );
                        result = -EINVAL;
                        break;
                    }
                    if ( result >= 0 ) {
                        sbuf->length = result;
                        result = 1;
                    }
                } else {
                    sbuf->length = len;
                    sbuf->value  = malloc( len );
                    if ( sbuf->value != NULL ) {
                        result = (int) keyGetString( section->selection,
                                                     (char *) sbuf->value,
                                                     sbuf->length );
                    }
                }
                logDebug( "type value: \'%s\', result: %d", sbuf->value, result );
                if ( result > 0 ) result = 1;
            } else {
                /* return the name of the type */
                sbuf->length = keyGetNameSize( section->selection );
                sbuf->value  = malloc( sbuf->length );
                if ( sbuf->value != NULL ) {
                    result = (int) keyGetName( section->selection,
                                               (char *)sbuf->value,
                                               sbuf->length );
                    logDebug( "type name: \'%s\', result: %d", sbuf->value, result );
                    if (result > 0) result = 1;
                }
            }
        }
    }

    logDebug( "%s returned %d", __func__, result );
    return result;
}

/**
 * @brief Activates the next item of the section if it exists.
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
    tMustachContext * context = (tMustachContext *) closure;

    if ( context != NULL ) {
        tSection * section = context->stack;
        if ( section != NULL ) {
            if ( section->next != NULL ) {
                section = section->next;
            }
            result = selectNextArrayKey( section, context->keySet );
        }
    }

    logDebug( "%s returned %d", __func__, result );
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

    logDebug( "%s returned %d", __func__, result );
    return result;
}


struct mustach_wrap_itf elektraMustachItf = {
    .start   = elektraStart,
    .stop    = elektraStop,
    .compare = elektraCompare,
    .sel     = elektraSel,
    .subsel  = elektraSubsel,
    .enter   = elektraEnter,
    .get     = elektraGet,
    .next    = elektraNext,
    .leave   = elektraLeave
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

        context->parent = keyNew(keyName, KEY_END);
        context->kdb    = kdbOpen( NULL, context->parent );
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
                 * No idea why, but errors occur if we don't. */
                kdbGet( context->kdb, context->keySet, context->parent );
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
 * uses mustach_wrap_mem, which renders the mustache template into a memory data
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
            /* efficient way to feed the template file into mustache */
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
