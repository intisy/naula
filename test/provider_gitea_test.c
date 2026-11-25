#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "http_fixture.h"
#include "naula/memory.h"
#include "naula/provider.h"
#include "provider_gitea_hooks.h"
#include "transport_hooks.h"

#ifndef NAULA_FORGEJO_FIXTURES
#error "NAULA_FORGEJO_FIXTURES names test/fixtures/forgejo"
#endif

#define HOST "codeberg.org"
#define NUMBER 16490u
#define COMMIT "28cde84643972ecdacd43a1a4c95bf52d017101f"
#define TIP "e4c0f1faa0210fa0f8e53aff71a31f64720a5caa"
#define TOKEN_TEXT "not-a-real-token"
#define NOT_FOUND_BODY "{\"message\":\"The target couldn't be found.\",\"errors\":[]}"
#define REPOSITORY_ROUTE "/repositories/16490"
#define PREFIX "/repos/Codeberg/pages-server"
#define COMPARE_FORWARD PREFIX "/compare/main..." COMMIT
#define COMPARE_BACKWARD PREFIX "/compare/" COMMIT "...main"
#define RAW_ROUTE PREFIX "/raw/README.md?ref=main"

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

static naula_repository fresh_repository(void) {
    naula_repository repository;
    memset(&repository, 0, sizeof repository);
    repository.size = sizeof repository;
    return repository;
}

static const naula_provider *gitea(void) {
    return naula_provider_gitea();
}

static naula_status lookup_at(const char *host, uint64_t number, const char *token, naula_repository *out,
                              naula_refusal *refusal) {
    return gitea()->lookup(gitea()->context, host, number, token, out, refusal);
}

static naula_status lookup(uint64_t number, const char *token, naula_repository *out, naula_refusal *refusal) {
    return lookup_at(HOST, number, token, out, refusal);
}

static naula_status contains(const char *ref, int *contained, naula_refusal *refusal) {
    return gitea()->contains(gitea()->context, HOST, NUMBER, ref, COMMIT, NULL, contained, refusal);
}

static void lookup_reads_a_repository_with_its_etag(void) {
    char *body = load("repository.json");
    http_route routes[1] = {route(REPOSITORY_ROUTE, 200, body)};
    routes[0].etag = "W/\"abc\"";
    http_fixture *server = serve(routes, 1);
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    CHECK(lookup(NUMBER, NULL, &repository, &refusal) == NAULA_OK);
    CHECK(repository.number == NUMBER && repository.archived == 0 && repository.fork_parent == 0);
    check_text(repository.account, "Codeberg", "account");
    check_text(repository.name, "pages-server", "name");
    check_text(repository.default_branch, "main", "default branch");
    check_text(repository.clone_url, "https://codeberg.org/Codeberg/pages-server.git", "clone url");
    check_text(repository.etag, "W/\"abc\"", "etag");
    naula_repository_release(&repository);
    naula_memory_release(finish(server));
    naula_memory_release(body);
}

