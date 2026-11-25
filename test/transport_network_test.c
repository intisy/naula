#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "naula/memory.h"
#include "naula/transport.h"

#define PINNED_URL "https://raw.githubusercontent.com/DaveGamble/cJSON/acc76239bee01d8e9c858ae2cab296704e52d916/LICENSE"
#define PINNED_SHA256 "a36dda207c36db5818729c54e7ad4e8b0c6fba847491ba64f372c1a2037b6d5c"
#define PINNED_BYTES 1084

static void a_pinned_https_file_is_fetched_over_the_os_stack(void) {
    uint8_t buffer[2048];
    naula_fetch_sink sink;
    naula_fetch_answer answer;
    char hex[NAULA_SHA256_HEX_SIZE];
    memset(&answer, 0, sizeof answer);
    answer.size = sizeof answer;
    naula_fetch_sink_start_memory(&sink, buffer, sizeof buffer);
    naula_fetch_into_memory(PINNED_URL, NULL, 75000, &sink, &answer);
    if (answer.outcome != NAULA_FETCH_RECEIVED) {
        fprintf(stderr, "not received: outcome %d status %ld %s\n", (int)answer.outcome, answer.status, answer.detail);
    }
    CHECK(answer.outcome == NAULA_FETCH_RECEIVED);
    CHECK(sink.bytes == PINNED_BYTES);
    naula_sha256_hex(&sink.hash, hex);
    check_text(hex, PINNED_SHA256, "sha256");
    naula_memory_release(answer.etag);
    naula_memory_release(answer.last_modified);
}

int main(void) {
    a_pinned_https_file_is_fetched_over_the_os_stack();
    return check_summary("transport_network_test");
}
