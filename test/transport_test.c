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
#include "naula/transport.h"
#include "transport_hooks.h"
#include "naula/json.h"

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#else
#include <stdlib.h>
#endif

#define BODY_LENGTH 4096
#define CUT_BODY_LENGTH 500
#define ETAG "\"one\""
#define LAST_MODIFIED "Wed, 21 Oct 2015 07:28:00 GMT"
#define SECRET "token s3cr3t-value"

static char body[BODY_LENGTH];
static uint8_t received[BODY_LENGTH];

static void prepare_body(void) {
    for (size_t index = 0; index < BODY_LENGTH; index++) {
        body[index] = (char)('a' + index % 26);
    }
}

static naula_fetch_answer empty_answer(void) {
    naula_fetch_answer answer;
    memset(&answer, 0, sizeof answer);
    answer.size = sizeof answer;
    return answer;
}

static void release(naula_fetch_answer *answer) {
    naula_memory_release(answer->etag);
    naula_memory_release(answer->last_modified);
}

static void url_of(char *url, size_t capacity, const char *host, unsigned short port, const char *path) {
    snprintf(url, capacity, "http://%s:%u%s", host, (unsigned)port, path);
}

static size_t occurrences(const char *text, const char *needle) {
    size_t count = 0;
    for (const char *found = strstr(text, needle); found != NULL; found = strstr(found + 1, needle)) {
        count++;
    }
    return count;
}

static naula_fetch_answer fetch_into_buffer(const char *url, const char *authorization, unsigned deadline,
                                            size_t capacity, naula_fetch_sink *sink) {
    naula_fetch_answer answer = empty_answer();
    naula_fetch_sink_start_memory(sink, received, capacity);
    naula_fetch_into_memory(url, authorization, deadline, sink, &answer);
    return answer;
}

static void a_200_is_received_with_its_validators(unsigned short port) {
    char url[128];
    naula_fetch_sink sink;
    char hex[NAULA_SHA256_HEX_SIZE];
    naula_sha256 expected;
    char expected_hex[NAULA_SHA256_HEX_SIZE];
    url_of(url, sizeof url, "127.0.0.1", port, "/file");
    naula_fetch_answer answer = fetch_into_buffer(url, NULL, 5000, BODY_LENGTH, &sink);
    CHECK(answer.outcome == NAULA_FETCH_RECEIVED);
    CHECK(sink.bytes == BODY_LENGTH && memcmp(received, body, BODY_LENGTH) == 0);
    naula_sha256_start(&expected);
    naula_sha256_add(&expected, (const uint8_t *)body, BODY_LENGTH);
    naula_sha256_hex(&expected, expected_hex);
    naula_sha256_hex(&sink.hash, hex);
    check_text(hex, expected_hex, "hash of the body");
    check_text(answer.etag, ETAG, "etag");
    check_text(answer.last_modified, LAST_MODIFIED, "last modified");
    release(&answer);
}

static void another_status_is_answered_as_status(unsigned short port) {
    char url[128];
    naula_fetch_sink sink;
    url_of(url, sizeof url, "127.0.0.1", port, "/missing");
    naula_fetch_answer answer = fetch_into_buffer(url, NULL, 5000, BODY_LENGTH, &sink);
    CHECK(answer.outcome == NAULA_FETCH_STATUS && answer.status == 404);
    release(&answer);
}

static void a_body_cut_off_answers_its_declared_length(unsigned short port) {
    char url[128];
    naula_fetch_sink sink;
    url_of(url, sizeof url, "127.0.0.1", port, "/cut");
    naula_fetch_answer answer = fetch_into_buffer(url, NULL, 5000, BODY_LENGTH, &sink);
    CHECK(answer.outcome == NAULA_FETCH_CUT_OFF && answer.declared == 1000);
    release(&answer);
}

static void a_deadline_answers_cut_off_not_refused(unsigned short port) {
    char url[128];
    naula_fetch_sink sink;
    url_of(url, sizeof url, "127.0.0.1", port, "/stall");
    naula_fetch_answer answer = fetch_into_buffer(url, NULL, 300, BODY_LENGTH, &sink);
    CHECK(answer.outcome == NAULA_FETCH_CUT_OFF);
    CHECK(answer.declared == 0 && sink.bytes == 0);
    CHECK(strstr(answer.detail, "did not finish within 300 ms") != NULL);
    CHECK(answer.fix != NULL);
    release(&answer);
}

