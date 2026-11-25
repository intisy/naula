#ifndef NAULA_TRANSPORT_H
#define NAULA_TRANSPORT_H

/**
 * @file
 * naula's HTTP transport: one GET over the operating system's stack (WinHTTP, or libcurl loaded at run time), into a
 * sink that hashes and bounds what it takes. naula_fetch_request is read only up to its `size`; the caller sets
 * naula_fetch_answer's `size` and the transport fills its first shape.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "naula/sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NAULA_FETCH_NETWORK_FIX "check the network, then try again"
#define NAULA_FETCH_PUBLISHED_FIX "check that the catalog still publishes this file"
#define NAULA_FETCH_DETAIL_SIZE 1024

/** Where fetched bytes go: the partial file or a memory buffer, the running hash and the byte count, bounded by limit when limited. */
typedef struct naula_fetch_sink {
    FILE *file;
    uint8_t *memory;
    naula_sha256 hash;
    uint64_t bytes;
    uint64_t limit;
    int limited;
    int exceeded;
    int unwritable;
} naula_fetch_sink;

void naula_fetch_sink_start(naula_fetch_sink *sink, FILE *file, int limited, uint64_t limit);

/** A sink that keeps at most capacity bytes in buffer instead of a file; more than that marks it exceeded. */
void naula_fetch_sink_start_memory(naula_fetch_sink *sink, uint8_t *buffer, size_t capacity);

/** Writes and hashes the bytes. 0, and nothing more taken, once they would pass the limit or cannot be written. */
int naula_fetch_sink_take(naula_fetch_sink *sink, const uint8_t *bytes, size_t length);

/** Whether a body of the declared length fits the limit; one that does not marks the sink exceeded. */
int naula_fetch_sink_admits(naula_fetch_sink *sink, uint64_t declared);

/** First shape ends at `authorization`. */
typedef struct naula_fetch_request {
    uint32_t size;
    const char *url;
    const char *etag;
    const char *last_modified;
    unsigned deadline_milliseconds;
    const char *authorization;
} naula_fetch_request;

typedef enum naula_fetch_outcome {
    NAULA_FETCH_RECEIVED,
    NAULA_FETCH_NOT_MODIFIED,
    NAULA_FETCH_STATUS,
    NAULA_FETCH_CUT_OFF,
    NAULA_FETCH_STOPPED_BY_SINK,
    NAULA_FETCH_REFUSED,
    NAULA_FETCH_OUT_OF_MEMORY
} naula_fetch_outcome;

/**
 * What a transport found, its `size` set by the caller. STATUS carries the status; CUT_OFF the declared length, or 0
 * with a `detail` and `fix` when the deadline ended the transfer, which says nothing about whether the host is gone;
 * REFUSED its own detail and fix; RECEIVED and NOT_MODIFIED may carry the validators the answer sent, released with
 * naula_memory_release. First shape ends at `fix`.
 */
typedef struct naula_fetch_answer {
    uint32_t size;
    naula_fetch_outcome outcome;
    long status;
    uint64_t declared;
    char *etag;
    char *last_modified;
    char detail[NAULA_FETCH_DETAIL_SIZE];
    const char *fix;
} naula_fetch_answer;

void naula_fetch_refuse(naula_fetch_answer *answer, const char *fix, const char *format, ...);

/** Answers that memory ran out before the transport could answer. */
void naula_fetch_out_of_memory(naula_fetch_answer *answer);

/** Answers CUT_OFF for a transfer that did not finish within request->deadline_milliseconds. */
void naula_fetch_deadline_passed(naula_fetch_answer *answer, const naula_fetch_request *request);

/**
 * Fetches url into a memory sink under https only and the deadline override, so a lookup that needs only a short
 * answer shares its transport and refusals. authorization, when not NULL, is the whole value of an Authorization
 * header on one line.
 *
 * @implNote The header goes to the host and port url names and to no other: a redirect to another host is followed
 * without it. WinHTTP follows redirects by hand here, so the check is ours; libcurl drops a custom Authorization
 * header when a redirect changes host (7.58 and later, CURLOPT_UNRESTRICTED_AUTH left at 0).
 */
void naula_fetch_into_memory(const char *url, const char *authorization, unsigned deadline_milliseconds,
                             naula_fetch_sink *sink, naula_fetch_answer *answer);

/** Whether the test hook has allowed plain http; always 0 in the shipped library. */
int naula_fetch_plain_http_is_allowed(void);

/** Fetches an https url, or an http one when the request allows it, over the operating system's stack. */
void naula_fetch_over_network(const naula_fetch_request *request, naula_fetch_sink *sink, naula_fetch_answer *answer);

#ifdef __cplusplus
}
#endif

#endif
