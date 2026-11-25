#include <string.h>

#include "json_document.h"
#include "naula/json.h"
#include "naula/memory.h"

#define NOT_AN_ESCAPED_UNIT (-1L)
#define ESCAPE_WIDTH 6

void naula_json_use_allocator(void) {
    cJSON_Hooks hooks = {naula_memory_allocate, naula_memory_release};
    cJSON_InitHooks(&hooks);
}

static int hex_value(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static long escaped_unit_at(const char *text, size_t length, size_t at) {
    if (at + ESCAPE_WIDTH > length || text[at] != '\\' || text[at + 1] != 'u') {
        return NOT_AN_ESCAPED_UNIT;
    }
    long unit = 0;
    for (size_t offset = 2; offset < ESCAPE_WIDTH; offset++) {
        int digit = hex_value(text[at + offset]);
        if (digit < 0) {
            return NOT_AN_ESCAPED_UNIT;
        }
        unit = unit * 16 + digit;
    }
    return unit;
}

static int is_high_surrogate(long unit) {
    return unit >= 0xD800 && unit <= 0xDBFF;
}

static int is_low_surrogate(long unit) {
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

/**
 * How many bytes past the backslash the escape at `at` covers, rewriting an unpaired surrogate to U+FFFD in place,
 * since cJSON refuses one and that refusal would read as running out of memory.
 */
static size_t rewrite_escape(char *text, size_t length, size_t at, int *carries_nul) {
    long unit = escaped_unit_at(text, length, at);
    if (unit == NOT_AN_ESCAPED_UNIT) {
        return 1;
    }
    if (unit == 0) {
        *carries_nul = 1;
        return 0;
    }
    if (is_high_surrogate(unit) && is_low_surrogate(escaped_unit_at(text, length, at + ESCAPE_WIDTH))) {
        return 2 * ESCAPE_WIDTH - 1;
    }
    if (is_high_surrogate(unit) || is_low_surrogate(unit)) {
        memcpy(text + at, "\\uFFFD", ESCAPE_WIDTH);
    }
    return ESCAPE_WIDTH - 1;
}

static int rewrite_escapes(char *text, size_t length) {
    int carries_nul = 0;
    int in_string = 0;
    for (size_t index = 0; index < length && !carries_nul; index++) {
        if (text[index] == '"') {
            in_string = !in_string;
        } else if (in_string && text[index] == '\\') {
            index += rewrite_escape(text, length, index, &carries_nul);
        }
    }
    return !carries_nul;
}

static const char *refusal_of(const uint8_t *bytes, size_t length) {
    if (!naula_json_is_valid_utf8(bytes, length)) {
        return "is not UTF-8";
    }
    if (memchr(bytes, '\0', length) != NULL) {
        return "carries a NUL";
    }
    if (!naula_json_is_grammar((const char *)bytes, length)) {
        return "is not JSON";
    }
    return NULL;
}

naula_status naula_json_parse(const uint8_t *bytes, size_t length, cJSON **root, const char **reason) {
    *root = NULL;
    *reason = length == 0 ? "is empty" : refusal_of(bytes, length);
    if (*reason != NULL) {
        return NAULA_MALFORMED;
    }
    char *text = naula_memory_allocate(length + 1);
    if (text == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    memcpy(text, bytes, length);
    text[length] = '\0';
    if (!rewrite_escapes(text, length)) {
        naula_memory_release(text);
        *reason = "carries an escaped NUL";
        return NAULA_MALFORMED;
    }
    *root = cJSON_ParseWithLength(text, length);
    naula_memory_release(text);
    return *root == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}
