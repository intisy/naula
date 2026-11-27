#include <string.h>

#include "layout_apply_internal.h"
#include "layout_internal.h"
#include "naula/memory.h"

static int is_ascii_alphanumeric(unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9');
}

static int is_continuation(unsigned char byte) {
    return (byte & 0xC0) == 0x80;
}

static size_t lead_length(unsigned char lead) {
    if (lead >= 0xF0 && lead <= 0xF4) {
        return 4;
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        return 3;
    }
    return lead >= 0xC2 && lead <= 0xDF ? 2 : 1;
}

/**
 * How many bytes the character at `text` takes and how many UTF-16 units it is, since Claude Code replaces each
 * unit of its UTF-16 path text, so a character past U+FFFF becomes two dashes. A byte that starts no well-formed
 * character counts as one byte and one unit.
 */
static size_t character_at(const unsigned char *text, size_t *units) {
    size_t length = lead_length(text[0]);
    for (size_t index = 1; index < length; index++) {
        if (!is_continuation(text[index])) {
            length = 1;
            break;
        }
    }
    *units = length == 4 ? 2 : 1;
    return length;
}

void naula_memory_bank_key(const char *real_path, char *key, size_t capacity) {
    if (capacity == 0) {
        return;
    }
    const unsigned char *text = (const unsigned char *)real_path;
    size_t written = 0;
    while (*text != '\0') {
        int kept = is_ascii_alphanumeric(*text);
        size_t units = 1;
        size_t length = kept ? 1 : character_at(text, &units);
        if (written + units >= capacity) {
            key[0] = '\0';
            return;
        }
        for (size_t unit = 0; unit < units; unit++) {
            key[written++] = kept ? (char)*text : '-';
        }
        text += length;
    }
    key[written] = '\0';
}

char *memory_bank_path(const char *banks, const char *real_path) {
    size_t capacity = strlen(real_path) + 1;
    char *key = naula_memory_allocate(capacity);
    if (key == NULL) {
        return NULL;
    }
    naula_memory_bank_key(real_path, key, capacity);
    char *path = key[0] == '\0' ? NULL : layout_join(banks, key);
    naula_memory_release(key);
    return path;
}
