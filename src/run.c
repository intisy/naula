#include "run_internal.h"

#include <string.h>

#include "naula/memory.h"

int run_buffer_append(run_buffer *buffer, const char *bytes, size_t length) {
    if (buffer->length + length + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
        while (buffer->length + length + 1 > capacity) {
            capacity *= 2;
        }
        char *grown = naula_memory_reallocate(buffer->bytes, capacity);
        if (grown == NULL) {
            return 0;
        }
        buffer->bytes = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->bytes + buffer->length, bytes, length);
    buffer->length += length;
    buffer->bytes[buffer->length] = '\0';
    return 1;
}

void run_buffer_discard(run_buffer *buffer) {
    naula_memory_release(buffer->bytes);
    memset(buffer, 0, sizeof *buffer);
}

static char *take_text(run_buffer *buffer) {
    if (buffer->bytes == NULL) {
        return naula_memory_duplicate_text("");
    }
    char *text = buffer->bytes;
    memset(buffer, 0, sizeof *buffer);
    return text;
}

naula_status run_begin(const char *const *arguments, naula_run_result *result, naula_refusal *refusal) {
    if (result == NULL) {
        return NAULA_MALFORMED;
    }
    memset(result, 0, sizeof *result);
    result->size = (uint32_t)sizeof *result;
    if (arguments == NULL || arguments[0] == NULL || arguments[0][0] == '\0') {
        naula_refuse(refusal, "run-no-program", "pass the program as the first argument",
                     "no program to run was given");
        return NAULA_MALFORMED;
    }
    return NAULA_OK;
}

naula_status run_finish(naula_run_result *result, run_buffer *output, run_buffer *error, int exit_code) {
    result->exit_code = exit_code;
    result->output_length = output->length;
    result->output = take_text(output);
    result->error = take_text(error);
    if (result->output == NULL || result->error == NULL) {
        naula_run_result_release(result);
        run_buffer_discard(output);
        run_buffer_discard(error);
        return NAULA_OUT_OF_MEMORY;
    }
    return NAULA_OK;
}

void naula_run_result_release(naula_run_result *result) {
    if (result == NULL) {
        return;
    }
    naula_memory_release(result->output);
    naula_memory_release(result->error);
    result->output = NULL;
    result->error = NULL;
    result->output_length = 0;
}
