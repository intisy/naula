#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "check.h"
#include "home_internal.h"
#include "naula/repo_file.h"
#include "repo_file_rules.h"
#include "schema_json.h"
#include "schema_validate.h"

#ifndef NAULA_REPO_SCHEMA
#error "NAULA_REPO_SCHEMA names schema/repo.schema.json"
#endif
#ifndef NAULA_REPO_FILE_FIXTURES
#error "NAULA_REPO_FILE_FIXTURES names test/fixtures/repo-file"
#endif
#ifndef NAULA_REPO_SCHEMA_CASES
#error "NAULA_REPO_SCHEMA_CASES names test/fixtures/repo-file-schema"
#endif

#define LOCATION_CAPACITY 512
#define PROBLEM_CAPACITY 512

static const char *const ANNOTATIONS[] = {"$schema", "$id", "title", "description", "$comment"};

typedef struct fixture_verdict {
    const char *name;
    int accepted;
} fixture_verdict;

static const fixture_verdict FIXTURE_VERDICTS[] = {
    {"valid", 1},          {"valid-fork", 1},     {"valid-local", 1},    {"valid-org", 1},
    {"valid-top-org", 1},  {"org-without-rules", 1}, {"url-dependency", 1}, {"missing-homes", 0},
    {"forge-id", 0},       {"string-home", 0},    {"place-present", 0},  {"claims-present", 0},
};

typedef struct walk {
    int *seen;
    size_t found;
} walk;

typedef struct text_file {
    char *bytes;
    size_t length;
} text_file;

static text_file read_file(const char *path) {
    text_file read = {NULL, 0};
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        check_failures++;
        return read;
    }
    fseek(stream, 0, SEEK_END);
    long size = ftell(stream);
    fseek(stream, 0, SEEK_SET);
    read.bytes = malloc((size_t)size + 1);
    read.length = read.bytes == NULL ? 0 : fread(read.bytes, 1, (size_t)size, stream);
    if (read.bytes != NULL) {
        read.bytes[read.length] = '\0';
    }
    fclose(stream);
    return read;
}

static cJSON *read_json(const char *path) {
    text_file read = read_file(path);
    cJSON *json = read.bytes == NULL ? NULL : schema_json_read(read.bytes, read.length);
    if (read.bytes != NULL && json == NULL) {
        fprintf(stderr, "%s is not JSON\n", path);
        check_failures++;
    }
    free(read.bytes);
    return json;
}

static int is_annotation(const char *keyword) {
    for (size_t index = 0; index < sizeof ANNOTATIONS / sizeof ANNOTATIONS[0]; index++) {
        if (strcmp(keyword, ANNOTATIONS[index]) == 0) {
            return 1;
        }
    }
    return 0;
}

static void extend(char *buffer, const char *location, const char *step) {
    int written = snprintf(buffer, LOCATION_CAPACITY, "%s/%s", location, step);
    CHECK(written > 0 && written < LOCATION_CAPACITY);
}

static const repo_file_rule *rule_at(const char *location, size_t *index) {
    for (size_t at = 0; at < REPO_FILE_RULE_COUNT; at++) {
        if (strcmp(REPO_FILE_RULES[at].location, location) == 0) {
            *index = at;
            return &REPO_FILE_RULES[at];
        }
    }
    return NULL;
}

static const char *kind_name(repo_file_rule_kind kind) {
    static const char *const names[] = {"", "constraint", "pattern", "member", "required name"};
    return kind >= REPO_FILE_RULE_VALUE && kind <= REPO_FILE_RULE_REQUIRED ? names[kind] : "?";
}

/** An expected value that reads as JSON (`false`, `1`, `{}`) is compared as JSON, kind included; any other as text. */
static int value_is_expected(const cJSON *value, const char *expected) {
    cJSON *parsed = cJSON_ParseWithOpts(expected, NULL, 1);
    int equal;
    if (parsed == NULL) {
        return cJSON_IsString(value) && strcmp(value->valuestring, expected) == 0;
    }
    equal = schema_json_equal(parsed, value);
    cJSON_Delete(parsed);
    return equal;
}

