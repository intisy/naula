#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "cJSON.h"
#include "memory_testing.h"
#include "naula/repo_file.h"

#ifndef NAULA_REPO_FILE_FIXTURES
#error "NAULA_REPO_FILE_FIXTURES names test/fixtures/repo-file"
#endif
#ifndef NAULA_REPO_FILE_FORMAT_ONE
#error "NAULA_REPO_FILE_FORMAT_ONE names the format 1 fixture"
#endif

static const char *const VALID_FIXTURES[] = {"valid",         "valid-fork",        "valid-local",   "valid-org",
                                             "valid-top-org", "org-without-rules", "url-dependency"};
#define VALID_FIXTURE_COUNT (sizeof VALID_FIXTURES / sizeof VALID_FIXTURES[0])

typedef struct file_bytes {
    uint8_t *bytes;
    size_t length;
} file_bytes;

static file_bytes read_path(const char *path) {
    file_bytes read = {NULL, 0};
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
    read.length = fread(read.bytes, 1, (size_t)size, stream);
    read.bytes[read.length] = '\0';
    fclose(stream);
    return read;
}

static file_bytes read_fixture(const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s/repo.json", NAULA_REPO_FILE_FIXTURES, name);
    return read_path(path);
}

static void start(naula_repo_file *file, naula_refusal *refusal) {
    memset(file, 0, sizeof *file);
    file->size = sizeof *file;
    memset(refusal, 0, sizeof *refusal);
    refusal->size = sizeof *refusal;
}

static naula_status parse_text(const char *text, naula_repo_file *file, naula_refusal *refusal) {
    start(file, refusal);
    return naula_repo_file_parse((const uint8_t *)text, strlen(text), file, refusal);
}

static naula_status parse_fixture(const char *name, naula_repo_file *file, naula_refusal *refusal) {
    file_bytes read = read_fixture(name);
    start(file, refusal);
    naula_status status = naula_repo_file_parse(read.bytes, read.length, file, refusal);
    if (status != NAULA_OK) {
        fprintf(stderr, "%s: %s: %s\n", name, refusal->code, refusal->detail);
    }
    free(read.bytes);
    return status;
}

static void every_valid_fixture_parses(void) {
    for (size_t index = 0; index < VALID_FIXTURE_COUNT; index++) {
        naula_repo_file file;
        naula_refusal refusal;
        CHECK(parse_fixture(VALID_FIXTURES[index], &file, &refusal) == NAULA_OK);
        CHECK(file.format == 2 && file.name != NULL && file.import_name != NULL);
        naula_repo_file_release(&file);
    }
}

static void the_valid_fixture_reads_every_member(void) {
    naula_repo_file file;
    naula_refusal refusal;
    if (parse_fixture("valid", &file, &refusal) != NAULA_OK) {
        check_failures++;
        return;
    }
    check_text(file.name, "naula", "name");
    check_text(file.import_name, "naula", "importName");
    CHECK(file.home_count == 1 && file.homes[0].kind == NAULA_HOME_PROVIDER);
    check_text(file.homes[0].account, "intisy", "home account");
    CHECK(file.id == NULL);
    CHECK(file.metadata.present ==
          (NAULA_METADATA_DESCRIPTION | NAULA_METADATA_TOPICS | NAULA_METADATA_HOMEPAGE | NAULA_METADATA_SETTINGS));
    CHECK(file.metadata.topic_count == 2);
    check_text(file.metadata.topics[1], "git", "second topic");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(file.metadata.settings, "issues")));
    check_text(file.registry_npm, "@intisy", "npm registry");
    check_text(file.registry_maven, "com.intisy", "maven registry");
    CHECK(file.dependency_count == 2);
    naula_dependency *second = &file.dependencies[1];
    check_text(second->reference.id,
               "git:4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f/plugins/runtime", "second id");
    CHECK(second->reference.at_count == 2 && second->reference.at[0].number == 12);
    check_text(second->reference.at[1].domain, "naula.dev", "web place");
    check_text(second->path, "siblings/library/siblings/runtime", "path");
    CHECK(file.dependencies[0].path == NULL);
    CHECK(file.org == NULL);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(file.access, "readers")) == 1);
    naula_repo_file_release(&file);
}

