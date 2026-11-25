#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "http_fixture.h"
#include "naula/memory.h"
#include "naula/transport.h"

static naula_fetch_answer empty_answer(void) {
    naula_fetch_answer answer;
    memset(&answer, 0, sizeof answer);
    answer.size = sizeof answer;
    return answer;
}

static void plain_http_is_refused_without_the_hook(void) {
    const http_route routes[] = {{"/file", 200, NULL, "hello", 5, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0}};
    http_fixture *server = http_fixture_start(routes, 1);
    CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    char url[128];
    uint8_t buffer[16];
    naula_fetch_sink sink;
    naula_fetch_answer answer = empty_answer();
    snprintf(url, sizeof url, "http://127.0.0.1:%u/file", (unsigned)http_fixture_port(server));
    naula_fetch_sink_start_memory(&sink, buffer, sizeof buffer);
    CHECK(naula_fetch_plain_http_is_allowed() == 0);
    naula_fetch_into_memory(url, NULL, 5000, &sink, &answer);
    CHECK(answer.outcome == NAULA_FETCH_REFUSED);
    check_text(answer.fix, "serve the lookup over https", "fix");
    CHECK(sink.bytes == 0);

    answer = empty_answer();
    naula_fetch_request request = {sizeof request, url, NULL, NULL, 5000, NULL, NULL};
    naula_fetch_over_network(&request, &sink, &answer);
    CHECK(answer.outcome == NAULA_FETCH_REFUSED);
    CHECK(sink.bytes == 0);

    answer = empty_answer();
    request.url = "ftp://example.invalid/file";
    naula_fetch_over_network(&request, &sink, &answer);
    CHECK(answer.outcome == NAULA_FETCH_REFUSED);

    char *log = http_fixture_stop(server);
    CHECK(log != NULL && log[0] == '\0');
    naula_memory_release(log);
}

int main(void) {
    plain_http_is_refused_without_the_hook();
    return check_summary("transport_shipped_test");
}