static void record(walk *state, const char *location, repo_file_rule_kind kind, const cJSON *value) {
    size_t index = 0;
    const repo_file_rule *rule = rule_at(location, &index);
    state->found++;
    if (rule == NULL) {
        fprintf(stderr, "the schema states the %s %s, which the parser's rule table does not hold\n", kind_name(kind),
                location);
        check_failures++;
        return;
    }
    state->seen[index]++;
    if (rule->kind != kind) {
        fprintf(stderr, "%s is a %s in the schema and a %s in the rule table\n", location, kind_name(kind),
                kind_name(rule->kind));
        check_failures++;
    } else if ((kind == REPO_FILE_RULE_VALUE || kind == REPO_FILE_RULE_PATTERN) &&
               !value_is_expected(value, rule->expected)) {
        char *printed = cJSON_PrintUnformatted(value);
        fprintf(stderr, "the schema's %s is %s, and the parser holds %s\n", location, printed == NULL ? "?" : printed,
                rule->expected);
        cJSON_free(printed);
        check_failures++;
    }
}

static void walk_schema(walk *state, const cJSON *schema, const char *location);

static void walk_members(walk *state, const cJSON *members, const char *location, int recorded) {
    char member[LOCATION_CAPACITY];
    CHECK(cJSON_IsObject(members));
    for (const cJSON *each = members->child; each != NULL; each = each->next) {
        extend(member, location, each->string);
        if (recorded) {
            record(state, member, REPO_FILE_RULE_MEMBER, NULL);
        }
        walk_schema(state, each, member);
    }
}

static void walk_required(walk *state, const cJSON *required, const char *location) {
    char name[LOCATION_CAPACITY];
    CHECK(cJSON_IsArray(required));
    for (const cJSON *each = required->child; each != NULL; each = each->next) {
        CHECK(cJSON_IsString(each));
        extend(name, location, cJSON_IsString(each) ? each->valuestring : "");
        record(state, name, REPO_FILE_RULE_REQUIRED, NULL);
    }
}

static void walk_branches(walk *state, const cJSON *branches, const char *location) {
    char branch[LOCATION_CAPACITY];
    char index_text[24];
    size_t index = 0;
    CHECK(cJSON_IsArray(branches));
    for (const cJSON *each = branches->child; each != NULL; each = each->next, index++) {
        snprintf(index_text, sizeof index_text, "%zu", index);
        extend(branch, location, index_text);
        walk_schema(state, each, branch);
    }
}

static int is_one_of(const char *keyword, const char *const *names) {
    for (size_t index = 0; names[index] != NULL; index++) {
        if (strcmp(keyword, names[index]) == 0) {
            return 1;
        }
    }
    return 0;
}

static void walk_keyword(walk *state, const cJSON *keyword, const char *location) {
    static const char *const subschemas[] = {"items", "propertyNames", "if", "then", "else", NULL};
    static const char *const combinations[] = {"allOf", "anyOf", "oneOf", NULL};
    char next[LOCATION_CAPACITY];
    extend(next, location, keyword->string);
    if (strcmp(keyword->string, "properties") == 0 || strcmp(keyword->string, "definitions") == 0) {
        walk_members(state, keyword, next, strcmp(keyword->string, "properties") == 0);
    } else if (strcmp(keyword->string, "required") == 0) {
        walk_required(state, keyword, next);
    } else if (is_one_of(keyword->string, combinations)) {
        walk_branches(state, keyword, next);
    } else if (is_one_of(keyword->string, subschemas) ||
               (strcmp(keyword->string, "additionalProperties") == 0 && cJSON_IsObject(keyword))) {
        walk_schema(state, keyword, next);
    } else {
        record(state, next, strcmp(keyword->string, "pattern") == 0 ? REPO_FILE_RULE_PATTERN : REPO_FILE_RULE_VALUE,
               keyword);
    }
}

/** Draft-07 ignores every keyword beside a `$ref`, so a rule written there would bind nothing. */
static void check_reference_stands_alone(const cJSON *schema, const char *location) {
    int keywords = 0;
    for (const cJSON *keyword = schema->child; keyword != NULL; keyword = keyword->next) {
        keywords += !is_annotation(keyword->string);
    }
    if (cJSON_GetObjectItemCaseSensitive(schema, "$ref") != NULL && keywords != 1) {
        fprintf(stderr, "%s writes a rule beside a $ref, which draft-07 ignores\n", location);
        check_failures++;
    }
}

