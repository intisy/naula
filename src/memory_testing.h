#ifndef NAULA_MEMORY_TESTING_H
#define NAULA_MEMORY_TESTING_H

#include <stdint.h>

#include "naula/memory.h"

#ifdef NAULA_TESTING
/** Makes the given allocation from now fail, 1 being the next; 0 disarms. Resets the attempt count either way. */
void naula_memory_fail_at(uint64_t attempt);
uint64_t naula_memory_attempts(void);
/** Blocks allocated and not yet released, so a test can see a leak. */
uint64_t naula_memory_live_blocks(void);
#endif

#endif
