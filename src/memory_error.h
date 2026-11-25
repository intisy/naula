#ifndef NAULA_MEMORY_ERROR_H
#define NAULA_MEMORY_ERROR_H

#include <errno.h>

/**
 * Whether a C library call that failed did so because memory ran out, by the errno it set.
 *
 * @implNote dlopen allocates through the C library's own allocator, which the binary's counting allocator never
 * sees, so only its errno tells memory running out from any other failure.
 */
static inline int naula_is_memory_errno(int error) {
    return error == ENOMEM;
}

#endif
