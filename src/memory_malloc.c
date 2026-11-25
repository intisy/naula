#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "memory_testing.h"

#ifdef NAULA_TESTING

/** @implNote Counting is not thread safe: a test arms it on the thread that allocates. */
static uint64_t countdown;
static uint64_t attempts;

static int fails_now(void) {
    attempts++;
    if (countdown == 0) {
        return 0;
    }
    countdown--;
    return countdown == 0;
}

void naula_memory_fail_at(uint64_t attempt) {
    countdown = attempt;
    attempts = 0;
}

uint64_t naula_memory_attempts(void) {
    return attempts;
}

#else

static int fails_now(void) {
    return 0;
}

#endif

void *naula_memory_allocate(size_t bytes) {
    return fails_now() ? NULL : malloc(bytes);
}

void *naula_memory_reallocate(void *block, size_t bytes) {
    return fails_now() ? NULL : realloc(block, bytes);
}

void naula_memory_release(void *block) {
    free(block);
}

char *naula_memory_duplicate_text(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = naula_memory_allocate(length);
    if (copy != NULL) {
        memcpy(copy, text, length);
    }
    return copy;
}
