#ifndef NAULA_TEST_SCHEMA_VALIDATE_H
#define NAULA_TEST_SCHEMA_VALIDATE_H

#include "cJSON.h"

typedef enum schema_verdict { SCHEMA_REFUSED = 0, SCHEMA_ACCEPTED = 1, SCHEMA_UNSUPPORTED = 2 } schema_verdict;

/**
 * Validates `instance` against `schema`, a subschema of the draft-07 document `root` that `#/...` references
 * resolve in. Only the keywords schema/repo.schema.json uses are implemented; any other keyword, an unresolved
 * reference or a pattern outside schema_pattern's subset answers SCHEMA_UNSUPPORTED and names it in `problem`.
 */
schema_verdict schema_validate(const cJSON *root, const cJSON *schema, const cJSON *instance, char *problem,
                               size_t capacity);

/** JSON equality as `const` and `enum` read it: numbers by value, object members in any order. */
int schema_json_equal(const cJSON *left, const cJSON *right);

/** The subschema at a JSON pointer such as `/definitions/repoName`, or NULL. */
const cJSON *schema_at(const cJSON *root, const char *pointer);

#endif
