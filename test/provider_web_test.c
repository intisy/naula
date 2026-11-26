#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "http_fixture.h"
#include "naula/memory.h"
#include "naula/provider.h"
#include "provider_web_hooks.h"
#include "transport_hooks.h"
#include "naula/json.h"

#define HEX "dd92836cca7c292258635112ecfe1acb7b4f6ecd"
#define OTHER_HEX "0123456789abcdef0123456789abcdef01234567"
#define DOCUMENT_PATH "/.well-known/repo-identity/" HEX ".json"
#define DOCUMENT "{\"id\":\"git:" HEX "\",\"homes\":[{\"provider\":\"web\",\"domain\":\"naula.dev\"}]}"

static http_route route(int status, const char *body) {
    http_route made;
    memset(&made, 0, sizeof made);
    made.path = DOCUMENT_PATH;
    made.status = status;
    made.body = body;
    made.body_length = strlen(body);
    made.declared_length = HTTP_ROUTE_BODY_LENGTH;
    return made;
}

static http_fixture *serve(const http_route *routes) {
    http_fixture *server = http_fixture_start(routes, 1);
    CHECK(server != NULL);
    if (server != NULL) {
        char root[64];
        snprintf(root, sizeof root, "http://127.0.0.1:%u", (unsigned)http_fixture_port(server));
        naula_web_use_root(root);
    }
    return server;
}

static char *finish(http_fixture *server) {
    naula_web_use_root(NULL);
    return http_fixture_stop(server);
}

static naula_refusal fresh_refusal(void) {
    naula_refusal refusal;
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    return refusal;
}

static naula_status read_document(const char *domain, const char *hex, uint8_t **bytes, size_t *length,
                                  naula_refusal *refusal) {
    return naula_web_document(domain, hex, bytes, length, refusal);
}

static void web_reads_the_well_known_document(void) {
    http_route routes[1] = {route(200, DOCUMENT)};
    http_fixture *server = serve(routes);
    naula_refusal refusal = fresh_refusal();
    uint8_t *bytes = NULL;
    size_t length = 0;
    CHECK(read_document("naula.dev", HEX, &bytes, &length, &refusal) == NAULA_OK);
    CHECK(length == strlen(DOCUMENT) && bytes != NULL && memcmp(bytes, DOCUMENT, length) == 0);
    naula_memory_release(bytes);
    char *log = finish(server);
    CHECK(log != NULL && strstr(log, "GET " DOCUMENT_PATH) != NULL && strstr(log, "Accept: application/json") != NULL);
    naula_memory_release(log);
}