static void the_fork_fixture_states_its_own_id(void) {
    naula_repo_file file;
    naula_refusal refusal;
    if (parse_fixture("valid-fork", &file, &refusal) != NAULA_OK) {
        check_failures++;
        return;
    }
    check_text(file.id, "git:3ac5a2bb587e0f1d2c3b4a5968778695a4b3c2d1", "fork id");
    CHECK(file.home_count == 4 && file.homes[1].kind == NAULA_HOME_PROVIDER && file.homes[2].kind == NAULA_HOME_WEB &&
          file.homes[3].kind == NAULA_HOME_URL);
    CHECK(file.metadata.present == NAULA_METADATA_TOPICS && file.metadata.topic_count == 0);
    naula_repo_file_release(&file);
}

static void the_org_fixture_reads_its_declaration(void) {
    naula_repo_file file;
    naula_refusal refusal;
    if (parse_fixture("valid-org", &file, &refusal) != NAULA_OK) {
        check_failures++;
        return;
    }
    naula_org_declaration *org = file.org;
    check_text(org->name, "spisor", "org name");
    CHECK(org->parent != NULL && org->parent->at_count == 1 && org->parent->at[0].number == 1000000001u);
    check_text(org->rules, "RULES.md", "rules");
    CHECK(org->project_count == 2);
    check_text(org->project_names[1], "platform", "second project");
    check_text(org->project_descriptions[0], "What any spisor application uses.", "first project");
    CHECK(org->suborg_count == 1 && org->hold_count == 0);
    check_text(org->suborgs[0].name, "forebay", "suborg name");
    check_text(org->suborgs[0].rules, "forebay/CLAUDE.md", "suborg rules");
    CHECK(org->suborgs[0].parent == NULL && org->suborgs[0].account_count == 1);
    naula_repo_file_release(&file);
}

static void the_top_org_fixture_holds_orgs(void) {
    naula_repo_file file;
    naula_refusal refusal;
    if (parse_fixture("valid-top-org", &file, &refusal) != NAULA_OK) {
        check_failures++;
        return;
    }
    CHECK(file.org->parent == NULL && file.org->account_count == 2 && file.org->hold_count == 2);
    check_text(file.org->hold[1].url, "https://github.com/weivel/records", "hold by url");
    naula_repo_file_release(&file);
}

static void a_local_repo_needs_no_home(void) {
    naula_repo_file file;
    naula_refusal refusal;
    if (parse_fixture("valid-local", &file, &refusal) != NAULA_OK) {
        check_failures++;
        return;
    }
    check_text(file.id, "local:scratch", "local id");
    CHECK(file.home_count == 0 && file.homes == NULL);
    naula_repo_file_release(&file);
}

static void org_without_rules_parses_with_rules_null(void) {
    naula_repo_file file;
    naula_refusal refusal;
    if (parse_fixture("org-without-rules", &file, &refusal) != NAULA_OK) {
        check_failures++;
        return;
    }
    CHECK(file.org != NULL && file.org->rules == NULL);
    check_text(file.org->name, "pinaxis", "org name");
    naula_repo_file_release(&file);
}

static void url_dependency_parses_with_url_set(void) {
    naula_repo_file file;
    naula_refusal refusal;
    if (parse_fixture("url-dependency", &file, &refusal) != NAULA_OK) {
        check_failures++;
        return;
    }
    CHECK(file.dependency_count == 1 && file.dependencies[0].reference.id == NULL &&
          file.dependencies[0].reference.at_count == 0);
    check_text(file.dependencies[0].reference.url, "https://github.com/intisy/workflows", "dependency url");
    check_text(file.dependencies[0].path, ".workflows", "a path outside siblings");
    naula_repo_file_release(&file);
}

static void expect_refused(naula_status status, const naula_repo_file *file, const naula_refusal *refusal,
                           const char *member, const char *label) {
    if (status != NAULA_REFUSED) {
        fprintf(stderr, "%s: expected a refusal, got status %d\n", label, (int)status);
        check_failures++;
        return;
    }
    check_text(refusal->code, "repo-file-invalid", label);
    if (strstr(refusal->detail, member) == NULL) {
        fprintf(stderr, "%s: the detail \"%s\" does not name %s\n", label, refusal->detail, member);
        check_failures++;
    }
    CHECK(refusal->fix != NULL && refusal->fix[0] != '\0');
    CHECK(file->name == NULL && file->homes == NULL && file->dependencies == NULL && file->org == NULL);
}

