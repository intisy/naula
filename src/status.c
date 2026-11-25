#include "naula/status.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void copy_truncated(char *destination, size_t capacity, const char *text) {
    size_t length = text == NULL ? 0 : strlen(text);
    if (length >= capacity) {
        length = capacity - 1;
    }
    if (length > 0) {
        memcpy(destination, text, length);
    }
    destination[length] = '\0';
}

void naula_refuse(naula_refusal *refusal, const char *code, const char *fix, const char *format, ...) {
    va_list arguments;
    if (refusal == NULL) {
        return;
    }
    refusal->size = (uint32_t)sizeof *refusal;
    copy_truncated(refusal->code, sizeof refusal->code, code);
    refusal->fix = fix;
    va_start(arguments, format);
    vsnprintf(refusal->detail, sizeof refusal->detail, format, arguments);
    va_end(arguments);
    refusal->detail[sizeof refusal->detail - 1] = '\0';
}

void naula_overwrite(void *bytes, size_t length) {
    volatile unsigned char *cursor = bytes;
    while (length-- > 0) {
        *cursor++ = 0;
    }
}