static void web_refuses_a_document_for_another_id(void) {
    http_route routes[1] = {route(200, "{\"id\":\"git:" OTHER_HEX "\",\"homes\":[]}")};
    http_fixture *server = serve(routes);
    naula_refusal refusal = fresh_refusal();
    uint8_t *bytes = NULL;
    size_t length = 0;
    CHECK(read_document("naula.dev", HEX, &bytes, &length, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "document_for_another_id", "another id");
    CHECK(bytes == NULL && length == 0);
    naula_memory_release(finish(server));
}

static void web_refuses_a_document_that_is_not_an_identity_document(void) {
    const char *const bodies[] = {"not json", "[]", "{\"id\":\"git:" HEX "\"}", "{\"homes\":[]}",
                                  "{\"id\":\"local:name\",\"homes\":[]}", "{\"id\":\"nonsense\",\"homes\":[]}"};
    for (size_t index = 0; index < sizeof bodies / sizeof bodies[0]; index++) {
        http_route routes[1] = {route(200, bodies[index])};
        http_fixture *server = serve(routes);
        naula_refusal refusal = fresh_refusal();
        uint8_t *bytes = NULL;
        size_t length = 0;
        CHECK(read_document("naula.dev", HEX, &bytes, &length, &refusal) == NAULA_MALFORMED);
        CHECK(bytes == NULL);
        naula_memory_release(finish(server));
    }
}

static void web_a_missing_document_is_not_found_and_other_statuses_are_not(void) {
    const int statuses[] = {404, 403, 500};
    const naula_status answers[] = {NAULA_NOT_FOUND, NAULA_REFUSED, NAULA_REFUSED};
    for (size_t index = 0; index < 3; index++) {
        http_route routes[1] = {route(statuses[index], "{}")};
        http_fixture *server = serve(routes);
        naula_refusal refusal = fresh_refusal();
        uint8_t *bytes = NULL;
        size_t length = 0;
        CHECK(read_document("naula.dev", HEX, &bytes, &length, &refusal) == answers[index]);
        naula_memory_release(finish(server));
    }
}

static void web_a_timeout_is_timed_out(void) {
    http_route routes[1] = {route(200, DOCUMENT)};
    routes[0].stall_milliseconds = 1500;
    http_fixture *server = serve(routes);
    naula_fetch_use_deadline(300);
    naula_refusal refusal = fresh_refusal();
    uint8_t *bytes = NULL;
    size_t length = 0;
    CHECK(read_document("naula.dev", HEX, &bytes, &length, &refusal) == NAULA_TIMED_OUT);
    CHECK(refusal.fix != NULL && bytes == NULL);
    naula_fetch_use_deadline(0);
    naula_memory_release(finish(server));
}

static void web_a_domain_that_does_not_resolve_is_unreachable_never_not_found(void) {
    naula_web_use_root("http://naula-test.invalid");
    naula_refusal refusal = fresh_refusal();
    uint8_t *bytes = NULL;
    size_t length = 0;
    CHECK(read_document("naula.dev", HEX, &bytes, &length, &refusal) == NAULA_UNREACHABLE);
    check_text(refusal.code, "host_not_resolved", "unresolved domain");
    CHECK(refusal.fix != NULL && bytes == NULL);
    naula_web_use_root(NULL);
}

static void web_refuses_a_domain_or_an_identity_it_cannot_address(void) {
    naula_refusal refusal = fresh_refusal();
    uint8_t *bytes = NULL;
    size_t length = 0;
    CHECK(read_document("bad domain/x", HEX, &bytes, &length, &refusal) == NAULA_REFUSED);
    CHECK(read_document("", HEX, &bytes, &length, &refusal) == NAULA_REFUSED);
    CHECK(read_document(NULL, HEX, &bytes, &length, &refusal) == NAULA_REFUSED);
    CHECK(read_document("naula.dev", "DD92836CCA7C292258635112ECFE1ACB7B4F6ECD", &bytes, &length, &refusal) ==
          NAULA_REFUSED);
    CHECK(read_document("naula.dev", "abc", &bytes, &length, &refusal) == NAULA_REFUSED);
    CHECK(read_document("naula.dev", NULL, &bytes, &length, &refusal) == NAULA_REFUSED);
}

static void the_web_provider_supports_no_operation_and_refuses_each(void) {
    const naula_provider *web = naula_provider_web();
    uint32_t supported = 9;
    int anonymous = 1;
    naula_refusal refusal = fresh_refusal();
    uint8_t *bytes = NULL;
    size_t length = 0;
    naula_repository repository;
    memset(&repository, 0, sizeof repository);
    repository.size = sizeof repository;
    check_text(web->word, "web", "word");
    CHECK(web->describe(web->context, &supported, &anonymous) == NAULA_OK && supported == 0);
    CHECK(web->lookup(web->context, "naula.dev", 1, NULL, &repository, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "unsupported_operation", "lookup");
    CHECK(web->read_file(web->context, "naula.dev", 1, "main", "repo.json", NULL, &bytes, &length, &refusal) ==
          NAULA_REFUSED);
    naula_release_index index;
    memset(&index, 0, sizeof index);
    index.size = sizeof index;
    CHECK(web->release_index(web->context, "naula.dev", 1, "v1", "index.json", NULL, &index, &refusal) ==
          NAULA_REFUSED);
}

static void the_web_provider_refuses_every_write(void) {
    const naula_provider *web = naula_provider_web();
    naula_refusal refusal = fresh_refusal();
    naula_metadata metadata;
    memset(&metadata, 0, sizeof metadata);
    metadata.size = sizeof metadata;
    uint64_t number = 5;
    uint32_t applied = 5;
    CHECK(naula_provider_offers_writes(web));
    CHECK(web->create(web->context, "naula.dev", "a", "b", NULL, "t", &number, &refusal) == NAULA_REFUSED);
    CHECK(number == 0);
    check_text(refusal.code, "unsupported_operation", "create");
    CHECK(web->rename(web->context, "naula.dev", 1, "b", "t", &refusal) == NAULA_REFUSED);
    CHECK(web->transfer(web->context, "naula.dev", 1, "a", "t", &refusal) == NAULA_REFUSED);
    CHECK(web->archive(web->context, "naula.dev", 1, "t", &refusal) == NAULA_REFUSED);
    CHECK(web->read_metadata(web->context, "naula.dev", 1, "t", &metadata, &refusal) == NAULA_REFUSED);
    CHECK(web->write_metadata(web->context, "naula.dev", 1, &metadata, "t", &applied, &refusal) == NAULA_REFUSED);
    CHECK(applied == 0);
    check_text(refusal.code, "unsupported_operation", "write_metadata");
    naula_providers *providers = NULL;
    CHECK(naula_providers_open(&providers) == NAULA_OK);
    naula_providers_close(providers);
}

int main(void) {
    naula_json_use_allocator();
    naula_fetch_allow_plain_http(1);
    web_reads_the_well_known_document();
    web_refuses_a_document_for_another_id();
    web_refuses_a_document_that_is_not_an_identity_document();
    web_a_missing_document_is_not_found_and_other_statuses_are_not();
    web_a_timeout_is_timed_out();
    web_a_domain_that_does_not_resolve_is_unreachable_never_not_found();
    web_refuses_a_domain_or_an_identity_it_cannot_address();
    the_web_provider_supports_no_operation_and_refuses_each();
    the_web_provider_refuses_every_write();
    return check_summary("provider_web_test");
}