static void refused_fixtures_name_the_member(void) {
    static const struct {
        const char *fixture;
        const char *member;
    } cases[] = {{"missing-homes", "homes"},
                 {"forge-id", "dependencies[0].id"},
                 {"string-home", "homes[0]"},
                 {"place-present", "\"place\""},
                 {"claims-present", "\"claims\""}};
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; index++) {
        naula_repo_file file;
        naula_refusal refusal;
        file_bytes read = read_fixture(cases[index].fixture);
        start(&file, &refusal);
        naula_status status = naula_repo_file_parse(read.bytes, read.length, &file, &refusal);
        expect_refused(status, &file, &refusal, cases[index].member, cases[index].fixture);
        free(read.bytes);
    }
}

#define FILE_HEAD "{ \"format\": 2, \"name\": \"a\", \"importName\": \"a\", "
#define GITHUB_HOME "\"homes\": [ { \"provider\": \"github\", \"account\": \"intisy\" } ]"
#define HEX40 "0d62d7fd7ac5127a0ad5613bd9fa3643acb3cb39"
#define GIT_ID "\"git:" HEX40 "\""

static void expect_text_refused(const char *text, const char *member, const char *label) {
    naula_repo_file file;
    naula_refusal refusal;
    naula_status status = parse_text(text, &file, &refusal);
    expect_refused(status, &file, &refusal, member, label);
    if (status == NAULA_OK) {
        naula_repo_file_release(&file);
    }
}

static void members_out_of_shape_are_refused(void) {
    expect_text_refused(FILE_HEAD GITHUB_HOME ", \"name\": \"b\" }", "\"name\"", "a member written twice");
    expect_text_refused(FILE_HEAD GITHUB_HOME ", \"hosts\": [ \"github:intisy\" ] }", "\"hosts\"", "hosts");
    expect_text_refused(FILE_HEAD "\"homes\": [] }", "homes", "an empty homes");
    expect_text_refused(FILE_HEAD GITHUB_HOME ", \"id\": \"local:a\" }", "homes", "homes beside a local id");
    expect_text_refused(FILE_HEAD GITHUB_HOME ", \"id\": \"git:" HEX40 "/plugin\" }", "id", "a repo id with a segment");
    expect_text_refused("{ \"format\": 2, \"importName\": \"a\", " GITHUB_HOME " }", "name", "no name");
    expect_text_refused("{ \"format\": 2, \"name\": \"a/b\", \"importName\": \"a\", " GITHUB_HOME " }", "name",
                        "a name with a slash");
    expect_text_refused("{ \"format\": 2, \"name\": \"a\", \"importName\": \"A\", " GITHUB_HOME " }", "importName",
                        "an uppercase importName");
    expect_text_refused(FILE_HEAD GITHUB_HOME ", \"topics\": [ \"Tools\" ] }", "topics[0]", "an uppercase topic");
    expect_text_refused(FILE_HEAD GITHUB_HOME ", \"settings\": { \"pages\": true } }", "settings", "a settings member");
    expect_text_refused(FILE_HEAD GITHUB_HOME ", \"registries\": { \"pypi\": \"x\" } }", "registries",
                        "a registry naula does not know");
}