static void a_body_over_the_sink_is_stopped_by_the_sink(unsigned short port) {
    char url[128];
    naula_fetch_sink sink;
    url_of(url, sizeof url, "127.0.0.1", port, "/file");
    naula_fetch_answer answer = fetch_into_buffer(url, NULL, 5000, 100, &sink);
    CHECK(answer.outcome == NAULA_FETCH_STOPPED_BY_SINK);
    CHECK(sink.exceeded);
    release(&answer);
}

static void a_request_smaller_than_its_first_shape_is_refused(unsigned short port) {
    char url[128];
    naula_fetch_sink sink;
    naula_fetch_answer answer = empty_answer();
    url_of(url, sizeof url, "127.0.0.1", port, "/file");
    naula_fetch_request request;
    memset(&request, 0, sizeof request);
    request.size = sizeof(uint32_t);
    request.url = url;
    request.deadline_milliseconds = 5000;
    naula_fetch_sink_start_memory(&sink, received, BODY_LENGTH);
    naula_fetch_over_network(&request, &sink, &answer);
    CHECK(answer.outcome == NAULA_FETCH_REFUSED);
    CHECK(sink.bytes == 0);
}

static void run_against_the_server(void) {
    char cut_body[CUT_BODY_LENGTH];
    memset(cut_body, 'c', sizeof cut_body);
    const http_route routes[] = {
        {"/file", 200, NULL, body, BODY_LENGTH, HTTP_ROUTE_BODY_LENGTH, ETAG, LAST_MODIFIED, 0, 0},
        {"/cut", 200, NULL, cut_body, CUT_BODY_LENGTH, 1000, NULL, NULL, 0, 0},
        {"/stall", 200, NULL, body, BODY_LENGTH, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 1500, 0},
    };
    http_fixture *server = http_fixture_start(routes, sizeof routes / sizeof routes[0]);
    CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    unsigned short port = http_fixture_port(server);
    a_200_is_received_with_its_validators(port);
    another_status_is_answered_as_status(port);
    a_body_cut_off_answers_its_declared_length(port);
    a_body_over_the_sink_is_stopped_by_the_sink(port);
    a_request_smaller_than_its_first_shape_is_refused(port);
    a_deadline_answers_cut_off_not_refused(port);
    naula_memory_release(http_fixture_stop(server));
}

static char target_location[160];

