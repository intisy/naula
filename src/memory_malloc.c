#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "memory_testing.h"

#ifdef NAULA_TESTING

/** @implNote Counting is not thread safe: a test arms it on the thread that allocates. */
static uint64_t countdown;
static uint64_t attempts;
static uint64_t live;

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

uint64_t naula_memory_live_blocks(void) {
    return live;
}

static void counted(const void *made, const void *was) {
    live += made != NULL && was == NULL;
}

static void uncounted(const void *block) {
    live -= block != NULL;
}

#else

static int fails_now(void) {
    return 0;
}

static void counted(const void *made, const void *was) {
    (void)made;
    (void)was;
}

static void uncounted(const void *block) {
    (void)block;
}

#endif

void *naula_memory_allocate(size_t bytes) {
    void *block = fails_now() ? NULL : malloc(bytes);
    counted(block, NULL);
    return block;
}

void *naula_memory_reallocate(void *block, size_t bytes) {
    void *grown = fails_now() ? NULL : realloc(block, bytes);
    counted(grown, block);
    return grown;
}

void naula_memory_release(void *block) {
    uncounted(block);
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