static void dependencies_out_of_shape_are_refused(void) {
    expect_text_refused(FILE_HEAD GITHUB_HOME ", \"dependencies\": [ { \"id\": " GIT_ID
                                              ", \"importName\": \"b\", \"commit\": \"" HEX40 "\" } ] }",
                        "dependencies[0].at", "a git id with no at");
    expect_text_refused(FILE_HEAD GITHUB_HOME ", \"dependencies\": [ { \"id\": \"local:b\", \"at\": [ \"github:1\" ], "
                                              "\"importName\": \"b\", \"commit\": \"" HEX40 "\" } ] }",
                        "dependencies[0].at", "a local id with an at");
    expect_text_refused(FILE_HEAD GITHUB_HOME ", \"dependencies\": [ { \"id\": " GIT_ID ", \"url\": "
                                              "\"https://github.com/a/b\", \"at\": [ \"github:1\" ], "
                                              "\"importName\": \"b\", \"commit\": \"" HEX40 "\" } ] }",
                        "dependencies[0]", "an id beside a url");
    expect_text_refused(FILE_HEAD GITHUB_HOME ", \"dependencies\": [ { \"id\": " GIT_ID ", \"at\": [ \"github:x\" ], "
                                              "\"importName\": \"b\", \"commit\": \"" HEX40 "\" } ] }",
                        "dependencies[0].at[0]", "a place with no number");
    expect_text_refused(FILE_HEAD GITHUB_HOME ", \"dependencies\": [ { \"id\": " GIT_ID ", \"at\": [ \"github:1\" ], "
                                              "\"importName\": \"b\", \"commit\": \"0D62D7FD7AC5127A0AD5613BD9FA3643ACB3CB39\" } ] }",
                        "dependencies[0].commit", "an uppercase commit");
    expect_text_refused(FILE_HEAD GITHUB_HOME ", \"dependencies\": [ { \"id\": " GIT_ID ", \"at\": [ \"github:1\" ], "
                                              "\"importName\": \"b\", \"commit\": \"" HEX40 "\", \"path\": "
                                              "\"siblings/../../b\" } ] }",
                        "dependencies[0].path", "a path out of the repo");
    expect_text_refused(FILE_HEAD GITHUB_HOME ", \"dependencies\": [ { \"id\": " GIT_ID ", \"at\": [ \"github:1\" ], "
                                              "\"importName\": \"b\", \"commit\": \"" HEX40 "\", \"path\": \"/b\" } ] }",
                        "dependencies[0].path", "an absolute path");
}

#define ORG_HEAD FILE_HEAD GITHUB_HOME ", \"org\": { \"name\": \"o\", "
#define PARENT "\"parent\": { \"id\": " GIT_ID ", \"at\": [ \"github:1\" ] }"

static void org_declarations_out_of_shape_are_refused(void) {
    expect_text_refused(ORG_HEAD PARENT ", \"hold\": [ { \"id\": " GIT_ID ", \"at\": [ \"github:1\" ] } ] } }",
                        "org.hold", "hold beside a parent");
    expect_text_refused(ORG_HEAD "\"suborgs\": { \"f\": { " PARENT " } } } }", "org.suborgs.f", "a suborg's parent");
    expect_text_refused(ORG_HEAD "\"accounts\": [ { \"provider\": \"web\", \"domain\": \"naula.dev\" } ] } }",
                        "org.accounts[0]", "a web account");
    expect_text_refused(ORG_HEAD "\"projects\": { \"p\": \"two\\nlines\" } } }", "org.projects.p",
                        "a project on two lines");
    expect_text_refused(FILE_HEAD GITHUB_HOME ", \"org\": { \"rules\": \"RULES.md\" } }", "org.name", "an org with no name");
}

