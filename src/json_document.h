#ifndef NAULA_JSON_DOCUMENT_H
#define NAULA_JSON_DOCUMENT_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "naula/status.h"

/**
 * Parses bytes held to JSON itself: well-formed UTF-8, JSON's grammar (cJSON alone is laxer), nothing after the
 * value, no NUL raw or escaped, and no deeper nesting than cJSON reads. An unpaired surrogate escape reads as
 * U+FFFD. Answers NAULA_MALFORMED with `reason` set for bytes that are not such JSON, and NAULA_OUT_OF_MEMORY when
 * an allocation failed, never the one for the other.
 */
naula_status naula_json_parse(const uint8_t *bytes, size_t length, cJSON **root, const char **reason);

int naula_json_is_valid_utf8(const uint8_t *bytes, size_t length);
int naula_json_is_grammar(const char *text, size_t length);

#endif
