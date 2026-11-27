#include "repo_file_rules.h"

#include <stdio.h>
#include <string.h>

#include "home_internal.h"
#include "repo_file_internal.h"

#define TEXT_OF(value) REPO_FILE_TEXT_OF(value)

#define DEVICE "(?:[Cc][Oo][Nn]|[Pp][Rr][Nn]|[Aa][Uu][Xx]|[Nn][Uu][Ll]|[Cc][Oo][Mm][1-9]|[Ll][Pp][Tt][1-9])"
#define FOLDER_CHARACTER "[A-Za-z0-9._-]"
#define PATH_SEGMENT                                                                                                  \
    "(?!" DEVICE "(?:[./]|$))(?!\\.[Gg][Ii][Tt](?:/|$))(?!" FOLDER_CHARACTER "*\\.(?:/|$))" FOLDER_CHARACTER "+"
#define HASH                                                                                                          \
    "(?:[0-9a-f]{" TEXT_OF(REPO_FILE_SHORT_HASH_DIGITS) "}|[0-9a-f]{" TEXT_OF(REPO_FILE_LONG_HASH_DIGITS) "})"
#define LOCAL_NAME "local:[^/ \\t\\n\\r\\v\\f\\x00]+"
#define DOMAIN "(?![.-])[A-Za-z0-9.-]{1," TEXT_OF(REPO_FILE_DOMAIN_LENGTH_LIMIT) "}"
#define PORT "(?::[0-9]{1," TEXT_OF(REPO_FILE_PORT_DIGITS_LIMIT) "})?"
#define PROVIDER "(?=[a-z])[a-z0-9-]{1," TEXT_OF(REPO_FILE_PROVIDER_LENGTH_LIMIT) "}"

#define NAME_PATTERN                                                                                                  \
    "^(?!" DEVICE "(?:\\.|$))(?!\\.[Gg][Ii][Tt]$)(?!" FOLDER_CHARACTER "*\\.$)" FOLDER_CHARACTER                     \
    "{1," TEXT_OF(REPO_FILE_NAME_LENGTH_LIMIT) "}$"
#define IMPORT_NAME_PATTERN "^(?!(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\\.|$))[a-z0-9]+(?:[.-][a-z0-9]+)*$"
#define RELATIVE_PATH_PATTERN "^" PATH_SEGMENT "(?:/" PATH_SEGMENT ")*$"
#define OWN_ID_PATTERN "^(?:git:" HASH "|" LOCAL_NAME ")$"
#define REFERENCE_ID_PATTERN "^(?:git:" HASH "|" LOCAL_NAME ")(?:/(?!\\.{1,2}(?:/|$))[a-z0-9._-]+)*$"
#define NUMBERED_PLACE_PATTERN                                                                                        \
    "^(?!" WEB "[:@])" PROVIDER "(?:@" DOMAIN PORT ")?:(?!0+$)[0-9]{1," TEXT_OF(REPO_FILE_NUMBER_DIGITS_LIMIT) "}$"

#define WEB REPO_FILE_WEB_PROVIDER
#define ENUM_FIRST(name) "[\"" name "\""
#define ENUM_NEXT(name) ", \"" name "\""

#define DEPENDENCY REPO_FILE_DEPENDENCY
#define ORG REPO_FILE_ORG
#define SETTINGS REPO_FILE_SETTINGS
#define REGISTRIES REPO_FILE_REGISTRIES
#define ORG_REFERENCE REPO_FILE_ORG_REFERENCE
#define ORG_MEMBERSHIP REPO_FILE_ORG_MEMBERSHIP
#define SUBORG REPO_FILE_SUBORG
#define DEFINITION "/definitions/"

#define VALUE(location, expected) {location, REPO_FILE_RULE_VALUE, expected, NULL}
#define PATTERN(location, expected, matcher) {location, REPO_FILE_RULE_PATTERN, expected, matcher}
#define MEMBER(location) {location, REPO_FILE_RULE_MEMBER, NULL, NULL}
#define REQUIRED(location) {location, REPO_FILE_RULE_REQUIRED, NULL, NULL}
#define REFERENCE(location, definition) VALUE(location "/$ref", "#/definitions/" definition)
#define MEMBER_OF(location, definition) MEMBER(location), REFERENCE(location, definition)

