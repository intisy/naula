#ifndef NAULA_TRANSPORT_INTERNAL_H
#define NAULA_TRANSPORT_INTERNAL_H

#include "naula/transport.h"

/** Whether request is read as far as its first shape; a request that is not is answered REFUSED. */
int naula_fetch_request_is_usable(const naula_fetch_request *request, naula_fetch_answer *answer);

/** The request's extra header lines, NULL when it carries none or its size does not cover the member. */
const char *const *naula_fetch_request_extra_headers(const naula_fetch_request *request);

/** The body a request sends: no bytes and no content type for a request whose size does not cover them. */
typedef struct naula_fetch_body {
    const uint8_t *bytes;
    size_t length;
    const char *content_type;
} naula_fetch_body;

/** The request's method, "GET" when it names none or its size does not cover the member. */
const char *naula_fetch_request_method(const naula_fetch_request *request);

naula_fetch_body naula_fetch_request_body(const naula_fetch_request *request);

/** Only a GET is redirected; any other method answers its 3xx as STATUS. */
int naula_fetch_request_follows_redirects(const naula_fetch_request *request);

#endif
