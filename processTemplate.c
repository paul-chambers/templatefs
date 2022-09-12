//
// Created by paul on 9/11/22.
//

#include "common.h"
#include "templatefs.h"
#include "processTemplate.h"

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

typedef struct {
    int placeholder;
} tMustachContext;

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
    LOG_ON_ENTRY( "()" );
    int result = MUSTACH_OK;

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext * )closure;
        context->placeholder = 0;
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
    LOG_ON_ENTRY( "(%d)", status );

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext * )closure;
        context->placeholder = 0;
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
    LOG_ON_ENTRY( "(\'%s\')", value );
    int result = MUSTACH_OK;

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext * )closure;
        context->placeholder = 0;
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
    LOG_ON_ENTRY( "(\'%s\')", name );
    int result = 0;

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext * )closure;
        context->placeholder = 0;
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
    LOG_ON_ENTRY( "(\'%s\')", name );
    int result = 0;

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext * )closure;
        context->placeholder = 0;
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
    LOG_ON_ENTRY( "(\'%d\')", objiter );

    int result = 0;

    fuse_log( FUSE_LOG_DEBUG, "enter: %d", objiter );

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext * )closure;
        context->placeholder = 0;
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
    LOG_ON_ENTRY( "()" );
    int result = 0;

    fuse_log( FUSE_LOG_DEBUG, "next" );

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext * )closure;
        context->placeholder = 0;
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
    LOG_ON_ENTRY( "()" );
    int result = MUSTACH_OK;

    fuse_log( FUSE_LOG_DEBUG, "leave" );

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext *) closure;
        context->placeholder = 0;
    }

    return result;
}

/**
 * @brief return the value of key in sbuf
 *
 * Returns in 'sbuf' the value of the current selection if 'key'
 * is zero. Otherwise, when 'key' is not zero, return in 'sbuf'
 * the name of key of the current selection, or if no such key
 * exists, the empty string. Must return 1 if possible or
 * 0 when not possible or an error code.
 * @param closure
 * @param sbuf
 * @param key
 * @return 1 if successful, 0 if not, negative values indicate an error occurred
 */
int elektraGet( void * closure, struct mustach_sbuf * sbuf, int key )
{
    LOG_ON_ENTRY( "(%d)", key );
    int result = MUSTACH_OK;

    if ( closure != NULL ) {
        tMustachContext * context = (tMustachContext *) closure;
        context->placeholder = 0;
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
    struct stat st;
    tMustachContext * context = calloc( 1,
                                        sizeof( tMustachContext ));
    int result = fstat( fd, &st );
    if (result == -1) {
        result = -errno;
    } else {
        /* memory-efficient way to feed the contents of the template file into mustach */
        void * template = mmap( NULL,
                                st.st_size,
                                PROT_READ,
                                MAP_PRIVATE,
                                fd,
                                0 );

        /* now parse the template to generate the content to cache */
        result = mustach_wrap_mem( template,
                                   st.st_size,
                                   &elektraMustachItf,
                                   (void *)context,
                                   Mustach_With_AllExtensions,
                                   (char **)buffer,
                                   size);

        munmap( template, st.st_size );
    }

    return result;
}