static void check_object_is_closed(const cJSON *schema, const char *location) {
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(schema, "type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "object") == 0 &&
        cJSON_GetObjectItemCaseSensitive(schema, "additionalProperties") == NULL) {
        fprintf(stderr, "%s is an object that leaves additionalProperties open\n", location[0] == '\0' ? "/" : location);
        check_failures++;
    }
}

static void walk_schema(walk *state, const cJSON *schema, const char *location) {
    if (!cJSON_IsObject(schema)) {
        fprintf(stderr, "%s is not a schema object\n", location);
        check_failures++;
        return;
    }
    check_reference_stands_alone(schema, location);
    check_object_is_closed(schema, location);
    for (const cJSON *keyword = schema->child; keyword != NULL; keyword = keyword->next) {
        if (!is_annotation(keyword->string)) {
            walk_keyword(state, keyword, location);
        }
    }
}

static void every_rule_is_stated_once(const walk *state) {
    for (size_t index = 0; index < REPO_FILE_RULE_COUNT; index++) {
        if (state->seen[index] != 1) {
            fprintf(stderr, "the parser's rule table holds the %s %s, which the schema states %d times\n",
                    kind_name(REPO_FILE_RULES[index].kind), REPO_FILE_RULES[index].location, state->seen[index]);
            check_failures++;
        }
    }
    CHECK(state->found == REPO_FILE_RULE_COUNT);
}

static void every_pattern_has_its_matcher(void) {
    for (size_t index = 0; index < REPO_FILE_RULE_COUNT; index++) {
        const repo_file_rule *rule = &REPO_FILE_RULES[index];
        if ((rule->kind == REPO_FILE_RULE_PATTERN) != (rule->matcher != NULL)) {
            fprintf(stderr, "%s binds %s matcher\n", rule->location, rule->matcher == NULL ? "no" : "a stray");
            check_failures++;
        }
    }
}

static const char *const HOME_OBJECTS[] = {"/definitions/providerHome/properties/", "/definitions/webHome/properties/",
                                           "/definitions/urlHome/properties/"};
static const char *const HOME_MEMBERS[] = {REPO_FILE_HOME_PROVIDER, REPO_FILE_HOME_HOST, REPO_FILE_HOME_ACCOUNT,
                                           REPO_FILE_HOME_DOMAIN, REPO_FILE_HOME_URL};

static int is_home_member_in_table(const char *name) {
    for (size_t index = 0; index < sizeof HOME_OBJECTS / sizeof HOME_OBJECTS[0]; index++) {
        char location[LOCATION_CAPACITY];
        size_t at = 0;
        snprintf(location, sizeof location, "%s%s", HOME_OBJECTS[index], name);
        if (rule_at(location, &at) != NULL && REPO_FILE_RULES[at].kind == REPO_FILE_RULE_MEMBER) {
            return 1;
        }
    }
    return 0;
}

/** A home is read member by member in home.c, so its names must be exactly the members the three home shapes name. */
static void home_members_are_the_tables(void) {
    for (size_t index = 0; index < sizeof HOME_MEMBERS / sizeof HOME_MEMBERS[0]; index++) {
        if (!naula_home_names_member(HOME_MEMBERS[index]) || !is_home_member_in_table(HOME_MEMBERS[index])) {
            fprintf(stderr, "the home member %s is not both read by home.c and named by the table\n",
                    HOME_MEMBERS[index]);
            check_failures++;
        }
    }
    for (size_t rule = 0; rule < REPO_FILE_RULE_COUNT; rule++) {
        for (size_t index = 0; index < sizeof HOME_OBJECTS / sizeof HOME_OBJECTS[0]; index++) {
            size_t length = strlen(HOME_OBJECTS[index]);
            const char *name = REPO_FILE_RULES[rule].location + length;
            if (REPO_FILE_RULES[rule].kind == REPO_FILE_RULE_MEMBER &&
                strncmp(REPO_FILE_RULES[rule].location, HOME_OBJECTS[index], length) == 0 &&
                !naula_home_names_member(name)) {
                fprintf(stderr, "the table names the home member %s, which home.c does not read\n", name);
                check_failures++;
            }
        }
    }
}