const repo_file_rule REPO_FILE_RULES[] = {
    VALUE("/type", "object"),
    VALUE("/additionalProperties", "false"),
    REQUIRED("/required/format"),
    REQUIRED("/required/name"),
    REQUIRED("/required/importName"),
    MEMBER("/properties/format"),
    VALUE("/properties/format/const", TEXT_OF(REPO_FILE_FORMAT)),
    MEMBER_OF("/properties/name", "repoName"),
    MEMBER_OF("/properties/importName", "importName"),
    MEMBER("/properties/homes"),
    VALUE("/properties/homes/type", "array"),
    VALUE("/properties/homes/minItems", TEXT_OF(REPO_FILE_LEAST_HOMES)),
    REFERENCE("/properties/homes/items", "home"),
    MEMBER_OF("/properties/id", "ownId"),
    MEMBER_OF("/properties/description", "freeText"),
    MEMBER("/properties/topics"),
    VALUE("/properties/topics/type", "array"),
    VALUE("/properties/topics/maxItems", TEXT_OF(REPO_FILE_TOPIC_COUNT_LIMIT)),
    VALUE("/properties/topics/uniqueItems", "true"),
    REFERENCE("/properties/topics/items", "topic"),
    MEMBER_OF("/properties/homepage", "freeText"),

    MEMBER(SETTINGS),
    VALUE(SETTINGS "/type", "object"),
    VALUE(SETTINGS "/additionalProperties", "false"),
    MEMBER(SETTINGS "/properties/visibility"),
    VALUE(SETTINGS "/properties/visibility/enum", REPO_FILE_VISIBILITIES(ENUM_FIRST, ENUM_NEXT) "]"),
    MEMBER_OF(SETTINGS "/properties/defaultBranch", "branch"),
    MEMBER(SETTINGS "/properties/issues"),
    VALUE(SETTINGS "/properties/issues/type", "boolean"),
    MEMBER(SETTINGS "/properties/wiki"),
    VALUE(SETTINGS "/properties/wiki/type", "boolean"),

    MEMBER(REGISTRIES),
    VALUE(REGISTRIES "/type", "object"),
    VALUE(REGISTRIES "/additionalProperties", "false"),
    MEMBER_OF(REGISTRIES "/properties/npm", "registry"),
    MEMBER_OF(REGISTRIES "/properties/maven", "registry"),

    MEMBER("/properties/dependencies"),
    VALUE("/properties/dependencies/type", "array"),
    VALUE(DEPENDENCY "/type", "object"),
    VALUE(DEPENDENCY "/additionalProperties", "false"),
    REQUIRED(DEPENDENCY "/required/importName"),
    REQUIRED(DEPENDENCY "/required/commit"),
    MEMBER_OF(DEPENDENCY "/properties/id", "referenceId"),
    MEMBER_OF(DEPENDENCY "/properties/url", "gitUrl"),
    MEMBER_OF(DEPENDENCY "/properties/at", "at"),
    MEMBER_OF(DEPENDENCY "/properties/importName", "importName"),
    MEMBER_OF(DEPENDENCY "/properties/commit", "commit"),
    MEMBER_OF(DEPENDENCY "/properties/path", "relativePath"),
    REFERENCE(DEPENDENCY "/allOf/0", "idOrUrl"),

    MEMBER("/properties/org"),
    REFERENCE("/properties/org/oneOf/0", "org"),
    REFERENCE("/properties/org/oneOf/1", "orgMembership"),
    VALUE(ORG "/type", "object"),
    VALUE(ORG "/additionalProperties", "false"),
    REQUIRED(ORG "/required/name"),
    MEMBER_OF(ORG "/properties/name", "importName"),
    MEMBER_OF(ORG "/properties/parent", "orgReference"),
    MEMBER_OF(ORG "/properties/accounts", "accounts"),
    MEMBER_OF(ORG "/properties/rules", "relativePath"),
    MEMBER_OF(ORG "/properties/projects", "projects"),
    MEMBER_OF(ORG "/properties/suborgs", "suborgs"),
    MEMBER(ORG "/properties/hold"),
    VALUE(ORG "/properties/hold/type", "array"),
    REFERENCE(ORG "/properties/hold/items", "orgReference"),
    VALUE(ORG "/if/type", "object"),
    REQUIRED(ORG "/if/required/parent"),
    MEMBER(ORG "/if/properties/parent"),
    VALUE(ORG "/then/type", "object"),
    MEMBER(ORG "/then/properties/hold"),
    VALUE(ORG "/then/properties/hold/not", "{}"),

    VALUE(ORG_MEMBERSHIP "/type", "object"),
    VALUE(ORG_MEMBERSHIP "/additionalProperties", "false"),
    REQUIRED(ORG_MEMBERSHIP "/required/memberOf"),
    MEMBER_OF(ORG_MEMBERSHIP "/properties/memberOf", "orgReference"),

    MEMBER("/properties/access"),

    VALUE("/if/type", "object"),
    REQUIRED("/if/required/id"),
    MEMBER("/if/properties/id"),
    VALUE("/if/properties/id/type", "string"),
    PATTERN("/if/properties/id/pattern", "^local:", repo_file_names_local),
    VALUE("/then/type", "object"),
    MEMBER("/then/properties/homes"),
    VALUE("/then/properties/homes/not", "{}"),
    VALUE("/else/type", "object"),
    REQUIRED("/else/required/homes"),

    VALUE(DEFINITION "freeText/type", "string"),
    PATTERN(DEFINITION "freeText/pattern", "^[^\\x00]*$", repo_file_is_any_text),
    VALUE(DEFINITION "branch/type", "string"),
    PATTERN(DEFINITION "branch/pattern", "^[^\\x00]+$", repo_file_is_branch),
    VALUE(DEFINITION "repoName/type", "string"),
    PATTERN(DEFINITION "repoName/pattern", NAME_PATTERN, repo_file_is_repo_name),
    VALUE(DEFINITION "importName/type", "string"),
    PATTERN(DEFINITION "importName/pattern", IMPORT_NAME_PATTERN, repo_file_is_import_name),
    VALUE(DEFINITION "relativePath/type", "string"),
    VALUE(DEFINITION "relativePath/maxLength", TEXT_OF(REPO_FILE_PATH_LENGTH_LIMIT)),
    PATTERN(DEFINITION "relativePath/pattern", RELATIVE_PATH_PATTERN, repo_file_is_relative_path),
    VALUE(DEFINITION "topic/type", "string"),
    VALUE(DEFINITION "topic/maxLength", TEXT_OF(REPO_FILE_TOPIC_LENGTH_LIMIT)),
    PATTERN(DEFINITION "topic/pattern", "^[a-z0-9][a-z0-9-]*$", repo_file_is_topic),
    VALUE(DEFINITION "registry/type", "string"),
    PATTERN(DEFINITION "registry/pattern", "^[^\\x00-\\x20\\x7f]+$", repo_file_is_registry),
    VALUE(DEFINITION "commit/type", "string"),
    PATTERN(DEFINITION "commit/pattern", "^" HASH "$", repo_file_is_commit),
    VALUE(DEFINITION "ownId/type", "string"),
    PATTERN(DEFINITION "ownId/pattern", OWN_ID_PATTERN, repo_file_is_own_id),
    VALUE(DEFINITION "referenceId/type", "string"),
    PATTERN(DEFINITION "referenceId/pattern", REFERENCE_ID_PATTERN, repo_file_is_reference_id),
    VALUE(DEFINITION "orgReferenceId/type", "string"),
    PATTERN(DEFINITION "orgReferenceId/pattern", "^git:" HASH "$", repo_file_is_org_reference_id),
    VALUE(DEFINITION "gitUrl/type", "string"),
    VALUE(DEFINITION "gitUrl/maxLength", TEXT_OF(REPO_FILE_URL_LENGTH_LIMIT)),
    PATTERN(DEFINITION "gitUrl/pattern", "^(?:https|ssh)://[!-~]+$", naula_home_is_git_url),
    VALUE(DEFINITION "domain/type", "string"),
    PATTERN(DEFINITION "domain/pattern", "^" DOMAIN "$", naula_home_is_domain),
    VALUE(DEFINITION "host/type", "string"),
    PATTERN(DEFINITION "host/pattern", "^" DOMAIN PORT "$", naula_home_is_host),
    VALUE(DEFINITION "account/type", "string"),
    PATTERN(DEFINITION "account/pattern",
            "^(?!\\.{1,2}$)" FOLDER_CHARACTER "{1," TEXT_OF(REPO_FILE_ACCOUNT_LENGTH_LIMIT) "}$",
            naula_home_is_account),
    VALUE(DEFINITION "provider/type", "string"),
    PATTERN(DEFINITION "provider/pattern", "^(?!" WEB "$)" PROVIDER "$", naula_home_is_provider),

    REFERENCE(DEFINITION "home/oneOf/0", "providerHome"),
    REFERENCE(DEFINITION "home/oneOf/1", "webHome"),
    REFERENCE(DEFINITION "home/oneOf/2", "urlHome"),
    VALUE(DEFINITION "providerHome/type", "object"),
    VALUE(DEFINITION "providerHome/additionalProperties", "false"),
    REQUIRED(DEFINITION "providerHome/required/" REPO_FILE_HOME_PROVIDER),
    REQUIRED(DEFINITION "providerHome/required/" REPO_FILE_HOME_ACCOUNT),
    MEMBER_OF(DEFINITION "providerHome/properties/" REPO_FILE_HOME_PROVIDER, "provider"),
    MEMBER_OF(DEFINITION "providerHome/properties/" REPO_FILE_HOME_HOST, "host"),
    MEMBER_OF(DEFINITION "providerHome/properties/" REPO_FILE_HOME_ACCOUNT, "account"),
    VALUE(DEFINITION "webHome/type", "object"),
    VALUE(DEFINITION "webHome/additionalProperties", "false"),
    REQUIRED(DEFINITION "webHome/required/" REPO_FILE_HOME_PROVIDER),
    REQUIRED(DEFINITION "webHome/required/" REPO_FILE_HOME_DOMAIN),
    MEMBER(DEFINITION "webHome/properties/" REPO_FILE_HOME_PROVIDER),
    VALUE(DEFINITION "webHome/properties/" REPO_FILE_HOME_PROVIDER "/const", WEB),
    MEMBER_OF(DEFINITION "webHome/properties/" REPO_FILE_HOME_DOMAIN, "domain"),
    VALUE(DEFINITION "urlHome/type", "object"),
    VALUE(DEFINITION "urlHome/additionalProperties", "false"),
    REQUIRED(DEFINITION "urlHome/required/" REPO_FILE_HOME_URL),
    MEMBER_OF(DEFINITION "urlHome/properties/" REPO_FILE_HOME_URL, "gitUrl"),

    VALUE(DEFINITION "numberedPlace/type", "string"),
    PATTERN(DEFINITION "numberedPlace/pattern", NUMBERED_PLACE_PATTERN, naula_home_is_numbered_place),
    VALUE(DEFINITION "webPlace/type", "string"),
    PATTERN(DEFINITION "webPlace/pattern", "^" WEB ":" DOMAIN "$", naula_home_is_web_place),
    REFERENCE(DEFINITION "place/oneOf/0", "numberedPlace"),
    REFERENCE(DEFINITION "place/oneOf/1", "webPlace"),
    REFERENCE(DEFINITION "place/oneOf/2", "gitUrl"),
    VALUE(DEFINITION "at/type", "array"),
    VALUE(DEFINITION "at/minItems", TEXT_OF(REPO_FILE_LEAST_PLACES)),
    REFERENCE(DEFINITION "at/items", "place"),

    VALUE(DEFINITION "idOrUrl/oneOf/0/type", "object"),
    REQUIRED(DEFINITION "idOrUrl/oneOf/0/required/id"),
    MEMBER(DEFINITION "idOrUrl/oneOf/0/properties/id"),
    VALUE(DEFINITION "idOrUrl/oneOf/1/type", "object"),
    REQUIRED(DEFINITION "idOrUrl/oneOf/1/required/url"),
    MEMBER(DEFINITION "idOrUrl/oneOf/1/properties/url"),
    VALUE(DEFINITION "idOrUrl/if/type", "object"),
    REQUIRED(DEFINITION "idOrUrl/if/required/id"),
    MEMBER(DEFINITION "idOrUrl/if/properties/id"),
    VALUE(DEFINITION "idOrUrl/if/properties/id/type", "string"),
    PATTERN(DEFINITION "idOrUrl/if/properties/id/pattern", "^git:", repo_file_names_git),
    VALUE(DEFINITION "idOrUrl/then/type", "object"),
    REQUIRED(DEFINITION "idOrUrl/then/required/at"),
    MEMBER(DEFINITION "idOrUrl/then/properties/at"),
    VALUE(DEFINITION "idOrUrl/else/type", "object"),
    MEMBER(DEFINITION "idOrUrl/else/properties/at"),
    VALUE(DEFINITION "idOrUrl/else/properties/at/not", "{}"),

    VALUE(ORG_REFERENCE "/type", "object"),
    VALUE(ORG_REFERENCE "/additionalProperties", "false"),
    MEMBER_OF(ORG_REFERENCE "/properties/id", "orgReferenceId"),
    MEMBER_OF(ORG_REFERENCE "/properties/url", "gitUrl"),
    MEMBER_OF(ORG_REFERENCE "/properties/at", "at"),
    REFERENCE(ORG_REFERENCE "/allOf/0", "idOrUrl"),

    VALUE(DEFINITION "accounts/type", "array"),
    REFERENCE(DEFINITION "accounts/items", "providerHome"),
    VALUE(DEFINITION "projectDescription/type", "string"),
    PATTERN(DEFINITION "projectDescription/pattern", "^[^\\x00\\r\\n]+$", repo_file_is_one_line),
    VALUE(DEFINITION "projects/type", "object"),
    REFERENCE(DEFINITION "projects/propertyNames", "importName"),
    REFERENCE(DEFINITION "projects/additionalProperties", "projectDescription"),
    VALUE(DEFINITION "suborgs/type", "object"),
    REFERENCE(DEFINITION "suborgs/propertyNames", "importName"),
    REFERENCE(DEFINITION "suborgs/additionalProperties", "suborg"),
    VALUE(SUBORG "/type", "object"),
    VALUE(SUBORG "/additionalProperties", "false"),
    MEMBER_OF(SUBORG "/properties/accounts", "accounts"),
    MEMBER_OF(SUBORG "/properties/rules", "relativePath"),
    MEMBER_OF(SUBORG "/properties/projects", "projects"),
    MEMBER_OF(SUBORG "/properties/suborgs", "suborgs"),
};

const size_t REPO_FILE_RULE_COUNT = sizeof REPO_FILE_RULES / sizeof REPO_FILE_RULES[0];

/** A name holding `/` or `~` would be escaped in a JSON pointer, so it can never be a member the table names. */
static int is_plain_member_name(const char *name) {
    return strpbrk(name, "/~") == NULL;
}

int repo_file_rules_allow_member(const char *object_location, const char *name) {
    char location[MEMBER_PATH_LIMIT];
    int written = snprintf(location, sizeof location, "%s/properties/%s", object_location, name);
    if (written < 0 || (size_t)written >= sizeof location || !is_plain_member_name(name)) {
        return 0;
    }
    for (size_t index = 0; index < REPO_FILE_RULE_COUNT; index++) {
        const repo_file_rule *rule = &REPO_FILE_RULES[index];
        if (rule->kind == REPO_FILE_RULE_MEMBER && strcmp(rule->location, location) == 0) {
            return 1;
        }
    }
    return 0;
}
