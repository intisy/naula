#include <stddef.h>
#include <string.h>

#include "transport_hooks.h"
#include "transport_internal.h"

static int plain_http_allowed = 0;
static unsigned deadline_override = 0;

#ifdef NAULA_TEST_HOOKS
void naula_fetch_allow_plain_http(int allowed) {
    plain_http_allowed = allowed;
}

void naula_fetch_use_deadline(unsigned milliseconds) {
    deadline_override = milliseconds;
}
#endif

int naula_fetch_plain_http_is_allowed(void) {
    return plain_http_allowed;
}

static int starts_with_ignoring_case(const char *url, const char *scheme) {
    for (size_t index = 0; scheme[index] != 0; index++) {
        if (url[index] == 0 || (url[index] | 0x20) != scheme[index]) {
            return 0;
        }
    }
    return 1;
}

static int is_network_url(const char *url) {
    return starts_with_ignoring_case(url, "https:") || (plain_http_allowed && starts_with_ignoring_case(url, "http:"));
}

int naula_fetch_request_is_usable(const naula_fetch_request *request, naula_fetch_answer *answer) {
    size_t first_shape = offsetof(naula_fetch_request, authorization) + sizeof request->authorization;
    if (request->size >= first_shape && request->url != NULL) {
        return 1;
    }
    naula_fetch_refuse(answer, "send a request whose size covers its first shape and which names a url",
                       "the request is smaller than its first shape or names no url");
    return 0;
}

const char *const *naula_fetch_request_extra_headers(const naula_fetch_request *request) {
    size_t covered = offsetof(naula_fetch_request, extra_headers) + sizeof request->extra_headers;
    return request->size >= covered ? request->extra_headers : NULL;
}

void naula_fetch_into_memory(const char *url, const char *authorization, unsigned deadline_milliseconds,
                             naula_fetch_sink *sink, naula_fetch_answer *answer) {
    naula_fetch_into_memory_with_headers(url, authorization, NULL, deadline_milliseconds, sink, answer);
}

void naula_fetch_into_memory_with_headers(const char *url, const char *authorization, const char *const *extra_headers,
                                          unsigned deadline_milliseconds, naula_fetch_sink *sink,
                                          naula_fetch_answer *answer) {
    if (!is_network_url(url)) {
        naula_fetch_refuse(answer, "serve the lookup over https", "%s is not an https url", url);
        return;
    }
    naula_fetch_request request = {sizeof request, url, NULL, NULL,
                                   deadline_override != 0 ? deadline_override : deadline_milliseconds,
                                   authorization, extra_headers};
    naula_fetch_over_network(&request, sink, answer);
}
