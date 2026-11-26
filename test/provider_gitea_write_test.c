#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "http_fixture.h"
#include "naula/json.h"
#include "naula/memory.h"
#include "naula/metadata.h"
#include "naula/provider.h"
#include "provider_gitea_hooks.h"
#include "transport_hooks.h"

#ifndef NAULA_FORGEJO_FIXTURES
#error "NAULA_FORGEJO_FIXTURES names test/fixtures/forgejo"
#endif

#define HOST "codeberg.org"
#define NUMBER 16490u
#define TOKEN_TEXT "not-a-real-token"
#define NOT_FOUND_BODY "{\"message\":\"The target couldn't be found.\",\"errors\":[]}"
#define REPOSITORY_ROUTE "/repositories/16490"
#define PREFIX "/repos/Codeberg/pages-server"
#define VERSION_ROUTE "/version"
#define CODEBERG_VERSION "{\"version\":\"17.0.0-121-39320c0+gitea-1.22.0\"}"
#define RENAMED "{\"id\":16490,\"name\":\"renamed\",\"owner\":{\"login\":\"Codeberg\"},\"archived\":false}"
#define ARCHIVED "{\"id\":16490,\"name\":\"pages-server\",\"owner\":{\"login\":\"Codeberg\"},\"archived\":true}"

static char *repository_body;

static char *load(const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", NAULA_FORGEJO_FIXTURES, name);
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    if (file == NULL) {
        return NULL;
    }
    char *text = naula_memory_allocate(65536);
    size_t length = fread(text, 1, 65535, file);
    text[length] = '\0';
    fclose(file);
    return text;
}

static http_route route(const char *path, int status, const char *body) {
    http_route made;
    memset(&made, 0, sizeof made);
    made.path = path;
    made.status = status;
    made.body = body;
    made.body_length = strlen(body);
    made.declared_length = HTTP_ROUTE_BODY_LENGTH;
    return made;
}

static http_fixture *serve(const http_route *routes, size_t count) {
    http_fixture *server = http_fixture_start(routes, count);
    CHECK(server != NULL);
    if (server != NULL) {
        char root[64];
        snprintf(root, sizeof root, "http://127.0.0.1:%u", (unsigned)http_fixture_port(server));
        naula_gitea_use_api_root(root);
    }
    return server;
}

static char *finish(http_fixture *server) {
    naula_gitea_use_api_root(NULL);
    return http_fixture_stop(server);
}

static naula_refusal fresh_refusal(void) {
    naula_refusal refusal;
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    return refusal;
}

static naula_metadata fresh_metadata(void) {
    naula_metadata metadata;
    memset(&metadata, 0, sizeof metadata);
    metadata.size = sizeof metadata;
    return metadata;
}

static const naula_provider *forgejo(void) {
    return naula_provider_forgejo();
}

static int ends_with(const char *text, const char *end) {
    size_t length = strlen(text);
    size_t end_length = strlen(end);
    return length >= end_length && strcmp(text + length - end_length, end) == 0;
}

/** Whether the log holds the lookup, then the version, then the write, in that order. */
static int looks_up_then_checks_the_version_then_writes(const char *log, const char *write_line) {
    const char *lookup = strstr(log, "GET " REPOSITORY_ROUTE " ");
    const char *version = strstr(log, "GET " VERSION_ROUTE " ");
    const char *write = strstr(log, write_line);
    return lookup != NULL && version != NULL && write != NULL && lookup < version && version < write;
}

static void rename_patches_the_name_by_the_names_the_number_answers(void) {
    http_route routes[3] = {route(REPOSITORY_ROUTE, 200, repository_body), route(VERSION_ROUTE, 200, CODEBERG_VERSION),
                            route(PREFIX, 200, RENAMED)};
    http_fixture *server = serve(routes, 3);
    naula_refusal refusal = fresh_refusal();
    CHECK(forgejo()->rename(forgejo()->context, HOST, NUMBER, "renamed", TOKEN_TEXT, &refusal) == NAULA_OK);
    char *log = finish(server);
    CHECK(log != NULL && looks_up_then_checks_the_version_then_writes(log, "PATCH " PREFIX " HTTP/1.1\r\n"));
    CHECK(log != NULL && ends_with(log, "\r\n\r\n{\"name\":\"renamed\"}"));
    CHECK(log != NULL && strstr(log, "Content-Type: application/json\r\n") != NULL);
    naula_memory_release(log);
}