static void send_a_redirect_and_log(const char *location_host, int expects_authorization_after) {
    char extra[200];
    char url[128];
    naula_fetch_sink sink;
    http_route routes[2] = {
        {"/start", 302, extra, "", 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
        {"/target", 200, NULL, body, BODY_LENGTH, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
    };
    http_fixture *server = http_fixture_start(routes, 2);
    CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    unsigned short port = http_fixture_port(server);
    snprintf(target_location, sizeof target_location, "http://%s:%u/target", location_host, (unsigned)port);
    snprintf(extra, sizeof extra, "Location: %s\r\n", target_location);
    url_of(url, sizeof url, "127.0.0.1", port, "/start");
    naula_fetch_answer answer = fetch_into_buffer(url, SECRET, 5000, BODY_LENGTH, &sink);
    CHECK(answer.outcome == NAULA_FETCH_RECEIVED);
    CHECK(sink.bytes == BODY_LENGTH);
    release(&answer);
    char *log = http_fixture_stop(server);
    CHECK(log != NULL);
    if (log == NULL) {
        return;
    }
    const char *second = strstr(log, "GET /target ");
    CHECK(second != NULL);
    CHECK(occurrences(log, SECRET) == (expects_authorization_after ? 2u : 1u));
    CHECK(second != NULL && (strstr(second, SECRET) != NULL) == (expects_authorization_after != 0));
    char host_line[64];
    snprintf(host_line, sizeof host_line, "Host: %s:", location_host);
    CHECK(second != NULL && strstr(second, host_line) != NULL);
    naula_memory_release(log);
}

static void authorization_is_not_sent_after_a_cross_host_redirect(void) {
    send_a_redirect_and_log("localhost", 0);
}

static void authorization_follows_a_redirect_to_the_same_host(void) {
    send_a_redirect_and_log("127.0.0.1", 1);
}

#if defined(_WIN32)
static void winhttp_takes_the_system_proxy(void) {
    CHECK(naula_fetch_windows_access_type() == WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY);
}
#else
static void clear_proxy_environment(void) {
    const char *names[] = {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy",
                           "ALL_PROXY",  "no_proxy",   "NO_PROXY"};
    for (size_t index = 0; index < sizeof names / sizeof names[0]; index++) {
        unsetenv(names[index]);
    }
}

static void libcurl_honours_https_proxy_and_no_proxy(void) {
    naula_fetch_sink sink;
    http_fixture *proxy = http_fixture_start_proxy();
    CHECK(proxy != NULL);
    char proxy_url[64];
    snprintf(proxy_url, sizeof proxy_url, "http://127.0.0.1:%u", (unsigned)http_fixture_port(proxy));
    setenv("HTTPS_PROXY", proxy_url, 1);
    naula_fetch_answer through = fetch_into_buffer("https://catalog.invalid/index.json", NULL, 5000, BODY_LENGTH, &sink);
    CHECK(through.outcome == NAULA_FETCH_REFUSED && strstr(through.detail, "502") != NULL);
    setenv("NO_PROXY", "catalog.invalid", 1);
    naula_fetch_answer past = fetch_into_buffer("https://catalog.invalid/index.json", NULL, 5000, BODY_LENGTH, &sink);
    CHECK(past.outcome == NAULA_FETCH_REFUSED && strstr(past.detail, "resolve host") != NULL);
    unsetenv("NO_PROXY");
    unsetenv("HTTPS_PROXY");
    char *log = http_fixture_stop(proxy);
    CHECK(log != NULL && occurrences(log, "CONNECT catalog.invalid:443 HTTP/1.1") == 1);
    naula_memory_release(log);
    release(&through);
    release(&past);
}
#endif

#if !defined(_WIN32)
static void a_stopped_transfer_is_settled_by_its_own_flag_whatever_libcurl_reports(void) {
    naula_fetch_sink sink;
    naula_fetch_curl_use_library(NAULA_FAKE_CURL);
    naula_fetch_answer answer = fetch_into_buffer("https://forge.invalid/refused", NULL, 5000, BODY_LENGTH, &sink);
    CHECK(answer.outcome == NAULA_FETCH_STATUS);
    CHECK(answer.status == 401);
    release(&answer);
    answer = fetch_into_buffer("https://forge.invalid/large", NULL, 5000, 1, &sink);
    CHECK(answer.outcome == NAULA_FETCH_STOPPED_BY_SINK);
    naula_fetch_curl_use_library(NULL);
    release(&answer);
}
#endif

#if defined(__linux__)
static void a_linux_without_libcurl_is_refused_naming_the_package(void) {
    naula_fetch_sink sink;
    naula_fetch_curl_use_library("libnaula-no-such-curl.so.4");
    naula_fetch_answer answer = fetch_into_buffer("https://catalog.invalid/index.json", NULL, 5000, BODY_LENGTH, &sink);
    CHECK(answer.outcome == NAULA_FETCH_REFUSED);
    check_text(answer.fix, "install the libcurl package (libcurl4 on Debian and Ubuntu)", "libcurl fix");
    naula_fetch_curl_use_library(NULL);
    release(&answer);
}
#endif

int main(void) {
    naula_json_use_allocator();
#if !defined(_WIN32)
    clear_proxy_environment();
#endif
    prepare_body();
    naula_fetch_allow_plain_http(1);
    run_against_the_server();
    authorization_is_not_sent_after_a_cross_host_redirect();
    authorization_follows_a_redirect_to_the_same_host();
#if defined(_WIN32)
    winhttp_takes_the_system_proxy();
#else
    libcurl_honours_https_proxy_and_no_proxy();
#endif
#if !defined(_WIN32)
    a_stopped_transfer_is_settled_by_its_own_flag_whatever_libcurl_reports();
#endif
#if defined(__linux__)
    a_linux_without_libcurl_is_refused_naming_the_package();
#endif
    return check_summary("transport_test");
}