static void parse_never_answers_not_found(void) {
    naula_repo_file file;
    naula_refusal refusal;
    CHECK(parse_text("", &file, &refusal) == NAULA_MALFORMED);
    check_text(refusal.code, "repo-file-not-json", "empty bytes");
    CHECK(parse_text("{ \"format\": 2,", &file, &refusal) == NAULA_MALFORMED);
    CHECK(parse_text("[]", &file, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "repo-file-invalid", "an array");
    CHECK(parse_text("{ \"format\": 1, \"importName\": \"a\" }", &file, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "repo-file-format-one", "format 1");
    CHECK(parse_text("{ \"format\": 3 }", &file, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "repo-file-format", "format 3");
    CHECK(parse_text("{ \"name\": \"a\" }", &file, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "repo-file-format", "no format");
}

static void a_file_struct_smaller_than_its_first_shape_is_refused(void) {
    naula_repo_file file;
    naula_refusal refusal;
    start(&file, &refusal);
    file.size = 8;
    const char *text = FILE_HEAD GITHUB_HOME " }";
    CHECK(naula_repo_file_parse((const uint8_t *)text, strlen(text), &file, &refusal) == NAULA_REFUSED);
    CHECK(file.name == NULL);
}

static int same_text(const char *left, const char *right) {
    return (left == NULL && right == NULL) || (left != NULL && right != NULL && strcmp(left, right) == 0);
}

static int same_home(const naula_home *left, const naula_home *right) {
    return left->kind == right->kind && same_text(left->provider, right->provider) &&
           same_text(left->host, right->host) && same_text(left->account, right->account) &&
           same_text(left->domain, right->domain) && same_text(left->url, right->url);
}

static int same_homes(const naula_home *left, const naula_home *right, size_t count) {
    for (size_t index = 0; index < count; index++) {
        if (!same_home(&left[index], &right[index])) {
            return 0;
        }
    }
    return 1;
}

static int same_place(const naula_place *left, const naula_place *right) {
    return left->kind == right->kind && left->number == right->number && same_text(left->provider, right->provider) &&
           same_text(left->host, right->host) && same_text(left->domain, right->domain) &&
           same_text(left->url, right->url);
}

static int same_reference(const naula_reference *left, const naula_reference *right) {
    if (!same_text(left->id, right->id) || !same_text(left->url, right->url) || left->at_count != right->at_count) {
        return 0;
    }
    for (size_t index = 0; index < left->at_count; index++) {
        if (!same_place(&left->at[index], &right->at[index])) {
            return 0;
        }
    }
    return 1;
}

static int same_texts(char *const *left, char *const *right, size_t count) {
    for (size_t index = 0; index < count; index++) {
        if (!same_text(left[index], right[index])) {
            return 0;
        }
    }
    return 1;
}

static int same_org_parent(const naula_org_declaration *left, const naula_org_declaration *right) {
    if (left->parent == NULL || right->parent == NULL) {
        return left->parent == right->parent;
    }
    return same_reference(left->parent, right->parent);
}

static int same_org(const naula_org_declaration *left, const naula_org_declaration *right) {
    if (left == NULL || right == NULL) {
        return left == right;
    }
    if (!same_text(left->name, right->name) || !same_org_parent(left, right) ||
        left->account_count != right->account_count || !same_homes(left->accounts, right->accounts, left->account_count) ||
        !same_text(left->rules, right->rules) || left->project_count != right->project_count ||
        !same_texts(left->project_names, right->project_names, left->project_count) ||
        !same_texts(left->project_descriptions, right->project_descriptions, left->project_count) ||
        left->suborg_count != right->suborg_count || left->hold_count != right->hold_count) {
        return 0;
    }
    for (size_t index = 0; index < left->suborg_count; index++) {
        if (!same_org(&left->suborgs[index], &right->suborgs[index])) {
            return 0;
        }
    }
    for (size_t index = 0; index < left->hold_count; index++) {
        if (!same_reference(&left->hold[index], &right->hold[index])) {
            return 0;
        }
    }
    return 1;
}

static int same_dependency(const naula_dependency *left, const naula_dependency *right) {
    return same_reference(&left->reference, &right->reference) && same_text(left->import_name, right->import_name) &&
           same_text(left->commit, right->commit) && same_text(left->path, right->path);
}

static int same_json(const cJSON *left, const cJSON *right) {
    if (left == NULL || right == NULL) {
        return left == right;
    }
    return cJSON_Compare(left, right, 1);
}

static int same_metadata(const naula_metadata *left, const naula_metadata *right) {
    return left->present == right->present && same_text(left->description, right->description) &&
           left->topic_count == right->topic_count && same_texts(left->topics, right->topics, left->topic_count) &&
           same_text(left->homepage, right->homepage) && same_json(left->settings, right->settings);
}

static int same_file(const naula_repo_file *left, const naula_repo_file *right) {
    if (!same_text(left->name, right->name) || !same_text(left->import_name, right->import_name) ||
        left->home_count != right->home_count || !same_homes(left->homes, right->homes, left->home_count) ||
        !same_text(left->id, right->id) || !same_metadata(&left->metadata, &right->metadata) ||
        !same_text(left->registry_npm, right->registry_npm) || !same_text(left->registry_maven, right->registry_maven) ||
        left->dependency_count != right->dependency_count || !same_org(left->org, right->org) ||
        !same_json(left->access, right->access)) {
        return 0;
    }
    for (size_t index = 0; index < left->dependency_count; index++) {
        if (!same_dependency(&left->dependencies[index], &right->dependencies[index])) {
            return 0;
        }
    }
    return 1;
}

static void write_then_parse_is_identity(void) {
    for (size_t index = 0; index < VALID_FIXTURE_COUNT; index++) {
        naula_repo_file first;
        naula_repo_file second;
        naula_refusal refusal;
        if (parse_fixture(VALID_FIXTURES[index], &first, &refusal) != NAULA_OK) {
            check_failures++;
            continue;
        }
        char *json = NULL;
        size_t length = 0;
        CHECK(naula_repo_file_write(&first, &json, &length) == NAULA_OK);
        start(&second, &refusal);
        if (json != NULL && naula_repo_file_parse((const uint8_t *)json, length, &second, &refusal) == NAULA_OK) {
            if (!same_file(&first, &second)) {
                fprintf(stderr, "%s: the written file reads back differently\n", VALID_FIXTURES[index]);
                check_failures++;
            }
            naula_repo_file_release(&second);
        } else {
            fprintf(stderr, "%s: the written file does not parse: %s\n", VALID_FIXTURES[index], refusal.detail);
            check_failures++;
        }
        naula_memory_release(json);
        naula_repo_file_release(&first);
    }
}

static void write_is_byte_stable(void) {
    for (size_t index = 0; index < VALID_FIXTURE_COUNT; index++) {
        file_bytes fixture = read_fixture(VALID_FIXTURES[index]);
        naula_repo_file file;
        naula_refusal refusal;
        start(&file, &refusal);
        if (naula_repo_file_parse(fixture.bytes, fixture.length, &file, &refusal) != NAULA_OK) {
            check_failures++;
            free(fixture.bytes);
            continue;
        }
        char *json = NULL;
        size_t length = 0;
        CHECK(naula_repo_file_write(&file, &json, &length) == NAULA_OK);
        if (json == NULL || length != fixture.length || memcmp(json, fixture.bytes, length) != 0) {
            fprintf(stderr, "%s: writing what was read changed bytes:\n%s\n", VALID_FIXTURES[index],
                    json == NULL ? "(nothing)" : json);
            check_failures++;
        }
        CHECK(json == NULL || json[length] == '\0');
        naula_memory_release(json);
        naula_repo_file_release(&file);
        free(fixture.bytes);
    }
}

static void format_one_reads_paths_and_github_ids(void) {
    file_bytes read = read_path(NAULA_REPO_FILE_FORMAT_ONE);
    naula_repo_file file;
    naula_refusal refusal;
    start(&file, &refusal);
    if (naula_repo_file_parse_format_one(read.bytes, read.length, &file, &refusal) != NAULA_OK) {
        fprintf(stderr, "format one: %s: %s\n", refusal.code, refusal.detail);
        check_failures++;
        free(read.bytes);
        return;
    }
    CHECK(file.format == 1 && file.name == NULL);
    check_text(file.import_name, "claude-code-proxy", "format one importName");
    CHECK(file.metadata.topic_count == 9 && (file.metadata.present & NAULA_METADATA_DESCRIPTION) != 0);
    CHECK(file.dependency_count == 13);
    size_t with_path = 0;
    for (size_t index = 0; index < file.dependency_count; index++) {
        naula_reference *reference = &file.dependencies[index].reference;
        CHECK(reference->id != NULL && strncmp(reference->id, "github:", 7) == 0);
        CHECK(reference->at_count == 1 && reference->at[0].kind == NAULA_HOME_PROVIDER);
        check_text(reference->at[0].provider, "github", "format one place");
        with_path += file.dependencies[index].path != NULL;
    }
    CHECK(with_path == 10);
    check_text(file.dependencies[0].reference.id, "github:1427561829", "first id");
    CHECK(file.dependencies[0].reference.at[0].number == 1427561829u);
    check_text(file.dependencies[3].path, "siblings/typescript/siblings/conformance", "fourth path");
    check_text(file.dependencies[12].commit, "0d0ef67e66de32b40ef9c7851dcd8652e93af808", "last commit");
    char *json = NULL;
    size_t length = 0;
    CHECK(naula_repo_file_write(&file, &json, &length) == NAULA_MALFORMED && json == NULL);
    naula_repo_file_release(&file);
    free(read.bytes);
}

static void format_one_keeps_hosts_and_projects(void) {
    const char *text = "{ \"format\": 1, \"importName\": \"docs\", \"place\": \"docs\", \"hosts\": [ \"github:spisor\", "
                       "\"gitea@git.example.com:spisor\" ], \"claims\": [ \"github:forebay\" ], \"projects\": { "
                       "\"shared\": \"What any application uses.\" }, \"dependencies\": [ { \"id\": "
                       "\"github:12/runtime\", \"importName\": \"runtime\", \"commit\": \"" HEX40 "\" } ] }";
    naula_repo_file file;
    naula_refusal refusal;
    start(&file, &refusal);
    if (naula_repo_file_parse_format_one((const uint8_t *)text, strlen(text), &file, &refusal) != NAULA_OK) {
        fprintf(stderr, "format one: %s\n", refusal.detail);
        check_failures++;
        return;
    }
    CHECK(file.home_count == 2 && file.homes[1].kind == NAULA_HOME_PROVIDER);
    check_text(file.homes[1].host, "git.example.com", "format one home host");
    check_text(file.homes[0].account, "spisor", "format one home account");
    CHECK(file.org != NULL && file.org->name == NULL && file.org->project_count == 1);
    check_text(file.org->project_names[0], "shared", "format one project");
    check_text(file.dependencies[0].reference.id, "github:12/runtime", "format one id with a segment");
    CHECK(file.dependencies[0].reference.at[0].number == 12);
    naula_repo_file_release(&file);
    const char *two = "{ \"format\": 2, \"importName\": \"a\" }";
    start(&file, &refusal);
    CHECK(naula_repo_file_parse_format_one((const uint8_t *)two, strlen(two), &file, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "repo-file-format", "format 2 read as format 1");
}

static void write_fails_cleanly_at_every_allocation(const naula_repo_file *file, const char *label) {
    char *json = NULL;
    size_t length = 0;
    naula_memory_fail_at(0);
    CHECK(naula_repo_file_write(file, &json, &length) == NAULA_OK);
    uint64_t attempts = naula_memory_attempts();
    naula_memory_release(json);
    for (uint64_t attempt = 1; attempt <= attempts; attempt++) {
        json = NULL;
        naula_memory_fail_at(attempt);
        naula_status status = naula_repo_file_write(file, &json, &length);
        naula_memory_fail_at(0);
        if (status != NAULA_OUT_OF_MEMORY || json != NULL) {
            fprintf(stderr, "%s: failing write allocation %llu answered %d\n", label, (unsigned long long)attempt,
                    (int)status);
            check_failures++;
            naula_memory_release(json);
        }
    }
}

static void parse_fails_cleanly_at_every_allocation(const file_bytes *fixture, const char *label) {
    naula_repo_file file;
    naula_refusal refusal;
    start(&file, &refusal);
    naula_memory_fail_at(0);
    CHECK(naula_repo_file_parse(fixture->bytes, fixture->length, &file, &refusal) == NAULA_OK);
    uint64_t attempts = naula_memory_attempts();
    write_fails_cleanly_at_every_allocation(&file, label);
    naula_repo_file_release(&file);
    for (uint64_t attempt = 1; attempt <= attempts; attempt++) {
        start(&file, &refusal);
        naula_memory_fail_at(attempt);
        naula_status status = naula_repo_file_parse(fixture->bytes, fixture->length, &file, &refusal);
        naula_memory_fail_at(0);
        if (status != NAULA_OUT_OF_MEMORY || file.name != NULL || file.homes != NULL || file.org != NULL) {
            fprintf(stderr, "%s: failing allocation %llu answered %d\n", label, (unsigned long long)attempt, (int)status);
            check_failures++;
        }
    }
}

static void out_of_memory_is_answered_at_every_allocation(void) {
    for (size_t index = 0; index < VALID_FIXTURE_COUNT; index++) {
        file_bytes fixture = read_fixture(VALID_FIXTURES[index]);
        parse_fails_cleanly_at_every_allocation(&fixture, VALID_FIXTURES[index]);
        free(fixture.bytes);
    }
}

int main(void) {
    every_valid_fixture_parses();
    the_valid_fixture_reads_every_member();
    the_fork_fixture_states_its_own_id();
    the_org_fixture_reads_its_declaration();
    the_top_org_fixture_holds_orgs();
    a_local_repo_needs_no_home();
    org_without_rules_parses_with_rules_null();
    url_dependency_parses_with_url_set();
    refused_fixtures_name_the_member();
    members_out_of_shape_are_refused();
    dependencies_out_of_shape_are_refused();
    org_declarations_out_of_shape_are_refused();
    parse_never_answers_not_found();
    a_file_struct_smaller_than_its_first_shape_is_refused();
    write_then_parse_is_identity();
    write_is_byte_stable();
    format_one_reads_paths_and_github_ids();
    format_one_keeps_hosts_and_projects();
    out_of_memory_is_answered_at_every_allocation();
    return check_summary("repo_file_test");
}
