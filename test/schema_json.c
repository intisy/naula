#include "schema_json.h"

#include <string.h>

typedef struct token_reader {
    const char *text;
    size_t length;
    size_t at;
} token_reader;

static int read_hex4(const token_reader *in, size_t at, unsigned *value) {
    *value = 0;
    if (at + 4 > in->length) {
        return 0;
    }
    for (size_t index = at; index < at + 4; index++) {
        char digit = in->text[index];
        unsigned nibble = digit >= '0' && digit <= '9'   ? (unsigned)(digit - '0')
                          : digit >= 'a' && digit <= 'f' ? (unsigned)(digit - 'a' + 10)
                          : digit >= 'A' && digit <= 'F' ? (unsigned)(digit - 'A' + 10)
                                                         : 16u;
        if (nibble > 15u) {
            return 0;
        }
        *value = *value * 16u + nibble;
    }
    return 1;
}

static int is_low_surrogate_next(const token_reader *in) {
    unsigned low = 0;
    return in->at + 1 < in->length && in->text[in->at] == '\\' && in->text[in->at + 1] == 'u' &&
           read_hex4(in, in->at + 2, &low) && low >= 0xDC00u && low <= 0xDFFFu;
}

static size_t escape_length(token_reader *in) {
    unsigned code_point = 0;
    if (in->at + 1 >= in->length || in->text[in->at + 1] != 'u') {
        in->at += 2;
        return 1;
    }
    read_hex4(in, in->at + 2, &code_point);
    in->at += 6;
    if (code_point >= 0xD800u && code_point <= 0xDBFFu && is_low_surrogate_next(in)) {
        in->at += 6;
        return 4;
    }
    return code_point < 0x80u ? 1 : (code_point < 0x800u ? 2 : 3);
}

static size_t next_string_length(token_reader *in) {
    size_t decoded = 0;
    while (in->at < in->length && in->text[in->at] != '"') {
        in->at++;
    }
    in->at++;
    while (in->at < in->length && in->text[in->at] != '"') {
        if (in->text[in->at] == '\\') {
            decoded += escape_length(in);
        } else {
            in->at++;
            decoded++;
        }
    }
    in->at++;
    return decoded;
}

static void annotate(token_reader *in, cJSON *node) {
    if (node->string != NULL) {
        next_string_length(in);
    }
    if (cJSON_IsString(node)) {
        node->valueint = (int)next_string_length(in) + 1;
    }
    for (cJSON *child = node->child; child != NULL; child = child->next) {
        annotate(in, child);
    }
}

cJSON *schema_json_read(const char *text, size_t length) {
    cJSON *json = cJSON_ParseWithLength(text, length);
    token_reader in = {text, length, 0};
    if (json != NULL) {
        annotate(&in, json);
    }
    return json;
}

size_t schema_json_string_length(const cJSON *string) {
    return string->valueint > 0 ? (size_t)string->valueint - 1 : strlen(string->valuestring);
}
