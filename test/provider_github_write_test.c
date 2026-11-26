#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "forge_common.h"
#include "http_fixture.h"
#include "naula/json.h"
#include "naula/memory.h"
#include "naula/metadata.h"
#include "naula/provider.h"
#include "provider_github_hooks.h"
#include "transport_hooks.h"

#define NUMBER 1296269u
#define TOKEN_TEXT "not-a-real-token"
#define REPOSITORY_ROUTE "/repositories/1296269"
#define NOT_FOUND_BODY "{\"message\":\"Not Found\"}"
#define LIVE "{\"id\":1296269,\"name\":\"Hello-World\",\"owner\":{\"login\":\"octocat\"},\"archived\":false}"
#define RENAMED "{\"id\":1296269,\"name\":\"renamed\",\"owner\":{\"login\":\"octocat\"},\"archived\":false}"
#define ARCHIVED "{\"id\":1296269,\"name\":\"Hello-World\",\"owner\":{\"login\":\"octocat\"},\"archived\":true}"

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

static naula_metadata fresh_metadata(void) {
    naula_metadata metadata;
    memset(&metadata, 0, sizeof metadata);
    metadata.size = sizeof metadata;
    return metadata;
}

static const naula_provider *github(void) {
    return naula_provider_github();
}

static int ends_with(const char *text, const char *end) {
    size_t length = strlen(text);
    size_t end_length = strlen(end);
    return length >= end_length && strcmp(text + length - end_length, end) == 0;
}

/** The one request the log holds sent exactly body, declared by its length. */
static void check_sole_body(const char *log, const char *request_line, const char *body) {
    char declared[64];
    char ending[256];
    snprintf(declared, sizeof declared, "Content-Length: %u\r\n", (unsigned)strlen(body));
    snprintf(ending, sizeof ending, "\r\n\r\n%s", body);
    CHECK(log != NULL && strstr(log, request_line) == log);
    CHECK(log != NULL && strstr(log, declared) != NULL);
    CHECK(log != NULL && strstr(log, "Content-Type: application/json\r\n") != NULL);
    CHECK(log != NULL && strstr(log, "Authorization: token " TOKEN_TEXT "\r\n") != NULL);
    CHECK(log != NULL && ends_with(log, ending));
}

