#ifndef NAULA_JSON_H
#define NAULA_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "naula/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Points cJSON's allocation at naula_memory_allocate and naula_memory_release, so a test's counting allocator sees
 * every block a JSON parse makes. Call it first in every binary that links naula, before any thread starts.
 *
 * @implNote cJSON_InitHooks is process-global, so the library never installs hooks itself: a binary that also links
 * another user of cJSON decides once which allocator they share.
 */
void naula_json_use_allocator(void);

/**
 * Parses bytes held to JSON itself: well-formed UTF-8, JSON's grammar (cJSON alone is laxer), nothing after the
 * value, no NUL raw or escaped, and no deeper nesting than cJSON reads. An unpaired surrogate escape reads as
 * U+FFFD. Answers NAULA_MALFORMED with `reason` set for bytes that are not such JSON, and NAULA_OUT_OF_MEMORY when
 * an allocation failed, never the one for the other. A duplicated key stays in the tree as often as it is written,
 * so a reader that refuses one can see it. Release `root` with cJSON_Delete.
 */
naula_status naula_json_parse(const uint8_t *bytes, size_t length, cJSON **root, const char **reason);

/**
 * naula_json_parse, then every duplicated key, at any depth, keeps its first position and its last value, as
 * JavaScript's JSON.parse reads it, so a cJSON lookup or walk sees what JavaScript sees. Allocates no more than
 * naula_json_parse.
 */
naula_status naula_json_parse_keeping_last_value(const uint8_t *bytes, size_t length, cJSON **root,
                                                 const char **reason);

/** True when the bytes are well-formed UTF-8: no overlong form, no surrogate, nothing past U+10FFFF. */
int naula_json_is_valid_utf8(const uint8_t *bytes, size_t length);

/**
 * True when the text is one JSON value of JSON's own grammar with only whitespace around it: whitespace is SP, TAB,
 * LF and CR, a string holds no raw character below 0x20 and only JSON's escapes, a number is
 * -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?, and nothing nests deeper than CJSON_NESTING_LIMIT. Checks no UTF-8.
 */
int naula_json_is_grammar(const char *text, size_t length);

#ifdef __cplusplus
}
#endif

#endif
