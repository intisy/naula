#include <string.h>

#include "naula/json.h"

typedef struct cursor {
    const unsigned char *text;
    size_t length;
    size_t at;
    int depth;
} cursor;

static int read_value(cursor *reader);

static int is_digit(unsigned char character) {
    return character >= '0' && character <= '9';
}

static int is_hex_digit(unsigned char character) {
    return is_digit(character) || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
}

static int at_end(const cursor *reader) {
    return reader->at >= reader->length;
}

static unsigned char peek(const cursor *reader) {
    return at_end(reader) ? 0 : reader->text[reader->at];
}

static int accept(cursor *reader, unsigned char character) {
    if (!at_end(reader) && reader->text[reader->at] == character) {
        reader->at++;
        return 1;
    }
    return 0;
}

static void skip_whitespace(cursor *reader) {
    while (!at_end(reader)) {
        unsigned char character = reader->text[reader->at];
        if (character != ' ' && character != '\t' && character != '\n' && character != '\r') {
            return;
        }
        reader->at++;
    }
}

static int read_digits(cursor *reader) {
    size_t start = reader->at;
    while (is_digit(peek(reader))) {
        reader->at++;
    }
    return reader->at > start;
}

static int read_number(cursor *reader) {
    accept(reader, '-');
    if (!accept(reader, '0') && !(is_digit(peek(reader)) && read_digits(reader))) {
        return 0;
    }
    if (accept(reader, '.') && !read_digits(reader)) {
        return 0;
    }
    if (accept(reader, 'e') || accept(reader, 'E')) {
        if (!accept(reader, '+')) {
            accept(reader, '-');
        }
        return read_digits(reader);
    }
    return 1;
}

static int read_escape(cursor *reader) {
    unsigned char escaped = peek(reader);
    reader->at++;
    if (escaped != 0 && strchr("\"\\/bfnrt", escaped) != NULL) {
        return 1;
    }
    if (escaped != 'u') {
        return 0;
    }
    for (int digit = 0; digit < 4; digit++) {
        if (!is_hex_digit(peek(reader))) {
            return 0;
        }
        reader->at++;
    }
    return 1;
}

static int read_string(cursor *reader) {
    if (!accept(reader, '"')) {
        return 0;
    }
    while (!at_end(reader)) {
        unsigned char character = reader->text[reader->at++];
        if (character == '"') {
            return 1;
        }
        if (character < 0x20) {
            return 0;
        }
        if (character == '\\' && !read_escape(reader)) {
            return 0;
        }
    }
    return 0;
}

static int read_literal(cursor *reader, const char *literal) {
    size_t length = strlen(literal);
    if (reader->length - reader->at < length || memcmp(reader->text + reader->at, literal, length) != 0) {
        return 0;
    }
    reader->at += length;
    return 1;
}

static int read_members(cursor *reader, unsigned char close, int keyed) {
    skip_whitespace(reader);
    if (accept(reader, close)) {
        return 1;
    }
    do {
        skip_whitespace(reader);
        if (keyed) {
            if (!read_string(reader)) {
                return 0;
            }
            skip_whitespace(reader);
            if (!accept(reader, ':')) {
                return 0;
            }
        }
        if (!read_value(reader)) {
            return 0;
        }
        skip_whitespace(reader);
    } while (accept(reader, ','));
    return accept(reader, close);
}

/** The same limit cJSON applies, so a text this accepts never fails in cJSON for its depth. */
static int read_container(cursor *reader, unsigned char close, int keyed) {
    if (++reader->depth > CJSON_NESTING_LIMIT) {
        return 0;
    }
    reader->at++;
    int read = read_members(reader, close, keyed);
    reader->depth--;
    return read;
}

static int read_value(cursor *reader) {
    skip_whitespace(reader);
    switch (peek(reader)) {
    case '{':
        return read_container(reader, '}', 1);
    case '[':
        return read_container(reader, ']', 0);
    case '"':
        return read_string(reader);
    case 't':
        return read_literal(reader, "true");
    case 'f':
        return read_literal(reader, "false");
    case 'n':
        return read_literal(reader, "null");
    default:
        return read_number(reader);
    }
}

int naula_json_is_grammar(const char *text, size_t length) {
    cursor reader = {(const unsigned char *)text, length, 0, 0};
    if (!read_value(&reader)) {
        return 0;
    }
    skip_whitespace(&reader);
    return at_end(&reader);
}

static size_t sequence_length(uint8_t lead) {
    if (lead < 0x80) {
        return 1;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        return 2;
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        return 3;
    }
    return lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
}

/** The second byte's range is what rules out overlong forms, surrogates and code points past U+10FFFF. */
static int second_byte_in_range(uint8_t lead, uint8_t second) {
    switch (lead) {
    case 0xE0:
        return second >= 0xA0;
    case 0xED:
        return second <= 0x9F;
    case 0xF0:
        return second >= 0x90;
    case 0xF4:
        return second <= 0x8F;
    default:
        return 1;
    }
}

int naula_json_is_valid_utf8(const uint8_t *bytes, size_t length) {
    size_t index = 0;
    while (index < length) {
        size_t width = sequence_length(bytes[index]);
        if (width == 0 || index + width > length) {
            return 0;
        }
        for (size_t offset = 1; offset < width; offset++) {
            if ((bytes[index + offset] & 0xC0) != 0x80) {
                return 0;
            }
        }
        if (width > 2 && !second_byte_in_range(bytes[index], bytes[index + 1])) {
            return 0;
        }
        index += width;
    }
    return 1;
}