static void rename_patches_the_name_keeping_the_number(void) {
    http_route routes[1] = {route(REPOSITORY_ROUTE, 200, RENAMED)};
    http_fixture *server = serve(routes, 1);
    naula_refusal refusal = fresh_refusal();
    CHECK(github()->rename(github()->context, NULL, NUMBER, "renamed", TOKEN_TEXT, &refusal) == NAULA_OK);
    char *log = finish(server);
    check_sole_body(log, "PATCH " REPOSITORY_ROUTE " HTTP/1.1\r\n", "{\"name\":\"renamed\"}");
    naula_memory_release(log);
    server = serve(routes, 1);
    CHECK(github()->rename(github()->context, NULL, NUMBER, "other", TOKEN_TEXT, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "rename_not_applied", "a rename answered with another name");
    naula_memory_release(finish(server));
}

static void a_409_on_rename_is_the_name_taken_and_a_422_is_refused_by_validation(void) {
    http_route routes[1] = {route(REPOSITORY_ROUTE, 409, "{\"message\":\"Conflict\"}")};
    http_fixture *server = serve(routes, 1);
    naula_refusal refusal = fresh_refusal();
    CHECK(github()->rename(github()->context, NULL, NUMBER, "taken", TOKEN_TEXT, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "name_taken", "a taken name");
    CHECK(strstr(refusal.detail, "taken") != NULL);
    naula_memory_release(finish(server));
    routes[0] = route(REPOSITORY_ROUTE, 422, "{\"message\":\"Validation Failed\"}");
    server = serve(routes, 1);
    CHECK(github()->rename(github()->context, NULL, NUMBER, "taken", TOKEN_TEXT, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "refused_by_validation", "a name the host refused");
    CHECK(refusal.fix != NULL && strstr(refusal.fix, "taken or reserved") != NULL);
    naula_memory_release(finish(server));
}

static void a_308_on_a_write_is_redirected_and_never_followed(void) {
    char location[160];
    http_route routes[2] = {route(REPOSITORY_ROUTE, 308, ""), route("/elsewhere", 200, ARCHIVED)};
    routes[0].extra_headers = location;
    http_fixture *server = serve(routes, 2);
    snprintf(location, sizeof location, "Location: http://127.0.0.1:%u/elsewhere\r\n",
             (unsigned)http_fixture_port(server));
    naula_refusal refusal = fresh_refusal();
    CHECK(github()->archive(github()->context, NULL, NUMBER, TOKEN_TEXT, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "redirected", "a 308 on a write");
    char *log = finish(server);
    CHECK(log != NULL && strstr(log, "/elsewhere") == NULL);
    naula_memory_release(log);
}

static void transfer_posts_the_new_owner(void) {
    http_route routes[1] = {route(REPOSITORY_ROUTE "/transfer", 202, LIVE)};
    http_fixture *server = serve(routes, 1);
    naula_refusal refusal = fresh_refusal();
    CHECK(github()->transfer(github()->context, NULL, NUMBER, "spisor", TOKEN_TEXT, &refusal) == NAULA_OK);
    char *log = finish(server);
    check_sole_body(log, "POST " REPOSITORY_ROUTE "/transfer HTTP/1.1\r\n", "{\"new_owner\":\"spisor\"}");
    naula_memory_release(log);
    routes[0] = route(REPOSITORY_ROUTE "/transfer", 422, "{}");
    server = serve(routes, 1);
    CHECK(github()->transfer(github()->context, NULL, NUMBER, "spisor", TOKEN_TEXT, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "transfer_refused", "a refused transfer");
    naula_memory_release(finish(server));
}

static void archive_patches_archived_true(void) {
    http_route routes[1] = {route(REPOSITORY_ROUTE, 200, ARCHIVED)};
    http_fixture *server = serve(routes, 1);
    naula_refusal refusal = fresh_refusal();
    CHECK(github()->archive(github()->context, NULL, NUMBER, TOKEN_TEXT, &refusal) == NAULA_OK);
    char *log = finish(server);
    check_sole_body(log, "PATCH " REPOSITORY_ROUTE " HTTP/1.1\r\n", "{\"archived\":true}");
    naula_memory_release(log);
    routes[0] = route(REPOSITORY_ROUTE, 200, LIVE);
    server = serve(routes, 1);
    CHECK(github()->archive(github()->context, NULL, NUMBER, TOKEN_TEXT, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "archive_not_applied", "an archive answered as not archived");
    naula_memory_release(finish(server));
}

static naula_metadata visibility_metadata(const char *settings) {
    naula_metadata metadata = fresh_metadata();
    metadata.settings = cJSON_Parse(settings);
    metadata.present = NAULA_METADATA_SETTINGS;
    return metadata;
}

static void create_posts_to_the_org_or_the_user(void) {
    const char org_answer[] = "{\"id\":77,\"name\":\"naula\",\"owner\":{\"login\":\"spisor\"}}";
    const char user_answer[] = "{\"id\":78,\"name\":\"naula\",\"owner\":{\"login\":\"finn\"}}";
    http_route routes[5] = {route("/orgs/spisor", 200, "{\"login\":\"spisor\"}"),
                            route("/orgs/spisor/repos", 201, org_answer), route("/orgs/finn", 404, NOT_FOUND_BODY),
                            route("/user", 200, "{\"login\":\"finn\"}"), route("/user/repos", 201, user_answer)};
    http_fixture *server = serve(routes, 5);
    naula_refusal refusal = fresh_refusal();
    naula_metadata metadata = visibility_metadata("{\"visibility\":\"public\",\"wiki\":false}");
    uint64_t number = 0;
    CHECK(github()->create(github()->context, NULL, "spisor", "naula", &metadata, TOKEN_TEXT, &number, &refusal) ==
          NAULA_OK);
    CHECK(number == 77);
    CHECK(github()->create(github()->context, NULL, "finn", "naula", NULL, TOKEN_TEXT, &number, &refusal) ==
          NAULA_OK);
    CHECK(number == 78);
    char *log = finish(server);
    const char *org_post = log == NULL ? NULL : strstr(log, "POST /orgs/spisor/repos HTTP/1.1\r\n");
    const char *user_post = log == NULL ? NULL : strstr(log, "POST /user/repos HTTP/1.1\r\n");
    CHECK(org_post != NULL && strstr(org_post, "\r\n\r\n{\"name\":\"naula\",\"private\":false}") != NULL);
    CHECK(user_post != NULL && strstr(user_post, "\r\n\r\n{\"name\":\"naula\",\"private\":true}") != NULL);
    CHECK(log != NULL && strstr(log, "wiki") == NULL);
    naula_memory_release(log);
    naula_metadata_release(&metadata);
}

static void create_never_posts_for_an_account_that_is_neither_an_org_nor_the_token_user(void) {
    const char user_answer[] = "{\"id\":78,\"name\":\"naula\",\"owner\":{\"login\":\"finn\"}}";
    http_route routes[3] = {route("/orgs/someone", 404, NOT_FOUND_BODY), route("/user", 200, "{\"login\":\"finn\"}"),
                            route("/user/repos", 201, user_answer)};
    http_fixture *server = serve(routes, 3);
    naula_refusal refusal = fresh_refusal();
    uint64_t number = 5;
    CHECK(github()->create(github()->context, NULL, "someone", "naula", NULL, TOKEN_TEXT, &number, &refusal) ==
          NAULA_REFUSED);
    check_text(refusal.code, "account_not_writable", "an account that is neither an org nor the token's user");
    CHECK(number == 0);
    char *log = finish(server);
    CHECK(log != NULL && strstr(log, "POST") == NULL);
    naula_memory_release(log);
}

static void create_refuses_a_visibility_that_is_neither_public_nor_private_before_any_request(void) {
    http_route routes[1] = {route("/orgs/spisor", 200, "{}")};
    http_fixture *server = serve(routes, 1);
    naula_refusal refusal = fresh_refusal();
    naula_metadata metadata = visibility_metadata("{\"visibility\":\"internal\"}");
    uint64_t number = 0;
    CHECK(github()->create(github()->context, NULL, "spisor", "naula", &metadata, TOKEN_TEXT, &number, &refusal) ==
          NAULA_REFUSED);
    check_text(refusal.code, "invalid_request", "an internal visibility");
    char *log = finish(server);
    CHECK(log != NULL && log[0] == '\0');
    naula_memory_release(log);
    naula_metadata_release(&metadata);
}

static void create_refuses_a_taken_name_and_an_answer_for_another_repository(void) {
    http_route routes[2] = {route("/orgs/spisor", 200, "{}"), route("/orgs/spisor/repos", 422, "{}")};
    http_fixture *server = serve(routes, 2);
    naula_refusal refusal = fresh_refusal();
    uint64_t number = 0;
    CHECK(github()->create(github()->context, NULL, "spisor", "naula", NULL, TOKEN_TEXT, &number, &refusal) ==
          NAULA_REFUSED);
    check_text(refusal.code, "refused_by_validation", "a 422 on create");
    naula_memory_release(finish(server));
    routes[1] = route("/orgs/spisor/repos", 409, "{}");
    server = serve(routes, 2);
    CHECK(github()->create(github()->context, NULL, "spisor", "naula", NULL, TOKEN_TEXT, &number, &refusal) ==
          NAULA_REFUSED);
    check_text(refusal.code, "name_taken", "a 409 on create");
    naula_memory_release(finish(server));
    routes[1] = route("/orgs/spisor/repos", 201, "{\"id\":77,\"name\":\"naula\",\"owner\":{\"login\":\"finn\"}}");
    server = serve(routes, 2);
    CHECK(github()->create(github()->context, NULL, "spisor", "naula", NULL, TOKEN_TEXT, &number, &refusal) ==
          NAULA_REFUSED);
    check_text(refusal.code, "created_elsewhere", "created under another account");
    CHECK(number == 0);
    naula_memory_release(finish(server));
}

static void writes_need_a_token_and_a_plain_name_before_any_request(void) {
    http_route routes[1] = {route(REPOSITORY_ROUTE, 200, RENAMED)};
    http_fixture *server = serve(routes, 1);
    naula_refusal refusal = fresh_refusal();
    uint64_t number = 0;
    uint32_t applied = 9;
    naula_metadata wanted = fresh_metadata();
    CHECK(github()->rename(github()->context, NULL, NUMBER, "renamed", NULL, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "token_needed", "a rename without a token");
    CHECK(github()->archive(github()->context, NULL, NUMBER, NULL, &refusal) == NAULA_REFUSED);
    CHECK(github()->write_metadata(github()->context, NULL, NUMBER, &wanted, NULL, &applied, &refusal) ==
          NAULA_REFUSED);
    CHECK(applied == 0);
    CHECK(github()->create(github()->context, NULL, "spisor", "naula", NULL, NULL, &number, &refusal) ==
          NAULA_REFUSED);
    const char *const unusable[] = {"-rf", "a/b", "..", "has space", ""};
    for (size_t index = 0; index < 5; index++) {
        refusal = fresh_refusal();
        CHECK(github()->rename(github()->context, NULL, NUMBER, unusable[index], TOKEN_TEXT, &refusal) ==
              NAULA_REFUSED);
        check_text(refusal.code, "invalid_request", unusable[index]);
        CHECK(github()->transfer(github()->context, NULL, NUMBER, unusable[index], TOKEN_TEXT, &refusal) ==
              NAULA_REFUSED);
        CHECK(github()->create(github()->context, NULL, unusable[index], "naula", NULL, TOKEN_TEXT, &number,
                               &refusal) == NAULA_REFUSED);
        CHECK(github()->create(github()->context, NULL, "spisor", unusable[index], NULL, TOKEN_TEXT, &number,
                               &refusal) == NAULA_REFUSED);
    }
    char *log = finish(server);
    CHECK(log != NULL && log[0] == '\0');
    naula_memory_release(log);
}

static void a_404_on_a_write_is_not_permitted_while_the_repository_lives_and_not_found_once_it_is_gone(void) {
    http_route routes[2] = {route(REPOSITORY_ROUTE, 404, NOT_FOUND_BODY), route(REPOSITORY_ROUTE, 200, LIVE)};
    routes[0].answer_limit = 1;
    http_fixture *server = serve(routes, 2);
    naula_refusal refusal = fresh_refusal();
    CHECK(github()->rename(github()->context, NULL, NUMBER, "renamed", TOKEN_TEXT, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "not_permitted", "a 404 on a live repository");
    naula_memory_release(finish(server));
    routes[1] = route(REPOSITORY_ROUTE, 404, NOT_FOUND_BODY);
    server = serve(routes, 2);
    CHECK(github()->archive(github()->context, NULL, NUMBER, TOKEN_TEXT, &refusal) == NAULA_NOT_FOUND);
    naula_memory_release(finish(server));
}

static void a_write_answered_with_a_redirect_is_refused_and_never_repeated(void) {
    char location[160];
    http_route routes[2] = {route(REPOSITORY_ROUTE, 307, ""), route("/elsewhere", 200, RENAMED)};
    routes[0].extra_headers = location;
    http_fixture *server = serve(routes, 2);
    snprintf(location, sizeof location, "Location: http://127.0.0.1:%u/elsewhere\r\n",
             (unsigned)http_fixture_port(server));
    naula_refusal refusal = fresh_refusal();
    CHECK(github()->rename(github()->context, NULL, NUMBER, "renamed", TOKEN_TEXT, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "redirected", "a redirected write");
    char *log = finish(server);
    CHECK(log != NULL && strstr(log, "/elsewhere") == NULL && strstr(log, "{\"name\":\"renamed\"}") != NULL);
    naula_memory_release(log);
}

#define WITH_HOMEPAGE "{\"id\":1296269,\"name\":\"Hello-World\",\"homepage\":\"https://naula.dev\"}"

static void write_metadata_leaves_an_absent_member_alone(void) {
    http_route routes[2] = {route(REPOSITORY_ROUTE, 200, WITH_HOMEPAGE),
                            route(REPOSITORY_ROUTE "/topics", 200, "{\"names\":[\"git\",\"tools\"]}")};
    http_fixture *server = serve(routes, 2);
    naula_refusal refusal = fresh_refusal();
    naula_metadata wanted = fresh_metadata();
    char description[] = "Words that must not be sent.";
    char homepage[] = "https://naula.dev";
    wanted.description = description;
    wanted.homepage = homepage;
    wanted.present = NAULA_METADATA_HOMEPAGE;
    uint32_t applied = 0;
    CHECK(github()->write_metadata(github()->context, NULL, NUMBER, &wanted, TOKEN_TEXT, &applied, &refusal) ==
          NAULA_OK);
    CHECK(applied == NAULA_METADATA_HOMEPAGE);
    char *log = finish(server);
    check_sole_body(log, "PATCH " REPOSITORY_ROUTE " HTTP/1.1\r\n", "{\"homepage\":\"https://naula.dev\"}");
    CHECK(log != NULL && strstr(log, "description") == NULL && strstr(log, "/topics") == NULL);
    naula_memory_release(log);
    applied = 9;
    wanted.present = 0;
    server = serve(routes, 2);
    CHECK(github()->write_metadata(github()->context, NULL, NUMBER, &wanted, TOKEN_TEXT, &applied, &refusal) ==
          NAULA_OK);
    CHECK(applied == 0);
    log = finish(server);
    CHECK(log != NULL && log[0] == '\0');
    naula_memory_release(log);
}

static void a_member_the_host_silently_ignored_is_not_applied(void) {
    http_route routes[2] = {route(REPOSITORY_ROUTE, 200, WITH_HOMEPAGE),
                            route(REPOSITORY_ROUTE "/topics", 200, "{\"names\":[\"other\"]}")};
    http_fixture *server = serve(routes, 2);
    naula_refusal refusal = fresh_refusal();
    naula_metadata wanted = fresh_metadata();
    char description[] = "Words.";
    char homepage[] = "https://naula.dev";
    char git[] = "git";
    char *topics[] = {git};
    wanted.description = description;
    wanted.homepage = homepage;
    wanted.topics = topics;
    wanted.topic_count = 1;
    wanted.settings = cJSON_Parse("{\"wiki\":false}");
    wanted.present = NAULA_METADATA_DESCRIPTION | NAULA_METADATA_HOMEPAGE | NAULA_METADATA_SETTINGS |
                     NAULA_METADATA_TOPICS;
    uint32_t applied = 0;
    CHECK(github()->write_metadata(github()->context, NULL, NUMBER, &wanted, TOKEN_TEXT, &applied, &refusal) ==
          NAULA_OK);
    CHECK(applied == NAULA_METADATA_HOMEPAGE);
    naula_memory_release(finish(server));
    cJSON_Delete(wanted.settings);
}

#define EVERY_MEMBER                                                                                                   \
    "{\"id\":1296269,\"name\":\"Hello-World\",\"description\":\"Words.\",\"homepage\":\"https://naula.dev\","          \
    "\"private\":false,\"default_branch\":\"main\",\"has_issues\":true,\"has_wiki\":false}"

static void write_metadata_writes_every_present_member(void) {
    http_route routes[2] = {route(REPOSITORY_ROUTE, 200, EVERY_MEMBER),
                            route(REPOSITORY_ROUTE "/topics", 200, "{\"names\":[\"git\",\"tools\"]}")};
    http_fixture *server = serve(routes, 2);
    naula_refusal refusal = fresh_refusal();
    naula_metadata wanted = fresh_metadata();
    char description[] = "Words.";
    char homepage[] = "https://naula.dev";
    char git[] = "git";
    char tools[] = "tools";
    char *topics[] = {git, tools};
    wanted.description = description;
    wanted.homepage = homepage;
    wanted.topics = topics;
    wanted.topic_count = 2;
    wanted.settings =
        cJSON_Parse("{\"visibility\":\"public\",\"defaultBranch\":\"main\",\"issues\":true,\"wiki\":false}");
    wanted.present = NAULA_METADATA_DESCRIPTION | NAULA_METADATA_TOPICS | NAULA_METADATA_HOMEPAGE |
                     NAULA_METADATA_SETTINGS;
    uint32_t applied = 0;
    CHECK(github()->write_metadata(github()->context, NULL, NUMBER, &wanted, TOKEN_TEXT, &applied, &refusal) ==
          NAULA_OK);
    CHECK(applied == wanted.present);
    char *log = finish(server);
    CHECK(log != NULL && strstr(log, "PATCH " REPOSITORY_ROUTE " HTTP/1.1\r\n") != NULL);
    CHECK(log != NULL &&
          strstr(log, "\r\n\r\n{\"description\":\"Words.\",\"homepage\":\"https://naula.dev\",\"private\":false,"
                      "\"default_branch\":\"main\",\"has_issues\":true,\"has_wiki\":false}") != NULL);
    CHECK(log != NULL && strstr(log, "PUT " REPOSITORY_ROUTE "/topics HTTP/1.1\r\n") != NULL);
    CHECK(log != NULL && ends_with(log, "\r\n\r\n{\"names\":[\"git\",\"tools\"]}"));
    naula_memory_release(log);
    cJSON_Delete(wanted.settings);
}

static void write_metadata_reports_topics_unsupported_on_a_provider_without_them(void) {
    static const forge_metadata_names without_topics = {"website", NULL};
    static const forge_flavor flavor = {"Topicless", "check the host", NULL};
    naula_refusal refusal = fresh_refusal();
    naula_metadata wanted = fresh_metadata();
    char description[] = "Words.";
    char git[] = "git";
    char *topics[] = {git};
    wanted.description = description;
    wanted.topics = topics;
    wanted.topic_count = 1;
    wanted.settings = cJSON_Parse("{\"discussions\":true}");
    wanted.present = NAULA_METADATA_DESCRIPTION | NAULA_METADATA_TOPICS | NAULA_METADATA_SETTINGS;
    forge_metadata_bodies bodies;
    CHECK(naula_forge_metadata_bodies(&flavor, &without_topics, &wanted, &bodies, &refusal) == NAULA_OK);
    CHECK(bodies.topics == NULL);
    CHECK(bodies.edit_members == NAULA_METADATA_DESCRIPTION);
    char *edit = bodies.edit == NULL ? NULL : cJSON_PrintUnformatted(bodies.edit);
    check_text(edit, "{\"description\":\"Words.\"}", "the edit carries no setting the host lacks");
    cJSON_free(edit);
    naula_forge_metadata_bodies_release(&bodies);
    cJSON_Delete(wanted.settings);
    wanted.settings = cJSON_Parse("{\"visibility\":\"internal\"}");
    CHECK(naula_forge_metadata_bodies(&flavor, &without_topics, &wanted, &bodies, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "invalid_request", "a setting out of shape");
    CHECK(bodies.edit == NULL && bodies.topics == NULL);
    cJSON_Delete(wanted.settings);
}

static void read_metadata_reads_every_member(void) {
    const char answer[] = "{\"id\":1296269,\"name\":\"Hello-World\",\"owner\":{\"login\":\"octocat\"},"
                          "\"description\":null,\"homepage\":\"https://naula.dev\",\"topics\":[\"git\",\"tools\"],"
                          "\"private\":true,\"default_branch\":\"main\",\"has_issues\":true,\"has_wiki\":false}";
    http_route routes[1] = {route(REPOSITORY_ROUTE, 200, answer)};
    http_fixture *server = serve(routes, 1);
    naula_refusal refusal = fresh_refusal();
    naula_metadata metadata = fresh_metadata();
    CHECK(github()->read_metadata(github()->context, NULL, NUMBER, NULL, &metadata, &refusal) == NAULA_OK);
    CHECK(metadata.present == (NAULA_METADATA_DESCRIPTION | NAULA_METADATA_TOPICS | NAULA_METADATA_HOMEPAGE |
                               NAULA_METADATA_SETTINGS));
    check_text(metadata.description, "", "a null description");
    check_text(metadata.homepage, "https://naula.dev", "homepage");
    CHECK(metadata.topic_count == 2);
    check_text(metadata.topic_count == 2 ? metadata.topics[1] : NULL, "tools", "second topic");
    char *settings = metadata.settings == NULL ? NULL : cJSON_PrintUnformatted(metadata.settings);
    check_text(settings, "{\"visibility\":\"private\",\"defaultBranch\":\"main\",\"issues\":true,\"wiki\":false}",
               "settings");
    cJSON_free(settings);
    naula_metadata_release(&metadata);
    CHECK(metadata.size == sizeof metadata && metadata.description == NULL);
    naula_memory_release(finish(server));
}

static naula_status refuse_nothing(void *context, uint32_t *supported, int *anonymous_lookup) {
    (void)context;
    *supported = 0;
    *anonymous_lookup = 0;
    return NAULA_OK;
}

static void a_provider_of_the_first_shape_offers_no_writes(void) {
    naula_provider first_shape = *github();
    first_shape.word = "first-shape";
    first_shape.describe = refuse_nothing;
    first_shape.size = (uint32_t)(offsetof(naula_provider, release_index) + sizeof first_shape.release_index);
    naula_provider missing_write = *github();
    missing_write.word = "missing-write";
    missing_write.archive = NULL;
    naula_providers *providers = NULL;
    naula_refusal refusal = fresh_refusal();
    CHECK(naula_providers_open(&providers) == NAULA_OK);
    CHECK(naula_provider_offers_writes(naula_providers_find(providers, "github", &refusal)));
    CHECK(naula_providers_register(providers, &first_shape, &refusal) == NAULA_OK);
    const naula_provider *registered = naula_providers_find(providers, "first-shape", &refusal);
    CHECK(registered != NULL && !naula_provider_offers_writes(registered));
    CHECK(naula_providers_register(providers, &missing_write, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "invalid_provider", "a write left NULL");
    naula_providers_close(providers);
}

static void an_account_rename_is_unsupported_and_names_the_website(void) {
    naula_refusal refusal = fresh_refusal();
    CHECK(naula_refuse_account_rename("github", &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "unsupported_operation", "account rename");
    check_text(refusal.fix, NAULA_ACCOUNT_RENAME_FIX, "account rename fix");
    uint32_t supported = 0;
    int anonymous = 0;
    CHECK(github()->describe(github()->context, &supported, &anonymous) == NAULA_OK);
    CHECK((supported & NAULA_OP_RENAME) != 0 && (supported & NAULA_OP_WRITE_METADATA) != 0);
}

int main(void) {
    naula_json_use_allocator();
    naula_fetch_allow_plain_http(1);
    rename_patches_the_name_keeping_the_number();
    a_409_on_rename_is_the_name_taken_and_a_422_is_refused_by_validation();
    a_308_on_a_write_is_redirected_and_never_followed();
    transfer_posts_the_new_owner();
    archive_patches_archived_true();
    create_posts_to_the_org_or_the_user();
    create_never_posts_for_an_account_that_is_neither_an_org_nor_the_token_user();
    create_refuses_a_visibility_that_is_neither_public_nor_private_before_any_request();
    create_refuses_a_taken_name_and_an_answer_for_another_repository();
    writes_need_a_token_and_a_plain_name_before_any_request();
    a_404_on_a_write_is_not_permitted_while_the_repository_lives_and_not_found_once_it_is_gone();
    a_write_answered_with_a_redirect_is_refused_and_never_repeated();
    write_metadata_leaves_an_absent_member_alone();
    a_member_the_host_silently_ignored_is_not_applied();
    write_metadata_writes_every_present_member();
    write_metadata_reports_topics_unsupported_on_a_provider_without_them();
    read_metadata_reads_every_member();
    a_provider_of_the_first_shape_offers_no_writes();
    an_account_rename_is_unsupported_and_names_the_website();
    return check_summary("provider_github_write_test");
}