static int ends_with(const char *text, const char *suffix) {
    size_t length = strlen(text);
    size_t suffix_length = strlen(suffix);
    return length >= suffix_length && strcmp(text + length - suffix_length, suffix) == 0;
}

/**
 * A vector holding a NUL never reaches a matcher, since naula_json_parse refuses the NUL first: the schema alone must
 * refuse it, and a document carrying one proves the parser does.
 */
static int holds_a_nul(const cJSON *vector) {
    return schema_json_string_length(vector) != strlen(vector->valuestring);
}

static void check_vector(const cJSON *schema, const cJSON *subschema, const repo_file_rule *rule, const cJSON *vector,
                         int expected) {
    char problem[PROBLEM_CAPACITY];
    const char *text = vector->valuestring;
    schema_verdict verdict = schema_validate(schema, subschema, vector, problem, sizeof problem);
    int matched = holds_a_nul(vector) ? 0 : rule->matcher(text);
    if (verdict == SCHEMA_UNSUPPORTED) {
        fprintf(stderr, "%s: the schema cannot be read here: %s\n", rule->location, problem);
        check_failures++;
        return;
    }
    if ((verdict == SCHEMA_ACCEPTED) != expected || matched != expected) {
        fprintf(stderr, "%s: \"%.80s\" should be %s; the schema %s it and the parser's matcher %s it\n", rule->location,
                text, expected ? "accepted" : "refused", verdict == SCHEMA_ACCEPTED ? "accepts" : "refuses",
                matched ? "accepts" : "refuses");
        check_failures++;
    }
}

static void check_vector_list(const cJSON *schema, const cJSON *subschema, const repo_file_rule *rule,
                              const cJSON *list, int expected) {
    CHECK(cJSON_IsArray(list) && cJSON_GetArraySize(list) > 0);
    for (const cJSON *text = list == NULL ? NULL : list->child; text != NULL; text = text->next) {
        CHECK(cJSON_IsString(text));
        if (cJSON_IsString(text)) {
            check_vector(schema, subschema, rule, text, expected);
        }
    }
}

static void every_matcher_agrees_with_its_pattern(const cJSON *schema, const cJSON *vectors) {
    char owner[LOCATION_CAPACITY];
    size_t patterns = 0;
    for (size_t index = 0; index < REPO_FILE_RULE_COUNT; index++) {
        const repo_file_rule *rule = &REPO_FILE_RULES[index];
        if (rule->kind != REPO_FILE_RULE_PATTERN || rule->matcher == NULL || !ends_with(rule->location, "/pattern")) {
            continue;
        }
        patterns++;
        snprintf(owner, sizeof owner, "%.*s", (int)(strlen(rule->location) - strlen("/pattern")), rule->location);
        const cJSON *subschema = schema_at(schema, owner);
        const cJSON *cases = cJSON_GetObjectItemCaseSensitive(vectors, owner);
        if (subschema == NULL || cases == NULL) {
            fprintf(stderr, "%s has no %s\n", owner, subschema == NULL ? "schema" : "vectors");
            check_failures++;
            continue;
        }
        check_vector_list(schema, subschema, rule, cJSON_GetObjectItemCaseSensitive(cases, "accept"), 1);
        check_vector_list(schema, subschema, rule, cJSON_GetObjectItemCaseSensitive(cases, "refuse"), 0);
    }
    CHECK(patterns > 0 && (int)patterns == cJSON_GetArraySize(vectors));
}

static int parser_accepts(const char *bytes, size_t length, char *problem, size_t capacity) {
    naula_repo_file file;
    naula_refusal refusal;
    memset(&file, 0, sizeof file);
    file.size = sizeof file;
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    naula_status status = naula_repo_file_parse((const uint8_t *)bytes, length, &file, &refusal);
    snprintf(problem, capacity, "%s", status == NAULA_OK ? "accepted" : refusal.detail);
    if (status == NAULA_OK) {
        naula_repo_file_release(&file);
    }
    return status == NAULA_OK;
}

