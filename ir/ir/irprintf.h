/**
 * A little printf understanding some firm types.
 * @author Sebastian Hack
 * @date 29.11.2004
 */

#ifndef _IRPRINTF_H
#define _IRPRINTF_H

#include "config.h"

/**
 * A string formatting routine for ir nodes.
 * This function rudimentarily implements a kind of printf(3) for ir
 * nodes. Following conversion specifiers. No length, special or field
 * width specifiers are accepted.
 * - %p A pointer.
 * - %s A string.
 * - %n A full description of a node.
 * - %o The opcode name of an ir node.
 * - %m The mode name of an ir mode.
 * - %N The node number of an ir node.
 * - %b The block node number of the nodes block.
 * - %t A tarval.
 *
 * @param fmt The format string.
 */
void ir_printf(const char *fmt, ...);

/**
 * @see irn_printf.
 */
void ir_fprintf(FILE *f, const char *fmt, ...);

/**
 * @see irn_printf.
 */
void ir_snprintf(char *buf, size_t n, const char *fmt, ...);

#ifdef DEBUG_libfirm

#define ir_debugf ir_printf
#define ir_fdebugf ir_fprintf

#else

static INLINE void ir_debugf(const char *fmt, ...)
{
}

static INLINE void ir_fdebugf(FILE *, const char *fmt, ...)
{
}


#endif

#endif
