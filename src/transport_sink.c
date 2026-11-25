#include "naula/transport.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void naula_fetch_sink_start(naula_fetch_sink *sink, FILE *file, int limited, uint64_t limit) {
    sink->file = file;
    sink->memory = NULL;
    naula_sha256_start(&sink->hash);
    sink->bytes = 0;
    sink->limit = limit;
    sink->limited = limited;
    sink->exceeded = 0;
    sink->unwritable = 0;
}

void naula_fetch_sink_start_memory(naula_fetch_sink *sink, uint8_t *buffer, size_t capacity) {
    naula_fetch_sink_start(sink, NULL, 1, capacity);
    sink->memory = buffer;
}

static int store_bytes(naula_fetch_sink *sink, const uint8_t *bytes, size_t length) {
    if (sink->memory != NULL) {
        memcpy(sink->memory + sink->bytes, bytes, length);
        return 1;
    }
    return fwrite(bytes, 1, length, sink->file) == length;
}

int naula_fetch_sink_take(naula_fetch_sink *sink, const uint8_t *bytes, size_t length) {
    if (sink->exceeded || sink->unwritable) {
        return 0;
    }
    if (sink->limited && length > sink->limit - sink->bytes) {
        sink->exceeded = 1;
        return 0;
    }
    if (length > 0 && !store_bytes(sink, bytes, length)) {
        sink->unwritable = 1;
        return 0;
    }
    naula_sha256_add(&sink->hash, bytes, length);
    sink->bytes += length;
    return 1;
}

int naula_fetch_sink_admits(naula_fetch_sink *sink, uint64_t declared) {
    if (sink->limited && declared > sink->limit) {
        sink->exceeded = 1;
    }
    return !sink->exceeded;
}

void naula_fetch_refuse(naula_fetch_answer *answer, const char *fix, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(answer->detail, sizeof answer->detail, format, arguments);
    va_end(arguments);
    answer->outcome = NAULA_FETCH_REFUSED;
    answer->fix = fix;
}

void naula_fetch_out_of_memory(naula_fetch_answer *answer) {
    answer->detail[0] = '\0';
    answer->outcome = NAULA_FETCH_OUT_OF_MEMORY;
    answer->fix = NULL;
}

void naula_fetch_deadline_passed(naula_fetch_answer *answer, const naula_fetch_request *request) {
    naula_fetch_refuse(answer, NAULA_FETCH_NETWORK_FIX, "%s did not finish within %u ms", request->url,
                       request->deadline_milliseconds);
    answer->outcome = NAULA_FETCH_CUT_OFF;
    answer->declared = 0;
}