static void a_lookup_without_a_token_names_the_fix(void) {
    http_route routes[1] = {route(REPOSITORY_ROUTE, 401, "{\"message\":\"token is required\"}")};
    http_fixture *server = serve(routes, 1);
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    CHECK(lookup(NUMBER, NULL, &repository, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "token_needed", "refusal code");
    CHECK(refusal.fix != NULL && strstr(refusal.fix, "token") != NULL);
    naula_memory_release(finish(server));
}

static void a_lookup_that_names_no_host_is_refused_with_the_fix(void) {
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    CHECK(lookup_at(NULL, NUMBER, NULL, &repository, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "host_needed", "no host");
    CHECK(refusal.fix != NULL && strstr(refusal.fix, "gitea@") != NULL);
    CHECK(lookup_at("", NUMBER, NULL, &repository, &refusal) == NAULA_REFUSED);
    CHECK(lookup_at("bad host", NUMBER, NULL, &repository, &refusal) == NAULA_REFUSED);
}

static void lookup_compares_the_echoed_number(void) {
    char *body = load("repository.json");
    http_route routes[1] = {route("/repositories/60", 200, body)};
    http_fixture *server = serve(routes, 1);
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    CHECK(lookup(60, NULL, &repository, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "number_mismatch", "refusal code");
    CHECK(repository.name == NULL && repository.account == NULL);
    naula_memory_release(finish(server));
    naula_memory_release(body);
}

static void lookup_reads_the_fork_parent(void) {
    const char fork[] = "{\"id\":77,\"name\":\"tea\",\"owner\":{\"login\":\"o\"},\"parent\":{\"id\":60}}";
    http_route routes[1] = {route("/repositories/77", 200, fork)};
    http_fixture *server = serve(routes, 1);
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    CHECK(lookup(77, NULL, &repository, &refusal) == NAULA_OK);
    CHECK(repository.fork_parent == 60);
    naula_repository_release(&repository);
    naula_memory_release(finish(server));
}

static void each_failure_status_is_its_own_answer_and_only_404_is_not_found(void) {
    const int statuses[] = {404, 403, 429, 401, 500};
    const naula_status answers[] = {NAULA_NOT_FOUND, NAULA_REFUSED, NAULA_REFUSED, NAULA_REFUSED, NAULA_REFUSED};
    const char *const codes[] = {"not_found", "forbidden_or_rate_limited", "rate_limited", "token_needed",
                                 "unexpected_status"};
    for (size_t index = 0; index < 5; index++) {
        http_route routes[1] = {route(REPOSITORY_ROUTE, statuses[index], "{}")};
        http_fixture *server = serve(routes, 1);
        naula_repository repository = fresh_repository();
        naula_refusal refusal = fresh_refusal();
        CHECK(lookup(NUMBER, NULL, &repository, &refusal) == answers[index]);
        check_text(refusal.code, codes[index], "status refusal");
        naula_memory_release(finish(server));
    }
}

static void a_timeout_is_timed_out_not_not_found(void) {
    char *body = load("repository.json");
    http_route routes[1] = {route(REPOSITORY_ROUTE, 200, body)};
    routes[0].stall_milliseconds = 1500;
    http_fixture *server = serve(routes, 1);
    naula_fetch_use_deadline(300);
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    CHECK(lookup(NUMBER, NULL, &repository, &refusal) == NAULA_TIMED_OUT);
    CHECK(refusal.fix != NULL);
    naula_fetch_use_deadline(0);
    naula_memory_release(finish(server));
    naula_memory_release(body);
}

static void the_token_is_sent_only_when_given_and_never_repeated(void) {
    http_route routes[1] = {route(REPOSITORY_ROUTE, 403, "{}")};
    http_fixture *server = serve(routes, 1);
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    CHECK(lookup(NUMBER, NULL, &repository, &refusal) == NAULA_REFUSED);
    CHECK(lookup(NUMBER, TOKEN_TEXT, &repository, &refusal) == NAULA_REFUSED);
    CHECK(strstr(refusal.detail, TOKEN_TEXT) == NULL && (refusal.fix == NULL || strstr(refusal.fix, TOKEN_TEXT) == NULL));
    char *log = finish(server);
    CHECK(log != NULL);
    if (log != NULL) {
        const char *second = strstr(log, "GET " REPOSITORY_ROUTE);
        second = second == NULL ? NULL : strstr(second + 1, "GET " REPOSITORY_ROUTE);
        CHECK(second != NULL && strstr(second, "token " TOKEN_TEXT) != NULL);
        CHECK(strstr(log, TOKEN_TEXT) == strstr(second, TOKEN_TEXT));
    }
    naula_memory_release(log);
    refusal = fresh_refusal();
    CHECK(lookup(NUMBER, "has a space", &repository, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "invalid_token", "unusable token");
}

static void every_call_sends_the_accept_header(void) {
    char *body = load("repository.json");
    http_route routes[1] = {route(REPOSITORY_ROUTE, 200, body)};
    http_fixture *server = serve(routes, 1);
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    CHECK(lookup(NUMBER, NULL, &repository, &refusal) == NAULA_OK);
    naula_repository_release(&repository);
    char *log = finish(server);
    CHECK(log != NULL && strstr(log, "Accept: application/json") != NULL);
    naula_memory_release(log);
    naula_memory_release(body);
}

static void find_reads_the_number_and_answers_zero_for_a_missing_or_renamed_name(void) {
    char *body = load("repository.json");
    char location[160];
    http_route routes[4] = {route(PREFIX, 200, body), route("/repos/Codeberg/gone", 404, NOT_FOUND_BODY),
                            route("/repos/Codeberg/old-name", 301, ""), route("/repos/CODEBERG/PAGES-SERVER", 200, body)};
    routes[2].extra_headers = location;
    http_fixture *server = serve(routes, 4);
    snprintf(location, sizeof location, "Location: http://127.0.0.1:%u" PREFIX "\r\n",
             (unsigned)http_fixture_port(server));
    naula_refusal refusal = fresh_refusal();
    uint64_t number = 5;
    CHECK(gitea()->find(gitea()->context, HOST, "Codeberg", "pages-server", NULL, &number, &refusal) == NAULA_OK);
    CHECK(number == NUMBER);
    CHECK(gitea()->find(gitea()->context, HOST, "Codeberg", "gone", NULL, &number, &refusal) == NAULA_OK);
    CHECK(number == 0);
    CHECK(gitea()->find(gitea()->context, HOST, "Codeberg", "old-name", NULL, &number, &refusal) == NAULA_OK);
    CHECK(number == 0);
    CHECK(gitea()->find(gitea()->context, HOST, "CODEBERG", "PAGES-SERVER", NULL, &number, &refusal) == NAULA_OK);
    CHECK(number == NUMBER);
    CHECK(gitea()->find(gitea()->context, HOST, "Codeberg", "../x", NULL, &number, &refusal) == NAULA_REFUSED);
    naula_memory_release(finish(server));
    naula_memory_release(body);
}

static void read_file_reads_the_raw_bytes_and_reverifies_the_number(void) {
    char *body = load("repository.json");
    http_route routes[3] = {route(REPOSITORY_ROUTE, 200, body), route(RAW_ROUTE, 200, "# Pages\n\x01\x02"),
                            route(REPOSITORY_ROUTE, 404, NOT_FOUND_BODY)};
    routes[0].answer_limit = 1;
    routes[1].body_length = 10;
    http_fixture *server = serve(routes, 3);
    naula_refusal refusal = fresh_refusal();
    uint8_t *bytes = NULL;
    size_t length = 0;
    CHECK(gitea()->read_file(gitea()->context, HOST, NUMBER, "main", "README.md", NULL, &bytes, &length, &refusal) ==
          NAULA_NOT_FOUND);
    CHECK(bytes == NULL && length == 0);
    naula_memory_release(finish(server));
    routes[0].answer_limit = 0;
    server = serve(routes, 2);
    CHECK(gitea()->read_file(gitea()->context, HOST, NUMBER, "main", "README.md", NULL, &bytes, &length, &refusal) ==
          NAULA_OK);
    CHECK(length == 10 && memcmp(bytes, "# Pages\n\x01\x02", 10) == 0);
    naula_memory_release(bytes);
    naula_memory_release(finish(server));
    naula_memory_release(body);
}

static void read_file_refuses_bytes_from_a_repository_renamed_during_the_read(void) {
    char *body = load("repository.json");
    const char other[] = "{\"id\":16490,\"name\":\"other-name\",\"owner\":{\"login\":\"Codeberg\"}}";
    http_route routes[3] = {route(REPOSITORY_ROUTE, 200, body), route(RAW_ROUTE, 200, "bytes"),
                            route(REPOSITORY_ROUTE, 200, other)};
    routes[0].answer_limit = 1;
    http_fixture *server = serve(routes, 3);
    naula_refusal refusal = fresh_refusal();
    uint8_t *bytes = NULL;
    size_t length = 0;
    CHECK(gitea()->read_file(gitea()->context, HOST, NUMBER, "main", "README.md", NULL, &bytes, &length, &refusal) ==
          NAULA_REFUSED);
    check_text(refusal.code, "repository_changed", "renamed during the read");
    CHECK(bytes == NULL && length == 0);
    naula_memory_release(finish(server));
    naula_memory_release(body);
}

static void a_missing_file_ref_or_commit_in_a_live_repository_is_not_not_found(void) {
    char *body = load("repository.json");
    http_route routes[4] = {route(PREFIX "/raw/nope?ref=main", 404, NOT_FOUND_BODY),
                            route(PREFIX "/raw/README.md?ref=nobranch", 404, NOT_FOUND_BODY),
                            route(PREFIX "/git/commits/" COMMIT, 404, NOT_FOUND_BODY),
                            route(REPOSITORY_ROUTE, 200, body)};
    http_fixture *server = serve(routes, 4);
    naula_refusal refusal = fresh_refusal();
    uint8_t *bytes = NULL;
    size_t length = 0;
    size_t count = 0;
    CHECK(gitea()->read_file(gitea()->context, HOST, NUMBER, "main", "nope", NULL, &bytes, &length, &refusal) ==
          NAULA_REFUSED);
    check_text(refusal.code, "missing_file_or_ref", "missing file");
    CHECK(gitea()->read_file(gitea()->context, HOST, NUMBER, "nobranch", "README.md", NULL, &bytes, &length, &refusal) ==
          NAULA_REFUSED);
    check_text(refusal.code, "missing_file_or_ref", "unknown ref");
    CHECK(gitea()->parent_count(gitea()->context, HOST, NUMBER, COMMIT, NULL, &count, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "missing_commit", "unknown commit");
    naula_memory_release(finish(server));
    naula_memory_release(body);
}

static void contains_reads_two_comparisons_and_the_tip(void) {
    char *behind = load("compare_behind.json");
    char *ahead = load("compare_ahead.json");
    char *repository = load("repository.json");
    char tip_body[128];
    snprintf(tip_body, sizeof tip_body, "{\"sha\":\"%s\"}", COMMIT);
    http_route ahead_routes[3] = {route(REPOSITORY_ROUTE, 200, repository), route(COMPARE_FORWARD, 200, behind),
                                  route(COMPARE_BACKWARD, 200, ahead)};
    http_fixture *server = serve(ahead_routes, 3);
    naula_refusal refusal = fresh_refusal();
    int contained = 0;
    CHECK(contains("main", &contained, &refusal) == NAULA_OK && contained == 1);
    naula_memory_release(finish(server));
    http_route identical_routes[4] = {route(REPOSITORY_ROUTE, 200, repository), route(COMPARE_FORWARD, 200, behind),
                                      route(COMPARE_BACKWARD, 200, behind), route(PREFIX "/git/commits/main", 200, tip_body)};
    server = serve(identical_routes, 4);
    CHECK(contains("main", &contained, &refusal) == NAULA_OK && contained == 1);
    naula_memory_release(finish(server));
    http_route unrelated_routes[4] = {route(REPOSITORY_ROUTE, 200, repository), route(COMPARE_FORWARD, 200, behind),
                                      route(COMPARE_BACKWARD, 200, behind),
                                      route(PREFIX "/git/commits/main", 200, "{\"sha\":\"" TIP "\"}")};
    server = serve(unrelated_routes, 4);
    CHECK(contains("main", &contained, &refusal) == NAULA_OK && contained == 0);
    naula_memory_release(finish(server));
    naula_memory_release(repository);
    naula_memory_release(ahead);
    naula_memory_release(behind);
}

static void contains_answers_no_for_a_commit_the_branch_lacks(void) {
    char *diverged = load("compare_diverged.json");
    char *ahead = load("compare_ahead.json");
    char *repository = load("repository.json");
    http_route routes[3] = {route(REPOSITORY_ROUTE, 200, repository), route(COMPARE_FORWARD, 200, diverged),
                            route(COMPARE_BACKWARD, 200, ahead)};
    http_fixture *server = serve(routes, 3);
    naula_refusal refusal = fresh_refusal();
    int contained = 1;
    CHECK(contains("main", &contained, &refusal) == NAULA_OK && contained == 0);
    naula_memory_release(finish(server));
    naula_memory_release(repository);
    naula_memory_release(ahead);
    naula_memory_release(diverged);
}

static void contains_answers_no_for_a_commit_a_live_repository_does_not_know(void) {
    char *repository = load("repository.json");
    http_route routes[2] = {route(REPOSITORY_ROUTE, 200, repository), route(COMPARE_FORWARD, 404, NOT_FOUND_BODY)};
    http_fixture *server = serve(routes, 2);
    naula_refusal refusal = fresh_refusal();
    int contained = 1;
    CHECK(contains("main", &contained, &refusal) == NAULA_OK && contained == 0);
    naula_memory_release(finish(server));
    http_route gone[2] = {route(REPOSITORY_ROUTE, 404, NOT_FOUND_BODY), route(COMPARE_FORWARD, 404, NOT_FOUND_BODY)};
    server = serve(gone, 2);
    CHECK(contains("main", &contained, &refusal) == NAULA_NOT_FOUND && contained == 0);
    naula_memory_release(finish(server));
    naula_memory_release(repository);
}

static void contains_refuses_a_commit_that_is_not_hex(void) {
    naula_refusal refusal = fresh_refusal();
    int contained = 1;
    CHECK(gitea()->contains(gitea()->context, HOST, NUMBER, "main", "not-hex", NULL, &contained, &refusal) ==
          NAULA_REFUSED);
    CHECK(contained == 0);
}

static void parent_count_reads_the_commit(void) {
    char *repository = load("repository.json");
    char *commit = load("commit.json");
    const char root[] = "{\"sha\":\"" COMMIT "\",\"parents\":[]}";
    http_route routes[2] = {route(REPOSITORY_ROUTE, 200, repository), route(PREFIX "/git/commits/" COMMIT, 200, commit)};
    http_fixture *server = serve(routes, 2);
    naula_refusal refusal = fresh_refusal();
    size_t count = 9;
    CHECK(gitea()->parent_count(gitea()->context, HOST, NUMBER, COMMIT, NULL, &count, &refusal) == NAULA_OK);
    CHECK(count == 1);
    naula_memory_release(finish(server));
    routes[1] = route(PREFIX "/git/commits/" COMMIT, 200, root);
    server = serve(routes, 2);
    CHECK(gitea()->parent_count(gitea()->context, HOST, NUMBER, COMMIT, NULL, &count, &refusal) == NAULA_OK);
    CHECK(count == 0);
    naula_memory_release(finish(server));
    naula_memory_release(commit);
    naula_memory_release(repository);
}

static void list_repos_reads_pages_until_an_empty_one(void) {
    char *repository = load("repository.json");
    char first_page[4096];
    snprintf(first_page, sizeof first_page, "[%s]", repository);
    http_route routes[2] = {route("/orgs/Codeberg/repos?limit=50&page=1", 200, first_page),
                            route("/orgs/Codeberg/repos?limit=50&page=2", 200, "[]")};
    http_fixture *server = serve(routes, 2);
    naula_refusal refusal = fresh_refusal();
    naula_repository *repositories = NULL;
    size_t count = 0;
    CHECK(gitea()->list_repos(gitea()->context, HOST, "Codeberg", NULL, &repositories, &count, &refusal) == NAULA_OK);
    CHECK(count == 1 && repositories != NULL && repositories[0].number == NUMBER);
    naula_repository_list_release(repositories, count);
    naula_memory_release(finish(server));
    naula_memory_release(repository);
}

static void list_repos_falls_back_to_a_user_and_answers_not_found_for_neither(void) {
    http_route routes[2] = {route("/orgs/nobody/repos?limit=50&page=1", 404, NOT_FOUND_BODY),
                            route("/users/nobody/repos?limit=50&page=1", 200, "[]")};
    http_fixture *server = serve(routes, 2);
    naula_refusal refusal = fresh_refusal();
    naula_repository *repositories = NULL;
    size_t count = 7;
    CHECK(gitea()->list_repos(gitea()->context, HOST, "nobody", NULL, &repositories, &count, &refusal) == NAULA_OK);
    CHECK(count == 0 && repositories == NULL);
    naula_memory_release(finish(server));
    routes[1] = route("/users/nobody/repos?limit=50&page=1", 404, NOT_FOUND_BODY);
    server = serve(routes, 2);
    CHECK(gitea()->list_repos(gitea()->context, HOST, "nobody", NULL, &repositories, &count, &refusal) ==
          NAULA_NOT_FOUND);
    naula_memory_release(finish(server));
}

static void describe_names_the_supported_operations_and_release_index_is_refused(void) {
    const naula_provider *words[] = {naula_provider_gitea(), naula_provider_forgejo()};
    const char *const names[] = {"gitea", "forgejo"};
    for (size_t index = 0; index < 2; index++) {
        uint32_t supported = 0;
        int anonymous = 1;
        check_text(words[index]->word, names[index], "word");
        CHECK(words[index]->describe(words[index]->context, &supported, &anonymous) == NAULA_OK);
        CHECK((supported & NAULA_OP_CONTAINS) != 0 && (supported & NAULA_OP_PARENT_COUNT) != 0);
        CHECK((supported & NAULA_OP_RELEASE_INDEX) == 0 && anonymous == 0);
        naula_refusal refusal = fresh_refusal();
        uint8_t *bytes = NULL;
        size_t length = 0;
        CHECK(words[index]->release_index(words[index]->context, HOST, NUMBER, "v1", NULL, &bytes, &length, &refusal) ==
              NAULA_REFUSED);
        check_text(refusal.code, "unsupported_operation", "release_index");
    }
}

static void forgejo_answers_from_its_own_word_and_the_registry_holds_every_built_in(void) {
    char *body = load("repository.json");
    http_route routes[1] = {route(REPOSITORY_ROUTE, 401, "{}")};
    http_fixture *server = serve(routes, 1);
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    const naula_provider *forgejo = naula_provider_forgejo();
    CHECK(forgejo->lookup(forgejo->context, HOST, NUMBER, NULL, &repository, &refusal) == NAULA_REFUSED);
    CHECK(strstr(refusal.detail, "Forgejo") != NULL && strstr(refusal.detail, "Gitea") == NULL);
    naula_memory_release(finish(server));
    naula_providers *providers = NULL;
    CHECK(naula_providers_open(&providers) == NAULA_OK);
    const char *const words[] = {"github", "gitea", "forgejo", "web"};
    for (size_t index = 0; index < 4; index++) {
        CHECK(naula_providers_find(providers, words[index], &refusal) != NULL);
    }
    naula_providers_close(providers);
    naula_memory_release(body);
}

int main(void) {
    naula_fetch_allow_plain_http(1);
    lookup_reads_a_repository_with_its_etag();
    a_lookup_without_a_token_names_the_fix();
    a_lookup_that_names_no_host_is_refused_with_the_fix();
    lookup_compares_the_echoed_number();
    lookup_reads_the_fork_parent();
    each_failure_status_is_its_own_answer_and_only_404_is_not_found();
    a_timeout_is_timed_out_not_not_found();
    the_token_is_sent_only_when_given_and_never_repeated();
    every_call_sends_the_accept_header();
    find_reads_the_number_and_answers_zero_for_a_missing_or_renamed_name();
    read_file_reads_the_raw_bytes_and_reverifies_the_number();
    read_file_refuses_bytes_from_a_repository_renamed_during_the_read();
    a_missing_file_ref_or_commit_in_a_live_repository_is_not_not_found();
    contains_reads_two_comparisons_and_the_tip();
    contains_answers_no_for_a_commit_the_branch_lacks();
    contains_answers_no_for_a_commit_a_live_repository_does_not_know();
    contains_refuses_a_commit_that_is_not_hex();
    parent_count_reads_the_commit();
    list_repos_reads_pages_until_an_empty_one();
    list_repos_falls_back_to_a_user_and_answers_not_found_for_neither();
    describe_names_the_supported_operations_and_release_index_is_refused();
    forgejo_answers_from_its_own_word_and_the_registry_holds_every_built_in();
    return check_summary("provider_gitea_test");
}
