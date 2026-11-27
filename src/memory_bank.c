#include <stdint.h>
#include <string.h>

#include "layout_apply_internal.h"
#include "layout_internal.h"
#include "naula/memory.h"

#define KEY_KEPT_UNITS 200
#define REPLACEMENT_UNIT 0xFFFDu

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

/** One character of the path as JavaScript holds it: one UTF-16 unit, or two for a character past U+FFFF. */
typedef struct utf16_character {
    size_t bytes;
    size_t units;
    uint32_t unit[2];
} utf16_character;

/** A byte that starts no well-formed character reads as one U+FFFD, as a UTF-8 decoder gives it. */
static utf16_character character_at(const unsigned char *text) {
    utf16_character character = {1, 1, {text[0], 0}};
    size_t length = lead_length(text[0]);
    if (text[0] >= 0x80 && length == 1) {
        character.unit[0] = REPLACEMENT_UNIT;
        return character;
    }
    for (size_t index = 1; index < length; index++) {
        if (!is_continuation(text[index])) {
            character.unit[0] = REPLACEMENT_UNIT;
            return character;
        }
    }
    uint32_t code = length == 1 ? text[0] : (uint32_t)text[0] & (0x7Fu >> length);
    for (size_t index = 1; index < length; index++) {
        code = (code << 6) | (text[index] & 0x3Fu);
    }
    character.bytes = length;
    if (code > 0xFFFFu) {
        character.units = 2;
        character.unit[0] = 0xD800u + ((code - 0x10000u) >> 10);
        character.unit[1] = 0xDC00u + ((code - 0x10000u) & 0x3FFu);
    } else {
        character.unit[0] = code;
    }
    return character;
}

/**
 * Claude Code's hash of the key's path, over its UTF-16 units: `h = (h << 5) - h + unit | 0` from 0, the bundled
 * function `Ble` that `Math.abs(...).toString(36)` prints.
 */
static int32_t path_hash(const char *real_path) {
    const unsigned char *text = (const unsigned char *)real_path;
    uint32_t hash = 0;
    while (*text != '\0') {
        utf16_character character = character_at(text);
        for (size_t unit = 0; unit < character.units; unit++) {
            hash = hash * 31u + character.unit[unit];
        }
        text += character.bytes;
    }
    return (int32_t)hash;
}

static size_t base36_of(int32_t hash, char digits[16]) {
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    int64_t magnitude = hash < 0 ? -(int64_t)hash : (int64_t)hash;
    char reversed[16];
    size_t length = 0;
    do {
        reversed[length++] = alphabet[magnitude % 36];
        magnitude /= 36;
    } while (magnitude > 0);
    for (size_t index = 0; index < length; index++) {
        digits[index] = reversed[length - 1 - index];
    }
    digits[length] = '\0';
    return length;
}

/** The replaced path, every unit that is not an ASCII letter or digit a `-`, as far as `capacity` allows. */
static size_t replaced_units(const char *real_path, char *key, size_t capacity) {
    const unsigned char *text = (const unsigned char *)real_path;
    size_t written = 0;
    while (*text != '\0') {
        int kept = is_ascii_alphanumeric(*text);
        utf16_character character = character_at(text);
        for (size_t unit = 0; unit < character.units; unit++) {
            if (written < capacity) {
                key[written] = kept ? (char)*text : '-';
            }
            written++;
        }
        text += character.bytes;
    }
    return written;
}

void naula_memory_bank_key(const char *real_path, char *key, size_t capacity) {
    if (capacity == 0) {
        return;
    }
    size_t units = replaced_units(real_path, key, capacity);
    char digits[16];
    size_t digit_count = units > KEY_KEPT_UNITS ? base36_of(path_hash(real_path), digits) : 0;
    size_t length = units > KEY_KEPT_UNITS ? KEY_KEPT_UNITS + 1 + digit_count : units;
    if (length >= capacity) {
        key[0] = '\0';
        return;
    }
    if (units > KEY_KEPT_UNITS) {
        key[KEY_KEPT_UNITS] = '-';
        memcpy(key + KEY_KEPT_UNITS + 1, digits, digit_count);
    }
    key[length] = '\0';
}

char *memory_bank_path(const char *banks, const char *real_path) {
    size_t capacity = strlen(real_path) + 1 + KEY_KEPT_UNITS + 16;
    char *key = naula_memory_allocate(capacity);
    if (key == NULL) {
        return NULL;
    }
    naula_memory_bank_key(real_path, key, capacity);
    char *path = key[0] == '\0' ? NULL : layout_join(banks, key);
    naula_memory_release(key);
    return path;
}
