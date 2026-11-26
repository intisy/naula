#ifndef NAULA_TEST_SCHEMA_JSON_H
#define NAULA_TEST_SCHEMA_JSON_H

#include <stddef.h>

#include "cJSON.h"

/**
 * cJSON_ParseWithLength, then each string value learns its decoded length, which schema_json_string_length answers.
 *
 * @implNote cJSON decodes `\u0000` into the string and its `valuestring` then reads as ending there, so a pattern that
 * refuses a NUL could never see one. The lengths come from the text's string tokens, which appear in the order a
 * walk of the tree meets each member's key and each string value.
 */
cJSON *schema_json_read(const char *text, size_t length);

/** A string's decoded length when schema_json_read gave it one, its strlen otherwise. */
size_t schema_json_string_length(const cJSON *string);

#endif
