#ifndef NAULA_MEMORY_H
#define NAULA_MEMORY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The one allocator of a binary: every block naula answers is released with naula_memory_release. A binary links
 * exactly one definition of it, so a test build can count every attempt and make any one of them fail.
 */
void *naula_memory_allocate(size_t bytes);
/** realloc through the same allocator; NULL leaves block untouched. */
void *naula_memory_reallocate(void *block, size_t bytes);
void naula_memory_release(void *block);
/** A copy of text, NULL only when memory runs out. */
char *naula_memory_duplicate_text(const char *text);

#ifdef __cplusplus
}
#endif

#endif
