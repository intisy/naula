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
#include "provider_github_hooks.h"
#include "transport_hooks.h"
#include "naula/json.h"

#ifndef NAULA_GITHUB_FIXTURES
#error "NAULA_GITHUB_FIXTURES names test/fixtures/github"
#endif

#define NUMBER 1296269u
#define OTHER_NUMBER 1300192u
#define COMMIT "7fd1a60b01f91b314f59955a4e4d4e80d8edf11d"
#define ETAG "W/\"abc\""
#define TOKEN_TEXT "not-a-real-token"
#define NOT_FOUND_BODY "{\"message\":\"Not Found\"}"
#define REPOSITORY_ROUTE "/repositories/1296269"

static char *load(const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", NAULA_GITHUB_FIXTURES, name);
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
        naula_github_use_api_root(root);
    }
    return server;
}

static char *finish(http_fixture *server) {
    naula_github_use_api_root(NULL);
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

static const naula_provider *github(void) {
    return naula_provider_github();
}

static naula_status lookup(uint64_t number, const char *token, naula_repository *out, naula_refusal *refusal) {
    return github()->lookup(github()->context, NULL, number, token, out, refusal);
}

static void lookup_reads_a_repository_with_its_etag(void) {
    char *body = load("repository.json");
    http_route routes[1] = {route(REPOSITORY_ROUTE, 200, body)};
    routes[0].etag = ETAG;
    http_fixture *server = serve(routes, 1);
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    CHECK(lookup(NUMBER, NULL, &repository, &refusal) == NAULA_OK);
    CHECK(repository.number == NUMBER && repository.archived == 0 && repository.fork_parent == 0);
    check_text(repository.account, "octocat", "account");
    check_text(repository.name, "Hello-World", "name");
    check_text(repository.default_branch, "master", "default branch");
    check_text(repository.clone_url, "https://github.com/octocat/Hello-World.git", "clone url");
    check_text(repository.etag, ETAG, "etag");
    naula_repository_release(&repository);
    naula_memory_release(finish(server));
    naula_memory_release(body);
}

static void lookup_compares_the_echoed_number(void) {
    char *body = load("repository.json");
    http_route routes[1] = {route("/repositories/1300192", 200, body)};
    http_fixture *server = serve(routes, 1);
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    CHECK(lookup(OTHER_NUMBER, NULL, &repository, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "number_mismatch", "refusal code");
    CHECK(repository.name == NULL && repository.account == NULL);
    naula_memory_release(finish(server));
    naula_memory_release(body);
}

static void lookup_reads_the_fork_parent(void) {
    char *body = load("fork.json");
    http_route routes[1] = {route("/repositories/1472306731", 200, body)};
    http_fixture *server = serve(routes, 1);
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    CHECK(lookup(1472306731u, NULL, &repository, &refusal) == NAULA_OK);
    CHECK(repository.fork_parent == NUMBER);
    check_text(repository.account, "b1ubu8", "fork account");
    naula_repository_release(&repository);
    naula_memory_release(finish(server));
    naula_memory_release(body);
}

static void lookup_refuses_a_number_beyond_what_a_double_holds(void) {
    const char big[] = "{\"id\":9007199254740993,\"name\":\"big\",\"owner\":{\"login\":\"o\"}}";
    http_route routes[1] = {route("/repositories/9007199254740993", 200, big)};
    http_fixture *server = serve(routes, 1);
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    CHECK(lookup(9007199254740993ull, NULL, &repository, &refusal) == NAULA_MALFORMED);
    naula_memory_release(finish(server));
}

static void lookup_refuses_an_answer_without_names_or_not_an_object(void) {
    const char no_names[] = "{\"id\":1296269}";
    http_route routes[1] = {route(REPOSITORY_ROUTE, 200, no_names)};
    http_fixture *server = serve(routes, 1);
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    CHECK(lookup(NUMBER, NULL, &repository, &refusal) == NAULA_MALFORMED);
    check_text(refusal.code, "malformed_answer", "no names");
    naula_memory_release(finish(server));
    const char not_an_object[] = "[]";
    routes[0] = route(REPOSITORY_ROUTE, 200, not_an_object);
    server = serve(routes, 1);
    CHECK(lookup(NUMBER, NULL, &repository, &refusal) == NAULA_MALFORMED);
    naula_memory_release(finish(server));
}

static void lookup_refuses_an_answer_above_the_cap(void) {
    size_t length = 300u * 1024u;
    char *oversized = naula_memory_allocate(length + 1);
    memset(oversized, ' ', length);
    oversized[length] = '\0';
    http_route routes[1] = {route(REPOSITORY_ROUTE, 200, oversized)};
    http_fixture *server = serve(routes, 1);
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    CHECK(lookup(NUMBER, NULL, &repository, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "answer_too_large", "oversized");
    naula_memory_release(finish(server));
    naula_memory_release(oversized);
}

static void a_404_by_number_is_not_found(void) {
    http_route routes[1] = {route(REPOSITORY_ROUTE, 404, NOT_FOUND_BODY)};
    http_fixture *server = serve(routes, 1);
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    CHECK(lookup(NUMBER, NULL, &repository, &refusal) == NAULA_NOT_FOUND);
    check_text(refusal.code, "not_found", "refusal code");
    naula_memory_release(finish(server));
}

static void a_rate_limit_and_a_missing_token_are_refused_with_their_own_codes(void) {
    const int statuses[] = {403, 429, 401};
    const char *const codes[] = {"forbidden_or_rate_limited", "rate_limited", "token_needed"};
    for (size_t index = 0; index < 3; index++) {
        http_route routes[1] = {route(REPOSITORY_ROUTE, statuses[index], "{}")};
        http_fixture *server = serve(routes, 1);
        naula_repository repository = fresh_repository();
        naula_refusal refusal = fresh_refusal();
        CHECK(lookup(NUMBER, NULL, &repository, &refusal) == NAULA_REFUSED);
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
        char first_head[512];
        snprintf(first_head, sizeof first_head, "%.*s", (int)(second - log), log);
        CHECK(strstr(first_head, TOKEN_TEXT) == NULL);
    }
    naula_memory_release(log);
    refusal = fresh_refusal();
    CHECK(lookup(NUMBER, "has a space", &repository, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "invalid_token", "unusable token");
}

static void a_lookup_struct_smaller_than_its_first_shape_is_refused(void) {
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    repository.size = sizeof(uint32_t);
    CHECK(lookup(NUMBER, NULL, &repository, &refusal) == NAULA_REFUSED);
}

static void find_reads_the_number_and_answers_zero_for_a_missing_name(void) {
    char *body = load("repository.json");
    http_route routes[2] = {route("/repos/octocat/Hello-World", 200, body),
                            route("/repos/octocat/gone", 404, NOT_FOUND_BODY)};
    http_fixture *server = serve(routes, 2);
    naula_refusal refusal = fresh_refusal();
    uint64_t number = 5;
    CHECK(github()->find(NULL, NULL, "octocat", "Hello-World", NULL, &number, &refusal) == NAULA_OK);
    CHECK(number == NUMBER);
    CHECK(github()->find(NULL, NULL, "octocat", "gone", NULL, &number, &refusal) == NAULA_OK);
    CHECK(number == 0);
    CHECK(github()->find(NULL, NULL, "octocat", "../x", NULL, &number, &refusal) == NAULA_REFUSED);
    naula_memory_release(finish(server));
    naula_memory_release(body);
}

static void find_answers_zero_for_a_repository_renamed_away_from_the_name(void) {
    char *body = load("repository.json");
    char location[128];
    http_route routes[3] = {route("/repos/octocat/old-name", 301, ""), route("/repos/octocat/Hello-World", 200, body),
                            route("/repos/OCTOCAT/hello-world", 200, body)};
    routes[0].extra_headers = location;
    http_fixture *server = serve(routes, 3);
    snprintf(location, sizeof location, "Location: http://127.0.0.1:%u/repos/octocat/Hello-World\r\n",
             (unsigned)http_fixture_port(server));
    naula_refusal refusal = fresh_refusal();
    uint64_t number = 5;
    CHECK(github()->find(NULL, NULL, "octocat", "old-name", NULL, &number, &refusal) == NAULA_OK);
    CHECK(number == 0);
    CHECK(github()->find(NULL, NULL, "OCTOCAT", "hello-world", NULL, &number, &refusal) == NAULA_OK);
    CHECK(number == NUMBER);
    naula_memory_release(finish(server));
    naula_memory_release(body);
}

static void every_call_sends_the_api_version_and_accept_headers(void) {
    char *body = load("repository.json");
    http_route routes[1] = {route(REPOSITORY_ROUTE, 200, body)};
    http_fixture *server = serve(routes, 1);
    naula_repository repository = fresh_repository();
    naula_refusal refusal = fresh_refusal();
    CHECK(lookup(NUMBER, NULL, &repository, &refusal) == NAULA_OK);
    naula_repository_release(&repository);
    char *log = finish(server);
    CHECK(log != NULL && strstr(log, "X-GitHub-Api-Version: 2022-11-28") != NULL);
    CHECK(log != NULL && strstr(log, "Accept: application/vnd.github+json") != NULL);
    naula_memory_release(log);
    naula_memory_release(body);
}

static void a_missing_file_ref_or_commit_in_a_live_repository_is_not_not_found(void) {
    char *repository_body = load("repository.json");
    http_route routes[4] = {route("/repositories/1296269/contents/nope?ref=master", 404, NOT_FOUND_BODY),
                            route("/repositories/1296269/contents/README?ref=nobranch", 404, NOT_FOUND_BODY),
                            route("/repositories/1296269/commits/" COMMIT, 404, NOT_FOUND_BODY),
                            route(REPOSITORY_ROUTE, 200, repository_body)};
    http_fixture *server = serve(routes, 4);
    naula_refusal refusal = fresh_refusal();
    uint8_t *bytes = NULL;
    size_t length = 0;
    size_t count = 0;
    CHECK(github()->read_file(NULL, NULL, NUMBER, "master", "nope", NULL, &bytes, &length, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "missing_file_or_ref", "missing file");
    CHECK(github()->read_file(NULL, NULL, NUMBER, "nobranch", "README", NULL, &bytes, &length, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "missing_file_or_ref", "unknown ref");
    CHECK(github()->parent_count(NULL, NULL, NUMBER, COMMIT, NULL, &count, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "missing_commit", "unknown commit");
    naula_memory_release(finish(server));
    routes[3] = route(REPOSITORY_ROUTE, 404, NOT_FOUND_BODY);
    server = serve(routes, 4);
    CHECK(github()->read_file(NULL, NULL, NUMBER, "master", "nope", NULL, &bytes, &length, &refusal) == NAULA_NOT_FOUND);
    CHECK(github()->parent_count(NULL, NULL, NUMBER, COMMIT, NULL, &count, &refusal) == NAULA_NOT_FOUND);
    naula_memory_release(finish(server));
    naula_memory_release(repository_body);
}

static void a_failure_with_no_refusal_to_fill_does_not_crash(void) {
    http_route routes[2] = {route("/orgs/spisor/repos?per_page=100&page=1", 500, "{}"),
                            route("/repos/octocat/x", 500, "{}")};
    http_fixture *server = serve(routes, 2);
    naula_repository *repositories = NULL;
    size_t count = 0;
    uint64_t number = 0;
    CHECK(github()->list_repos(NULL, NULL, "spisor", NULL, &repositories, &count, NULL) == NAULA_REFUSED);
    CHECK(github()->find(NULL, NULL, "octocat", "x", NULL, &number, NULL) == NAULA_REFUSED);
    naula_memory_release(finish(server));
}

static void read_file_decodes_the_contents(void) {
    char *contents = load("contents.json");
    char *repository_body = load("repository.json");
    http_route routes[2] = {route("/repositories/1296269/contents/README?ref=master", 200, contents),
                            route(REPOSITORY_ROUTE, 200, repository_body)};
    http_fixture *server = serve(routes, 2);
    naula_refusal refusal = fresh_refusal();
    uint8_t *bytes = NULL;
    size_t length = 0;
    CHECK(github()->read_file(NULL, NULL, NUMBER, "master", "README", NULL, &bytes, &length, &refusal) == NAULA_OK);
    CHECK(length == 13 && bytes != NULL && memcmp(bytes, "Hello World!\n", 13) == 0);
    naula_memory_release(bytes);
    naula_memory_release(finish(server));
    naula_memory_release(contents);
    naula_memory_release(repository_body);
}

static void read_file_reverifies_the_number(void) {
    char *contents = load("contents.json");
    http_route routes[2] = {route("/repositories/1296269/contents/README?ref=master", 200, contents),
                            route(REPOSITORY_ROUTE, 200, "{\"id\":42,\"name\":\"n\",\"owner\":{\"login\":\"o\"}}")};
    http_fixture *server = serve(routes, 2);
    naula_refusal refusal = fresh_refusal();
    uint8_t *bytes = NULL;
    size_t length = 0;
    CHECK(github()->read_file(NULL, NULL, NUMBER, "master", "README", NULL, &bytes, &length, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "number_mismatch", "reverified number");
    CHECK(bytes == NULL && length == 0);
    naula_memory_release(finish(server));
    naula_memory_release(contents);
}

static void read_file_refuses_a_folder_and_a_file_too_large_to_inline(void) {
    char *repository_body = load("repository.json");
    const char folder[] = "[{\"name\":\"a\",\"type\":\"file\"}]";
    const char large[] = "{\"type\":\"file\",\"encoding\":\"none\",\"content\":\"\"}";
    http_route routes[3] = {route("/repositories/1296269/contents/docs?ref=master", 200, folder),
                            route("/repositories/1296269/contents/big?ref=master", 200, large),
                            route(REPOSITORY_ROUTE, 200, repository_body)};
    http_fixture *server = serve(routes, 3);
    naula_refusal refusal = fresh_refusal();
    uint8_t *bytes = NULL;
    size_t length = 0;
    CHECK(github()->read_file(NULL, NULL, NUMBER, "master", "docs", NULL, &bytes, &length, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "not_a_file", "folder");
    CHECK(github()->read_file(NULL, NULL, NUMBER, "master", "big", NULL, &bytes, &length, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "file_too_large", "large file");
    naula_memory_release(finish(server));
    naula_memory_release(repository_body);
}

static void read_file_encodes_the_path_and_the_ref(void) {
    char *contents = load("contents.json");
    char *repository_body = load("repository.json");
    http_route routes[2] = {route("/repositories/1296269/contents/a%20b/c.json?ref=feature%2Fx", 200, contents),
                            route(REPOSITORY_ROUTE, 200, repository_body)};
    http_fixture *server = serve(routes, 2);
    naula_refusal refusal = fresh_refusal();
    uint8_t *bytes = NULL;
    size_t length = 0;
    CHECK(github()->read_file(NULL, NULL, NUMBER, "feature/x", "a b/c.json", NULL, &bytes, &length, &refusal) == NAULA_OK);
    naula_memory_release(bytes);
    naula_memory_release(finish(server));
    naula_memory_release(contents);
    naula_memory_release(repository_body);
}

static int contained_for(const char *fixture_name, int status, const char *route_path, naula_status *answered) {
    char *body = load(fixture_name);
    http_route routes[1] = {route(route_path, status, body != NULL ? body : "")};
    http_fixture *server = serve(routes, 1);
    naula_refusal refusal = fresh_refusal();
    int contained = -1;
    *answered = github()->contains(NULL, NULL, NUMBER, "master", COMMIT, NULL, &contained, &refusal);
    naula_memory_release(finish(server));
    naula_memory_release(body);
    return contained;
}

static void contains_reads_compare_ahead_or_identical(void) {
    naula_status status;
    const char *path = "/repositories/1296269/compare/" COMMIT "...master?per_page=1";
    CHECK(contained_for("compare_ahead.json", 200, path, &status) == 1 && status == NAULA_OK);
    CHECK(contained_for("compare_identical.json", 200, path, &status) == 1 && status == NAULA_OK);
    CHECK(contained_for("compare_behind.json", 200, path, &status) == 0 && status == NAULA_OK);
}

static void contains_refuses_what_a_live_repository_does_not_know_and_is_not_found_once_it_is_gone(void) {
    char *repository_body = load("repository.json");
    http_route routes[2] = {route("/repositories/1296269/compare/" COMMIT "...master?per_page=1", 404, NOT_FOUND_BODY),
                            route(REPOSITORY_ROUTE, 200, repository_body)};
    http_fixture *server = serve(routes, 2);
    naula_refusal refusal = fresh_refusal();
    int contained = -1;
    CHECK(github()->contains(NULL, NULL, NUMBER, "master", COMMIT, NULL, &contained, &refusal) == NAULA_REFUSED);
    CHECK(contained == 0);
    check_text(refusal.code, "missing_commit_or_ref", "unknown commit in a live repository");
    CHECK(refusal.fix != NULL);
    naula_memory_release(finish(server));
    routes[1] = route(REPOSITORY_ROUTE, 404, NOT_FOUND_BODY);
    server = serve(routes, 2);
    CHECK(github()->contains(NULL, NULL, NUMBER, "master", COMMIT, NULL, &contained, &refusal) == NAULA_NOT_FOUND);
    CHECK(contained == 0);
    naula_memory_release(finish(server));
    naula_memory_release(repository_body);
}

static void contains_refuses_a_commit_that_is_not_hex_or_a_ref_that_reads_as_an_option(void) {
    naula_refusal refusal = fresh_refusal();
    int contained = -1;
    CHECK(github()->contains(NULL, NULL, NUMBER, "master", "../x", NULL, &contained, &refusal) == NAULA_REFUSED);
    contained = -1;
    CHECK(github()->contains(NULL, NULL, NUMBER, "--all", COMMIT, NULL, &contained, &refusal) == NAULA_REFUSED);
    CHECK(contained == 0);
    check_text(refusal.code, "invalid_request", "ref read as an option");
}

static void parent_count_reads_the_commit(void) {
    char *body = load("commit.json");
    http_route routes[1] = {route("/repositories/1296269/commits/" COMMIT, 200, body)};
    http_fixture *server = serve(routes, 1);
    naula_refusal refusal = fresh_refusal();
    size_t count = 0;
    CHECK(github()->parent_count(NULL, NULL, NUMBER, COMMIT, NULL, &count, &refusal) == NAULA_OK);
    CHECK(count == 2);
    naula_memory_release(finish(server));
    naula_memory_release(body);
}

static void a_root_commit_has_no_parents(void) {
    const char root_commit[] = "{\"sha\":\"" COMMIT "\",\"parents\":[]}";
    http_route routes[1] = {route("/repositories/1296269/commits/" COMMIT, 200, root_commit)};
    http_fixture *server = serve(routes, 1);
    naula_refusal refusal = fresh_refusal();
    size_t count = 9;
    CHECK(github()->parent_count(NULL, NULL, NUMBER, COMMIT, NULL, &count, &refusal) == NAULA_OK);
    CHECK(count == 0);
    naula_memory_release(finish(server));
}

static char *page_of(unsigned first, unsigned count) {
    char *text = naula_memory_allocate(count * 128u + 8u);
    size_t length = 0;
    text[length++] = '[';
    for (unsigned index = 0; index < count; index++) {
        length += (size_t)snprintf(text + length, 128, "%s{\"id\":%u,\"name\":\"r%u\",\"owner\":{\"login\":\"spisor\"},\"archived\":%s}",
                                   index == 0 ? "" : ",", first + index, first + index,
                                   index % 2 == 0 ? "true" : "false");
    }
    text[length++] = ']';
    text[length] = '\0';
    return text;
}

static void list_repos_reads_every_page_of_an_organization(void) {
    char *first = page_of(1, 100);
    char *second = page_of(101, 1);
    http_route routes[2] = {route("/orgs/spisor/repos?per_page=100&page=1", 200, first),
                            route("/orgs/spisor/repos?per_page=100&page=2", 200, second)};
    http_fixture *server = serve(routes, 2);
    naula_refusal refusal = fresh_refusal();
    naula_repository *repositories = NULL;
    size_t count = 0;
    CHECK(github()->list_repos(NULL, NULL, "spisor", NULL, &repositories, &count, &refusal) == NAULA_OK);
    CHECK(count == 101);
    CHECK(count == 101 && repositories[0].number == 1 && repositories[0].archived == 1 && repositories[1].archived == 0);
    CHECK(count == 101 && repositories[100].number == 101);
    naula_repository_list_release(repositories, count);
    naula_memory_release(finish(server));
    naula_memory_release(first);
    naula_memory_release(second);
}

static void list_repos_falls_back_to_a_user_when_no_organization_has_the_name(void) {
    char *page = page_of(7, 2);
    http_route routes[2] = {route("/orgs/octocat/repos?per_page=100&page=1", 404, NOT_FOUND_BODY),
                            route("/users/octocat/repos?per_page=100&page=1", 200, page)};
    http_fixture *server = serve(routes, 2);
    naula_refusal refusal = fresh_refusal();
    naula_repository *repositories = NULL;
    size_t count = 0;
    CHECK(github()->list_repos(NULL, NULL, "octocat", NULL, &repositories, &count, &refusal) == NAULA_OK);
    CHECK(count == 2 && repositories[0].number == 7);
    naula_repository_list_release(repositories, count);
    naula_memory_release(finish(server));
    naula_memory_release(page);
}

static void list_repos_of_neither_an_organization_nor_a_user_is_not_found(void) {
    http_route routes[2] = {route("/orgs/nobody/repos?per_page=100&page=1", 404, NOT_FOUND_BODY),
                            route("/users/nobody/repos?per_page=100&page=1", 404, NOT_FOUND_BODY)};
    http_fixture *server = serve(routes, 2);
    naula_refusal refusal = fresh_refusal();
    naula_repository *repositories = NULL;
    size_t count = 0;
    CHECK(github()->list_repos(NULL, NULL, "nobody", NULL, &repositories, &count, &refusal) == NAULA_NOT_FOUND);
    CHECK(repositories == NULL && count == 0);
    naula_memory_release(finish(server));
}

static void describe_names_the_supported_operations(void) {
    uint32_t supported = 0;
    int anonymous = 0;
    CHECK(github()->describe(NULL, &supported, &anonymous) == NAULA_OK);
    CHECK((supported & NAULA_OP_LOOKUP) && (supported & NAULA_OP_FIND) && (supported & NAULA_OP_READ_FILE));
    CHECK((supported & NAULA_OP_CONTAINS) && (supported & NAULA_OP_PARENT_COUNT) && (supported & NAULA_OP_LIST_REPOS));
    CHECK((supported & NAULA_OP_RELEASE_INDEX) != 0 && anonymous == 1);
}

#define RELEASE_LATEST_ROUTE "/repositories/1296269/releases/latest"
#define INDEX_ASSET "gh_2.106.0_checksums.txt"
#define INDEX_ASSET_ROUTE "/repositories/1296269/releases/assets/793444804"
#define INDEX_URL "https://github.com/cli/cli/releases/download/v2.106.0/gh_2.106.0_checksums.txt"
#define INDEX_BYTES "{\"items\":{}}\n"

static naula_release_index fresh_index(void) {
    naula_release_index index;
    memset(&index, 0, sizeof index);
    index.size = sizeof index;
    return index;
}

static naula_status release_index(const char *tag, const char *asset, const char *token, naula_release_index *out,
                                  naula_refusal *refusal) {
    return github()->release_index(github()->context, NULL, NUMBER, tag, asset, token, out, refusal);
}

static void release_index_reads_the_asset_of_the_latest_release_by_number(void) {
    char *release = load("release.json");
    http_route routes[2] = {route(RELEASE_LATEST_ROUTE, 200, release), route(INDEX_ASSET_ROUTE, 200, INDEX_BYTES)};
    http_fixture *server = serve(routes, 2);
    naula_release_index index = fresh_index();
    naula_refusal refusal = fresh_refusal();
    CHECK(release_index(NULL, INDEX_ASSET, TOKEN_TEXT, &index, &refusal) == NAULA_OK);
    CHECK(index.length == strlen(INDEX_BYTES) && index.bytes != NULL &&
          memcmp(index.bytes, INDEX_BYTES, index.length) == 0);
    check_text(index.url, INDEX_URL, "the address the release offers the index at");
    naula_release_index_release(&index);
    CHECK(index.bytes == NULL && index.url == NULL && index.size == sizeof index);
    char *log = finish(server);
    CHECK(log != NULL);
    const char *asset_request = log == NULL ? NULL : strstr(log, "GET " INDEX_ASSET_ROUTE);
    CHECK(asset_request != NULL && strstr(asset_request, "Accept: application/octet-stream") != NULL);
    CHECK(asset_request != NULL && strstr(asset_request, "token " TOKEN_TEXT) != NULL);
    CHECK(log != NULL && strstr(log, "/repos/") == NULL);
    naula_memory_release(log);
    naula_memory_release(release);
}

static void release_index_reads_a_tagged_release_with_the_tag_encoded(void) {
    char *release = load("release.json");
    http_route routes[2] = {route("/repositories/1296269/releases/tags/v2%2F1", 200, release),
                            route(INDEX_ASSET_ROUTE, 200, INDEX_BYTES)};
    http_fixture *server = serve(routes, 2);
    naula_release_index index = fresh_index();
    naula_refusal refusal = fresh_refusal();
    CHECK(release_index("v2/1", INDEX_ASSET, NULL, &index, &refusal) == NAULA_OK);
    naula_release_index_release(&index);
    naula_memory_release(finish(server));
    naula_memory_release(release);
}

static void release_index_follows_the_asset_redirect_without_the_token(void) {
    char *release = load("release.json");
    char location[160];
    http_route routes[3] = {route(RELEASE_LATEST_ROUTE, 200, release), route(INDEX_ASSET_ROUTE, 302, ""),
                            route("/signed-asset", 200, INDEX_BYTES)};
    routes[1].extra_headers = location;
    http_fixture *server = serve(routes, 3);
    snprintf(location, sizeof location, "Location: http://localhost:%u/signed-asset\r\n",
             (unsigned)http_fixture_port(server));
    naula_release_index index = fresh_index();
    naula_refusal refusal = fresh_refusal();
    CHECK(release_index(NULL, INDEX_ASSET, TOKEN_TEXT, &index, &refusal) == NAULA_OK);
    CHECK(index.length == strlen(INDEX_BYTES));
    naula_release_index_release(&index);
    char *log = finish(server);
    const char *signed_request = log == NULL ? NULL : strstr(log, "GET /signed-asset");
    CHECK(signed_request != NULL && strstr(signed_request, TOKEN_TEXT) == NULL);
    naula_memory_release(log);
    naula_memory_release(release);
}

static void release_index_refuses_a_missing_release_or_asset_of_a_live_repository(void) {
    char *release = load("release.json");
    char *repository_body = load("repository.json");
    http_route routes[3] = {route(RELEASE_LATEST_ROUTE, 404, NOT_FOUND_BODY), route(REPOSITORY_ROUTE, 200, repository_body),
                            route(INDEX_ASSET_ROUTE, 404, NOT_FOUND_BODY)};
    http_fixture *server = serve(routes, 3);
    naula_release_index index = fresh_index();
    naula_refusal refusal = fresh_refusal();
    CHECK(release_index(NULL, INDEX_ASSET, NULL, &index, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "missing_release", "no release on a live repository");
    naula_memory_release(finish(server));

    routes[1] = route(REPOSITORY_ROUTE, 404, NOT_FOUND_BODY);
    server = serve(routes, 3);
    CHECK(release_index(NULL, INDEX_ASSET, NULL, &index, &refusal) == NAULA_NOT_FOUND);
    naula_memory_release(finish(server));

    routes[0] = route(RELEASE_LATEST_ROUTE, 200, release);
    routes[1] = route(REPOSITORY_ROUTE, 200, repository_body);
    server = serve(routes, 3);
    refusal = fresh_refusal();
    CHECK(release_index(NULL, INDEX_ASSET, NULL, &index, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "missing_index", "an asset gone from a live repository");
    refusal = fresh_refusal();
    CHECK(release_index(NULL, "weivel-index.json", NULL, &index, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "no_index_asset", "a release without the asset");
    CHECK(refusal.fix != NULL && strstr(refusal.detail, "weivel-index.json") != NULL);
    CHECK(index.bytes == NULL && index.url == NULL);
    naula_memory_release(finish(server));
    naula_memory_release(release);
    naula_memory_release(repository_body);
}

static void release_index_empties_a_garbage_result_before_refusing_it(void) {
    naula_release_index index;
    memset(&index, 0xAB, sizeof index);
    index.size = sizeof index;
    naula_refusal refusal = fresh_refusal();
    CHECK(github()->release_index(NULL, NULL, 0, NULL, INDEX_ASSET, NULL, &index, &refusal) == NAULA_REFUSED);
    CHECK(index.bytes == NULL && index.url == NULL && index.length == 0);
    naula_release_index_release(&index);
}

#define TWICE_NAMED_RELEASE                                                                                           \
    "{\"assets\":[{\"id\":1,\"name\":\"" INDEX_ASSET "\"},"                                                           \
    "{\"id\":793444804,\"name\":\"" INDEX_ASSET "\",\"browser_download_url\":\"" INDEX_URL "\"}]}"

static void release_index_takes_the_asset_of_that_name_that_has_an_address(void) {
    http_route routes[2] = {route(RELEASE_LATEST_ROUTE, 200, TWICE_NAMED_RELEASE),
                            route(INDEX_ASSET_ROUTE, 200, INDEX_BYTES)};
    http_fixture *server = serve(routes, 2);
    naula_release_index index = fresh_index();
    naula_refusal refusal = fresh_refusal();
    CHECK(release_index(NULL, INDEX_ASSET, NULL, &index, &refusal) == NAULA_OK);
    check_text(index.url, INDEX_URL, "the second asset of the name");
    naula_release_index_release(&index);
    naula_memory_release(finish(server));
}

#define LAST_ASSETS_RELEASE                                                                                           \
    "{\"assets\":[],\"assets\":[{\"id\":793444804,\"name\":\"" INDEX_ASSET "\",\"browser_download_url\":\"" INDEX_URL \
    "\"}]}"

static void release_index_reads_the_last_value_of_a_duplicated_member(void) {
    http_route routes[2] = {route(RELEASE_LATEST_ROUTE, 200, LAST_ASSETS_RELEASE),
                            route(INDEX_ASSET_ROUTE, 200, INDEX_BYTES)};
    http_fixture *server = serve(routes, 2);
    naula_release_index index = fresh_index();
    naula_refusal refusal = fresh_refusal();
    CHECK(release_index(NULL, INDEX_ASSET, NULL, &index, &refusal) == NAULA_OK);
    naula_release_index_release(&index);
    naula_memory_release(finish(server));
}

#define INDEX_CAP (4u * 1024u * 1024u)

static void release_index_refuses_an_index_past_four_mebibytes(void) {
    char *release = load("release.json");
    char *large = malloc(INDEX_CAP + 2);
    CHECK(large != NULL);
    if (large == NULL) {
        return;
    }
    memset(large, 'x', INDEX_CAP + 1);
    large[INDEX_CAP + 1] = '\0';
    http_route routes[2] = {route(RELEASE_LATEST_ROUTE, 200, release), route(INDEX_ASSET_ROUTE, 200, large)};
    http_fixture *server = serve(routes, 2);
    naula_release_index index = fresh_index();
    naula_refusal refusal = fresh_refusal();
    CHECK(release_index(NULL, INDEX_ASSET, NULL, &index, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "answer_too_large", "an index past the cap");
    CHECK(index.bytes == NULL && index.url == NULL);
    naula_memory_release(finish(server));
    large[INDEX_CAP] = '\0';
    routes[1] = route(INDEX_ASSET_ROUTE, 200, large);
    server = serve(routes, 2);
    CHECK(release_index(NULL, INDEX_ASSET, NULL, &index, &refusal) == NAULA_OK);
    CHECK(index.length == INDEX_CAP);
    naula_release_index_release(&index);
    naula_memory_release(finish(server));
    free(large);
    naula_memory_release(release);
}

static void release_index_refuses_what_it_cannot_address_before_any_request(void) {
    naula_release_index index = fresh_index();
    naula_refusal refusal = fresh_refusal();
    const char *const tags[] = {"", "-v1"};
    for (size_t at = 0; at < 2; at++) {
        CHECK(release_index(tags[at], INDEX_ASSET, NULL, &index, &refusal) == NAULA_REFUSED);
        check_text(refusal.code, "invalid_request", "an unusable tag");
    }
    CHECK(release_index(NULL, "", NULL, &index, &refusal) == NAULA_REFUSED);
    CHECK(release_index(NULL, NULL, NULL, &index, &refusal) == NAULA_REFUSED);
    CHECK(github()->release_index(NULL, NULL, 0, NULL, INDEX_ASSET, NULL, &index, &refusal) == NAULA_REFUSED);
    index.size = sizeof(uint32_t);
    CHECK(release_index(NULL, INDEX_ASSET, NULL, &index, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "invalid_request", "a result struct smaller than its first shape");
}

static void unknown_word_names_the_fix(void) {
    naula_providers *providers = NULL;
    naula_refusal refusal = fresh_refusal();
    CHECK(naula_providers_open(&providers) == NAULA_OK);
    const naula_provider *found = naula_providers_find(providers, "github", &refusal);
    CHECK(found != NULL && found->lookup == github()->lookup);
    CHECK(naula_providers_find(providers, "nonesuch", &refusal) == NULL);
    check_text(refusal.code, "unknown_provider", "unknown word");
    check_text(refusal.fix, "install a plugin that provides a resolver", "unknown word fix");
    naula_providers_close(providers);
}

static void register_refuses_a_second_provider_for_a_word(void) {
    naula_providers *providers = NULL;
    naula_refusal refusal = fresh_refusal();
    CHECK(naula_providers_open(&providers) == NAULA_OK);
    CHECK(naula_providers_register(providers, github(), &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "provider_word_taken", "second provider");
    naula_provider extra = *github();
    extra.word = "extra";
    CHECK(naula_providers_register(providers, &extra, &refusal) == NAULA_OK);
    CHECK(naula_providers_find(providers, "extra", &refusal) != NULL);
    naula_provider hollow = extra;
    hollow.word = "hollow";
    hollow.contains = NULL;
    CHECK(naula_providers_register(providers, &hollow, &refusal) == NAULA_REFUSED);
    naula_provider small = extra;
    small.word = "small";
    small.size = sizeof(uint32_t);
    CHECK(naula_providers_register(providers, &small, &refusal) == NAULA_REFUSED);
    CHECK(naula_providers_find(providers, "small", &refusal) == NULL);
    naula_providers_close(providers);
}

int main(void) {
    naula_json_use_allocator();
    naula_fetch_allow_plain_http(1);
    lookup_reads_a_repository_with_its_etag();
    lookup_compares_the_echoed_number();
    lookup_reads_the_fork_parent();
    lookup_refuses_a_number_beyond_what_a_double_holds();
    lookup_refuses_an_answer_without_names_or_not_an_object();
    lookup_refuses_an_answer_above_the_cap();
    a_404_by_number_is_not_found();
    a_rate_limit_and_a_missing_token_are_refused_with_their_own_codes();
    the_token_is_sent_only_when_given_and_never_repeated();
    a_lookup_struct_smaller_than_its_first_shape_is_refused();
    find_reads_the_number_and_answers_zero_for_a_missing_name();
    find_answers_zero_for_a_repository_renamed_away_from_the_name();
    every_call_sends_the_api_version_and_accept_headers();
    a_missing_file_ref_or_commit_in_a_live_repository_is_not_not_found();
    a_failure_with_no_refusal_to_fill_does_not_crash();
    read_file_decodes_the_contents();
    read_file_reverifies_the_number();
    read_file_refuses_a_folder_and_a_file_too_large_to_inline();
    read_file_encodes_the_path_and_the_ref();
    contains_reads_compare_ahead_or_identical();
    contains_refuses_what_a_live_repository_does_not_know_and_is_not_found_once_it_is_gone();
    contains_refuses_a_commit_that_is_not_hex_or_a_ref_that_reads_as_an_option();
    parent_count_reads_the_commit();
    a_root_commit_has_no_parents();
    list_repos_reads_every_page_of_an_organization();
    list_repos_falls_back_to_a_user_when_no_organization_has_the_name();
    list_repos_of_neither_an_organization_nor_a_user_is_not_found();
    describe_names_the_supported_operations();
    release_index_reads_the_asset_of_the_latest_release_by_number();
    release_index_reads_a_tagged_release_with_the_tag_encoded();
    release_index_follows_the_asset_redirect_without_the_token();
    release_index_refuses_a_missing_release_or_asset_of_a_live_repository();
    release_index_refuses_what_it_cannot_address_before_any_request();
    release_index_empties_a_garbage_result_before_refusing_it();
    release_index_takes_the_asset_of_that_name_that_has_an_address();
    release_index_reads_the_last_value_of_a_duplicated_member();
    release_index_refuses_an_index_past_four_mebibytes();
    unknown_word_names_the_fix();
    register_refuses_a_second_provider_for_a_word();
    a_timeout_is_timed_out_not_not_found();
    return check_summary("provider_github_test");
}