static void check_document(const cJSON *schema, const char *name, const char *bytes, size_t length, int expected) {
    char parser_problem[1024];
    char schema_problem[PROBLEM_CAPACITY];
    int parsed = parser_accepts(bytes, length, parser_problem, sizeof parser_problem);
    cJSON *document = schema_json_read(bytes, length);
    schema_verdict verdict =
        document == NULL ? SCHEMA_REFUSED
                         : schema_validate(schema, schema, document, schema_problem, sizeof schema_problem);
    cJSON_Delete(document);
    if (verdict == SCHEMA_UNSUPPORTED) {
        fprintf(stderr, "%s: the schema cannot be read: %s\n", name, schema_problem);
        check_failures++;
    } else if (parsed != expected || (verdict == SCHEMA_ACCEPTED) != expected) {
        fprintf(stderr, "%s should be %s; the parser says %s and the schema %s it\n", name,
                expected ? "accepted" : "refused", parser_problem, verdict == SCHEMA_ACCEPTED ? "accepts" : "refuses");
        check_failures++;
    }
}

static void every_fixture_keeps_its_verdict(const cJSON *schema) {
    char path[1024];
    for (size_t index = 0; index < sizeof FIXTURE_VERDICTS / sizeof FIXTURE_VERDICTS[0]; index++) {
        snprintf(path, sizeof path, "%s/%s/repo.json", NAULA_REPO_FILE_FIXTURES, FIXTURE_VERDICTS[index].name);
        text_file read = read_file(path);
        if (read.bytes != NULL) {
            check_document(schema, FIXTURE_VERDICTS[index].name, read.bytes, read.length,
                           FIXTURE_VERDICTS[index].accepted);
        }
        free(read.bytes);
    }
}

static void every_document_has_one_verdict(const cJSON *schema, const cJSON *documents) {
    CHECK(cJSON_IsArray(documents) && cJSON_GetArraySize(documents) > 0);
    for (const cJSON *each = documents == NULL ? NULL : documents->child; each != NULL; each = each->next) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(each, "name");
        const cJSON *accepted = cJSON_GetObjectItemCaseSensitive(each, "accepted");
        const cJSON *written = cJSON_GetObjectItemCaseSensitive(each, "text");
        const cJSON *document = cJSON_GetObjectItemCaseSensitive(each, "document");
        char *text = cJSON_IsString(written) ? NULL : cJSON_PrintUnformatted(document);
        const char *bytes = cJSON_IsString(written) ? written->valuestring : text;
        CHECK(cJSON_IsString(name) && cJSON_IsBool(accepted) && bytes != NULL);
        if (cJSON_IsString(name) && cJSON_IsBool(accepted) && bytes != NULL) {
            check_document(schema, name->valuestring, bytes, strlen(bytes), cJSON_IsTrue(accepted));
        }
        cJSON_free(text);
    }
}

static void the_schema_names_its_draft(const cJSON *schema) {
    const cJSON *draft = cJSON_GetObjectItemCaseSensitive(schema, "$schema");
    check_text(cJSON_IsString(draft) ? draft->valuestring : NULL, "http://json-schema.org/draft-07/schema#",
               "$schema");
}

int main(void) {
    cJSON *schema = read_json(NAULA_REPO_SCHEMA);
    cJSON *vectors = read_json(NAULA_REPO_SCHEMA_CASES "/vectors.json");
    cJSON *documents = read_json(NAULA_REPO_SCHEMA_CASES "/documents.json");
    walk state = {calloc(REPO_FILE_RULE_COUNT, sizeof(int)), 0};
    if (schema == NULL || vectors == NULL || documents == NULL || state.seen == NULL) {
        fprintf(stderr, "cannot read the schema and its cases\n");
        return 1;
    }
    the_schema_names_its_draft(schema);
    walk_schema(&state, schema, "");
    every_rule_is_stated_once(&state);
    every_pattern_has_its_matcher();
    home_members_are_the_tables();
    every_matcher_agrees_with_its_pattern(schema, vectors);
    every_fixture_keeps_its_verdict(schema);
    every_document_has_one_verdict(schema, documents);
    free(state.seen);
    cJSON_Delete(documents);
    cJSON_Delete(vectors);
    cJSON_Delete(schema);
    return check_summary("repo_file_schema_parity_test");
}