static void a_422_on_rename_is_refused_with_the_name_taken(void) {
    http_route routes[3] = {route(REPOSITORY_ROUTE, 200, repository_body), route(VERSION_ROUTE, 200, CODEBERG_VERSION),
                            route(PREFIX, 422, "{\"message\":\"repo name is already taken\"}")};
    http_fixture *server = serve(routes, 3);
    naula_refusal refusal = fresh_refusal();
    CHECK(forgejo()->rename(forgejo()->context, HOST, NUMBER, "taken", TOKEN_TEXT, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "name_taken", "a taken name");
    CHECK(strstr(refusal.detail, "Codeberg/taken") != NULL);
    naula_memory_release(finish(server));
}

static void a_host_older_than_gitea_1_20_is_refused_before_any_write(void) {
    http_route routes[3] = {route(REPOSITORY_ROUTE, 200, repository_body),
                            route(VERSION_ROUTE, 200, "{\"version\":\"1.19.4\"}"), route(PREFIX, 200, RENAMED)};
    http_fixture *server = serve(routes, 3);
    naula_refusal refusal = fresh_refusal();
    uint64_t number = 0;
    CHECK(forgejo()->rename(forgejo()->context, HOST, NUMBER, "renamed", TOKEN_TEXT, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "unsupported_operation", "rename on an old host");
    CHECK(forgejo()->archive(forgejo()->context, HOST, NUMBER, TOKEN_TEXT, &refusal) == NAULA_REFUSED);
    CHECK(forgejo()->create(forgejo()->context, HOST, "Codeberg", "naula", NULL, TOKEN_TEXT, &number, &refusal) ==
          NAULA_REFUSED);
    check_text(refusal.code, "unsupported_operation", "create on an old host");
    char *log = finish(server);
    CHECK(log != NULL && strstr(log, "PATCH") == NULL && strstr(log, "POST") == NULL);
    naula_memory_release(log);
}

static void transfer_posts_the_new_owner(void) {
    http_route routes[3] = {route(REPOSITORY_ROUTE, 200, repository_body), route(VERSION_ROUTE, 200, CODEBERG_VERSION),
                            route(PREFIX "/transfer", 202, repository_body)};
    http_fixture *server = serve(routes, 3);
    naula_refusal refusal = fresh_refusal();
    CHECK(forgejo()->transfer(forgejo()->context, HOST, NUMBER, "spisor", TOKEN_TEXT, &refusal) == NAULA_OK);
    char *log = finish(server);
    CHECK(log != NULL && looks_up_then_checks_the_version_then_writes(log, "POST " PREFIX "/transfer HTTP/1.1\r\n"));
    CHECK(log != NULL && ends_with(log, "\r\n\r\n{\"new_owner\":\"spisor\"}"));
    naula_memory_release(log);
}

static void archive_patches_archived_true(void) {
    http_route routes[3] = {route(REPOSITORY_ROUTE, 200, repository_body), route(VERSION_ROUTE, 200, CODEBERG_VERSION),
                            route(PREFIX, 200, ARCHIVED)};
    http_fixture *server = serve(routes, 3);
    naula_refusal refusal = fresh_refusal();
    CHECK(forgejo()->archive(forgejo()->context, HOST, NUMBER, TOKEN_TEXT, &refusal) == NAULA_OK);
    char *log = finish(server);
    CHECK(log != NULL && looks_up_then_checks_the_version_then_writes(log, "PATCH " PREFIX " HTTP/1.1\r\n"));
    CHECK(log != NULL && ends_with(log, "\r\n\r\n{\"archived\":true}"));
    naula_memory_release(log);
}

static void create_posts_to_the_org_or_the_user(void) {
    const char org_answer[] = "{\"id\":91,\"name\":\"naula\",\"owner\":{\"login\":\"Codeberg\"}}";
    const char user_answer[] = "{\"id\":92,\"name\":\"naula\",\"owner\":{\"login\":\"finn\"}}";
    http_route routes[6] = {route(VERSION_ROUTE, 200, CODEBERG_VERSION), route("/orgs/Codeberg", 200, "{}"),
                            route("/orgs/Codeberg/repos", 201, org_answer), route("/orgs/finn", 404, NOT_FOUND_BODY),
                            route("/user", 200, "{\"login\":\"finn\"}"), route("/user/repos", 201, user_answer)};
    http_fixture *server = serve(routes, 6);
    naula_refusal refusal = fresh_refusal();
    uint64_t number = 0;
    CHECK(forgejo()->create(forgejo()->context, HOST, "Codeberg", "naula", NULL, TOKEN_TEXT, &number, &refusal) ==
          NAULA_OK);
    CHECK(number == 91);
    CHECK(forgejo()->create(forgejo()->context, HOST, "finn", "naula", NULL, TOKEN_TEXT, &number, &refusal) ==
          NAULA_OK);
    CHECK(number == 92);
    char *log = finish(server);
    CHECK(log != NULL && strstr(log, "POST /orgs/Codeberg/repos HTTP/1.1\r\n") != NULL);
    CHECK(log != NULL && strstr(log, "POST /user/repos HTTP/1.1\r\n") != NULL);
    naula_memory_release(log);
    routes[2] = route("/orgs/Codeberg/repos", 409, "{\"message\":\"The repository with the same name already exists.\"}");
    server = serve(routes, 6);
    CHECK(forgejo()->create(forgejo()->context, HOST, "Codeberg", "naula", NULL, TOKEN_TEXT, &number, &refusal) ==
          NAULA_REFUSED);
    check_text(refusal.code, "name_taken", "a 409 on create");
    naula_memory_release(finish(server));
}

static void a_404_on_a_write_settles_by_the_number(void) {
    const char moved[] = "{\"id\":16490,\"name\":\"moved\",\"owner\":{\"login\":\"Codeberg\"}}";
    http_route routes[4] = {route(REPOSITORY_ROUTE, 200, repository_body), route(VERSION_ROUTE, 200, CODEBERG_VERSION),
                            route(PREFIX, 404, NOT_FOUND_BODY), route(REPOSITORY_ROUTE, 200, moved)};
    http_fixture *server = serve(routes, 4);
    naula_refusal refusal = fresh_refusal();
    CHECK(forgejo()->archive(forgejo()->context, HOST, NUMBER, TOKEN_TEXT, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "not_permitted", "a 404 on a live repository");
    naula_memory_release(finish(server));
    routes[0].answer_limit = 1;
    server = serve(routes, 4);
    CHECK(forgejo()->archive(forgejo()->context, HOST, NUMBER, TOKEN_TEXT, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "repository_changed", "renamed between the lookup and the write");
    naula_memory_release(finish(server));
    routes[3] = route(REPOSITORY_ROUTE, 404, NOT_FOUND_BODY);
    server = serve(routes, 4);
    CHECK(forgejo()->archive(forgejo()->context, HOST, NUMBER, TOKEN_TEXT, &refusal) == NAULA_NOT_FOUND);
    naula_memory_release(finish(server));
}

static void write_metadata_leaves_an_absent_member_alone(void) {
    http_route routes[4] = {route(REPOSITORY_ROUTE, 200, repository_body), route(VERSION_ROUTE, 200, CODEBERG_VERSION),
                            route(PREFIX, 200, repository_body), route(PREFIX "/topics", 204, "")};
    http_fixture *server = serve(routes, 4);
    naula_refusal refusal = fresh_refusal();
    naula_metadata wanted = fresh_metadata();
    char homepage[] = "https://naula.dev";
    wanted.homepage = homepage;
    wanted.present = NAULA_METADATA_HOMEPAGE;
    uint32_t applied = 0;
    CHECK(forgejo()->write_metadata(forgejo()->context, HOST, NUMBER, &wanted, TOKEN_TEXT, &applied, &refusal) ==
          NAULA_OK);
    CHECK(applied == NAULA_METADATA_HOMEPAGE);
    char *log = finish(server);
    CHECK(log != NULL && ends_with(log, "\r\n\r\n{\"website\":\"https://naula.dev\"}"));
    CHECK(log != NULL && strstr(log, "/topics") == NULL);
    naula_memory_release(log);
    char pages[] = "pages";
    char *topics[] = {pages};
    wanted.topics = topics;
    wanted.topic_count = 1;
    wanted.present = NAULA_METADATA_TOPICS;
    server = serve(routes, 4);
    CHECK(forgejo()->write_metadata(forgejo()->context, HOST, NUMBER, &wanted, TOKEN_TEXT, &applied, &refusal) ==
          NAULA_OK);
    CHECK(applied == NAULA_METADATA_TOPICS);
    log = finish(server);
    CHECK(log != NULL && strstr(log, "PATCH") == NULL && strstr(log, "PUT " PREFIX "/topics HTTP/1.1\r\n") != NULL);
    CHECK(log != NULL && ends_with(log, "\r\n\r\n{\"topics\":[\"pages\"]}"));
    naula_memory_release(log);
}

static void a_failure_part_way_names_what_was_written(void) {
    http_route routes[4] = {route(REPOSITORY_ROUTE, 200, repository_body), route(VERSION_ROUTE, 200, CODEBERG_VERSION),
                            route(PREFIX, 200, repository_body), route(PREFIX "/topics", 403, "{}")};
    http_fixture *server = serve(routes, 4);
    naula_refusal refusal = fresh_refusal();
    naula_metadata wanted = fresh_metadata();
    char description[] = "Words.";
    char pages[] = "pages";
    char *topics[] = {pages};
    wanted.description = description;
    wanted.topics = topics;
    wanted.topic_count = 1;
    wanted.present = NAULA_METADATA_DESCRIPTION | NAULA_METADATA_TOPICS;
    uint32_t applied = 0;
    CHECK(forgejo()->write_metadata(forgejo()->context, HOST, NUMBER, &wanted, TOKEN_TEXT, &applied, &refusal) ==
          NAULA_REFUSED);
    CHECK(applied == NAULA_METADATA_DESCRIPTION);
    check_text(refusal.code, "forbidden_or_rate_limited", "the topics refused");
    naula_memory_release(finish(server));
}

static void read_metadata_reads_by_the_names_the_number_answers(void) {
    http_route routes[4] = {route(REPOSITORY_ROUTE, 200, repository_body), route(VERSION_ROUTE, 200, CODEBERG_VERSION),
                            route(PREFIX, 200, repository_body),
                            route(PREFIX "/topics", 200, "{\"topics\":[\"codeberg\",\"pages\"]}")};
    http_fixture *server = serve(routes, 4);
    naula_refusal refusal = fresh_refusal();
    naula_metadata metadata = fresh_metadata();
    CHECK(forgejo()->read_metadata(forgejo()->context, HOST, NUMBER, TOKEN_TEXT, &metadata, &refusal) == NAULA_OK);
    check_text(metadata.homepage, "https://codeberg.page", "website");
    CHECK(metadata.description != NULL && strstr(metadata.description, "Codeberg Pages Server") != NULL);
    CHECK(metadata.topic_count == 2);
    char *settings = metadata.settings == NULL ? NULL : cJSON_PrintUnformatted(metadata.settings);
    check_text(settings, "{\"visibility\":\"public\",\"defaultBranch\":\"main\",\"issues\":true,\"wiki\":true}",
               "settings");
    cJSON_free(settings);
    naula_metadata_release(&metadata);
    char *log = finish(server);
    const char *topics_read = log == NULL ? NULL : strstr(log, "GET " PREFIX "/topics HTTP/1.1\r\n");
    CHECK(topics_read != NULL);
    CHECK(topics_read != NULL && strstr(topics_read, "GET " REPOSITORY_ROUTE " ") != NULL);
    naula_memory_release(log);
}

static void writes_need_a_token_before_any_request(void) {
    http_route routes[1] = {route(REPOSITORY_ROUTE, 200, "{}")};
    http_fixture *server = serve(routes, 1);
    naula_refusal refusal = fresh_refusal();
    CHECK(forgejo()->rename(forgejo()->context, HOST, NUMBER, "renamed", NULL, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "token_needed", "a rename without a token");
    CHECK(forgejo()->rename(forgejo()->context, HOST, NUMBER, "-renamed", TOKEN_TEXT, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "invalid_request", "a name read as an option");
    char *log = finish(server);
    CHECK(log != NULL && log[0] == '\0');
    naula_memory_release(log);
}

int main(void) {
    naula_json_use_allocator();
    naula_fetch_allow_plain_http(1);
    repository_body = load("repository.json");
    rename_patches_the_name_by_the_names_the_number_answers();
    a_422_on_rename_is_refused_with_the_name_taken();
    a_host_older_than_gitea_1_20_is_refused_before_any_write();
    transfer_posts_the_new_owner();
    archive_patches_archived_true();
    create_posts_to_the_org_or_the_user();
    a_404_on_a_write_settles_by_the_number();
    write_metadata_leaves_an_absent_member_alone();
    a_failure_part_way_names_what_was_written();
    read_metadata_reads_by_the_names_the_number_answers();
    writes_need_a_token_before_any_request();
    naula_memory_release(repository_body);
    return check_summary("provider_gitea_write_test");
}
