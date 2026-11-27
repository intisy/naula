#ifndef NAULA_REPO_FILE_RULES_H
#define NAULA_REPO_FILE_RULES_H

/**
 * The rules of repo.json format 2 as schema/repo.schema.json states them, one entry per schema location the parser
 * enforces. The parser takes its limits and its member lists from here and its pattern checks from the matchers bound
 * here, and test/repo_file_schema_parity_test.c holds this table and the schema to each other in both directions.
 */

#include <stddef.h>

#define REPO_FILE_FORMAT 2
#define REPO_FILE_NAME_LENGTH_LIMIT 100
#define REPO_FILE_TOPIC_COUNT_LIMIT 20
#define REPO_FILE_TOPIC_LENGTH_LIMIT 50
#define REPO_FILE_PATH_LENGTH_LIMIT 1024
#define REPO_FILE_LEAST_HOMES 1
#define REPO_FILE_LEAST_PLACES 1
#define REPO_FILE_SHORT_HASH_DIGITS 40
#define REPO_FILE_LONG_HASH_DIGITS 64
#define REPO_FILE_PROVIDER_LENGTH_LIMIT 32
#define REPO_FILE_ACCOUNT_LENGTH_LIMIT 100
#define REPO_FILE_DOMAIN_LENGTH_LIMIT 255
#define REPO_FILE_PORT_DIGITS_LIMIT 5
#define REPO_FILE_NUMBER_DIGITS_LIMIT 19
#define REPO_FILE_URL_LENGTH_LIMIT 2048

#define REPO_FILE_WEB_PROVIDER "web"

/**
 * The visibilities a repo may state, the first passed to `first` and each later one to `next`, so this one list
 * builds the schema's enum text, the parser's list and the refusal's fix.
 */
#define REPO_FILE_VISIBILITIES(first, next) first("public") next("private")

#define REPO_FILE_HOME_PROVIDER "provider"
#define REPO_FILE_HOME_HOST "host"
#define REPO_FILE_HOME_ACCOUNT "account"
#define REPO_FILE_HOME_DOMAIN "domain"
#define REPO_FILE_HOME_URL "url"

#define REPO_FILE_TEXT(value) #value
#define REPO_FILE_TEXT_OF(value) REPO_FILE_TEXT(value)

typedef enum repo_file_rule_kind {
    /** A constraint keyword; `expected` is JSON text, or a string's own text where it does not read as JSON. */
    REPO_FILE_RULE_VALUE = 1,
    /** A `pattern`; `expected` is the pattern's text and `matcher` the C check the parser applies in its place. */
    REPO_FILE_RULE_PATTERN = 2,
    /** A name under `properties`: the parser allows that member in that object, and no other. */
    REPO_FILE_RULE_MEMBER = 3,
    /** A name a `required` lists, at `<location of required>/<name>`. */
    REPO_FILE_RULE_REQUIRED = 4
} repo_file_rule_kind;

typedef int (*repo_file_matcher)(const char *text);

/** One schema location, a JSON pointer into schema/repo.schema.json. */
typedef struct repo_file_rule {
    const char *location;
    repo_file_rule_kind kind;
    const char *expected;
    repo_file_matcher matcher;
} repo_file_rule;

extern const repo_file_rule REPO_FILE_RULES[];
extern const size_t REPO_FILE_RULE_COUNT;

#define REPO_FILE_TOP ""
#define REPO_FILE_SETTINGS "/properties/settings"
#define REPO_FILE_REGISTRIES "/properties/registries"
#define REPO_FILE_DEPENDENCY "/properties/dependencies/items"
#define REPO_FILE_ORG "/definitions/org"
#define REPO_FILE_ORG_MEMBERSHIP "/definitions/orgMembership"
#define REPO_FILE_ORG_REFERENCE "/definitions/orgReference"
#define REPO_FILE_SUBORG "/definitions/suborg"

/** Whether the object at `object_location` (empty for the file itself) names `name` under its `properties`. */
int repo_file_rules_allow_member(const char *object_location, const char *name);

#endif
