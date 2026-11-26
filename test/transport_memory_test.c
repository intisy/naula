#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "http_fixture.h"
#include "memory_testing.h"
#include "naula/json.h"
#include "naula/memory.h"
#include "naula/transport.h"
#include "transport_hooks.h"

#define BODY "{\"answer\":true}"
#define ETAG "\"validator\""
#define LAST_MODIFIED "Wed, 21 Oct 2015 07:28:00 GMT"
#define ATTEMPT_LIMIT 200u

static const char *const extra_headers[] = {"Accept: application/json", NULL};

static void release(naula_fetch_answer *answer) {
    naula_memory_release(answer->etag);
    naula_memory_release(answer->last_modified);
}

/** One fetch with the attempt-th allocation failing (0 for none); *attempts is how many it made. */
static naula_fetch_answer fetch_failing_at(const char *url, uint64_t attempt, uint64_t *attempts) {
    uint8_t received[256];
    naula_fetch_sink sink;
    naula_fetch_answer answer;
    memset(&answer, 0, sizeof answer);
    answer.size = sizeof answer;
    naula_fetch_sink_start_memory(&sink, received, sizeof received);
    naula_memory_fail_at(attempt);
    naula_fetch_into_memory_with_headers(url, "token not-a-real-token", extra_headers, 5000, &sink, &answer);
    *attempts = naula_memory_attempts();
    naula_memory_fail_at(0);
    return answer;
}

static void every_allocation_a_fetch_makes_answers_out_of_memory_when_it_fails(void) {
    http_route routes[1];
    memset(routes, 0, sizeof routes);
    routes[0].path = "/validated";
    routes[0].status = 200;
    routes[0].body = BODY;
    routes[0].body_length = strlen(BODY);
    routes[0].declared_length = HTTP_ROUTE_BODY_LENGTH;
    routes[0].etag = ETAG;
    routes[0].last_modified = LAST_MODIFIED;
    http_fixture *server = http_fixture_start(routes, 1);
    CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    char url[96];
    snprintf(url, sizeof url, "http://127.0.0.1:%u/validated", (unsigned)http_fixture_port(server));
    uint64_t attempt = 1;
    for (; attempt <= ATTEMPT_LIMIT; attempt++) {
        uint64_t attempts = 0;
        naula_fetch_answer answer = fetch_failing_at(url, attempt, &attempts);
        if (attempts < attempt) {
            CHECK(answer.outcome == NAULA_FETCH_RECEIVED);
            check_text(answer.etag, ETAG, "etag once nothing fails");
            check_text(answer.last_modified, LAST_MODIFIED, "last modified once nothing fails");
            release(&answer);
            break;
        }
        if (answer.outcome != NAULA_FETCH_OUT_OF_MEMORY) {
            fprintf(stderr, "allocation %llu failing answered outcome %d, etag %s, last modified %s\n",
                    (unsigned long long)attempt, (int)answer.outcome, answer.etag == NULL ? "(none)" : answer.etag,
                    answer.last_modified == NULL ? "(none)" : answer.last_modified);
        }
        CHECK(answer.outcome == NAULA_FETCH_OUT_OF_MEMORY);
        CHECK(answer.etag == NULL && answer.last_modified == NULL && answer.detail[0] == '\0');
        release(&answer);
    }
    CHECK(attempt > 1 && attempt <= ATTEMPT_LIMIT);
    naula_memory_release(http_fixture_stop(server));
}

int main(void) {
    naula_json_use_allocator();
    naula_fetch_allow_plain_http(1);
    every_allocation_a_fetch_makes_answers_out_of_memory_when_it_fails();
    return check_summary("transport_memory_test");
}
