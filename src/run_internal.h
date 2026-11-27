#ifndef NAULA_RUN_INTERNAL_H
#define NAULA_RUN_INTERNAL_H

#include <stddef.h>

#include "naula/run.h"

typedef struct run_buffer {
    char *bytes;
    size_t length;
    size_t capacity;
} run_buffer;

/** 1 once bytes are appended, 0 when memory runs out. */
int run_buffer_append(run_buffer *buffer, const char *bytes, size_t length);

void run_buffer_discard(run_buffer *buffer);

/** Checks `arguments` and empties `result`; a refusal says why not. */
naula_status run_begin(const char *const *arguments, naula_run_result *result, naula_refusal *refusal);

/** Refuses input over NAULA_RUN_INPUT_LIMIT. */
naula_status run_check_input(const char *input, size_t input_length, naula_refusal *refusal);

/** Hands both buffers to `result` as NUL-terminated text, or discards them on running out of memory. */
naula_status run_finish(naula_run_result *result, run_buffer *output, run_buffer *error, int exit_code);

#endif
