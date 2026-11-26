#ifndef NAULA_TEST_SCHEMA_PATTERN_H
#define NAULA_TEST_SCHEMA_PATTERN_H

#include <stddef.h>

/**
 * An ECMA-262 pattern, as JSON Schema's `pattern` reads one, held to the subset schema/repo.schema.json writes:
 * `^`, `$`, ASCII literals, classes with ranges, `\d \t \n \r \v \f`, `\xHH` up to 7F and escaped
 * metacharacters, groups, `(?:)`, `(?=)`, `(?!)`, `|`, and greedy `* + ? {n} {n,} {n,m}`. It matches bytes, so a class
 * admitting bytes above 7F (a negated one) is refused unless it repeats as `*` or `+`, where bytes and code points
 * give the same verdict.
 *
 * @implNote A pattern outside the subset does not compile, so the parity test fails loudly rather than reading a
 * construct it does not implement as something else.
 */
typedef struct schema_pattern schema_pattern;

schema_pattern *schema_pattern_compile(const char *text);
int schema_pattern_search(const schema_pattern *pattern, const char *subject, size_t length);
void schema_pattern_release(schema_pattern *pattern);

#endif
