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

#define METHOD_LENGTH_LIMIT 16u

static int covers_the_body(const naula_fetch_request *request) {
    return request->size >= offsetof(naula_fetch_request, content_type) + sizeof request->content_type;
}

const char *naula_fetch_request_method(const naula_fetch_request *request) {
    return covers_the_body(request) && request->method != NULL ? request->method : "GET";
}

naula_fetch_body naula_fetch_request_body(const naula_fetch_request *request) {
    naula_fetch_body body = {NULL, 0, NULL};
    if (covers_the_body(request)) {
        body.bytes = request->body;
        body.length = request->body_length;
        body.content_type = request->content_type;
    }
    return body;
}

int naula_fetch_request_follows_redirects(const naula_fetch_request *request) {
    return strcmp(naula_fetch_request_method(request), "GET") == 0;
}

static int is_method_text(const char *method) {
    size_t length = strlen(method);
    if (length == 0 || length > METHOD_LENGTH_LIMIT) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        if (method[index] < 'A' || method[index] > 'Z') {
            return 0;
        }
    }
    return 1;
}

static int is_header_value(const char *text) {
    return text == NULL || strpbrk(text, "\r\n") == NULL;
}

static int is_usable_body(const naula_fetch_request *request) {
    naula_fetch_body body = naula_fetch_request_body(request);
    return is_method_text(naula_fetch_request_method(request)) && (body.bytes != NULL || body.length == 0) &&
           is_header_value(body.content_type);
}

int naula_fetch_request_is_usable(const naula_fetch_request *request, naula_fetch_answer *answer) {
    size_t first_shape = offsetof(naula_fetch_request, authorization) + sizeof request->authorization;
    if (request->size < first_shape || request->url == NULL) {
        naula_fetch_refuse(answer, "send a request whose size covers its first shape and which names a url",
                           "the request is smaller than its first shape or names no url");
        return 0;
    }
    if (!is_usable_body(request)) {
        naula_fetch_refuse(answer, "name the method in capital letters, give the body its bytes, and the content type on one line",
                           "the request for %s has an unusable method, body or content type", request->url);
        return 0;
    }
    return 1;
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
    naula_fetch_request request;
    memset(&request, 0, sizeof request);
    request.size = sizeof request;
    request.url = url;
    request.deadline_milliseconds = deadline_milliseconds;
    request.authorization = authorization;
    request.extra_headers = extra_headers;
    naula_fetch_send_into_memory(&request, sink, answer);
}

void naula_fetch_send_into_memory(const naula_fetch_request *request, naula_fetch_sink *sink,
                                  naula_fetch_answer *answer) {
    if (request->url == NULL || !is_network_url(request->url)) {
        naula_fetch_refuse(answer, "serve the lookup over https", "%s is not an https url",
                           request->url == NULL ? "(no url)" : request->url);
        return;
    }
    naula_fetch_request sent;
    memset(&sent, 0, sizeof sent);
    memcpy(&sent, request, request->size < sizeof sent ? request->size : sizeof sent);
    sent.size = request->size < sizeof sent ? request->size : (uint32_t)sizeof sent;
    if (deadline_override != 0) {
        sent.deadline_milliseconds = deadline_override;
    }
    naula_fetch_over_network(&sent, sink, answer);
}
