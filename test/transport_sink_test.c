#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "naula/transport.h"
#include "naula/json.h"

#ifndef NAULA_TEST_SCRATCH
#error "NAULA_TEST_SCRATCH names the folder a test may write into"
#endif

static void sink_refuses_past_its_limit(void) {
    FILE *file = fopen(NAULA_TEST_SCRATCH "/sink-limit.bin", "wb");
    naula_fetch_sink sink;
    CHECK(file != NULL);
    if (file == NULL) {
        return;
    }
    naula_fetch_sink_start(&sink, file, 1, 5);
    CHECK(naula_fetch_sink_take(&sink, (const uint8_t *)"abc", 3) == 1);
    CHECK(naula_fetch_sink_take(&sink, (const uint8_t *)"def", 3) == 0);
    CHECK(sink.exceeded == 1 && sink.bytes == 3);
    CHECK(naula_fetch_sink_take(&sink, (const uint8_t *)"d", 1) == 0);
    CHECK(sink.bytes == 3);
    fclose(file);
}

static void an_unlimited_sink_takes_everything(void) {
    FILE *file = fopen(NAULA_TEST_SCRATCH "/sink-unlimited.bin", "wb");
    naula_fetch_sink sink;
    CHECK(file != NULL);
    if (file == NULL) {
        return;
    }
    naula_fetch_sink_start(&sink, file, 0, 0);
    CHECK(naula_fetch_sink_take(&sink, (const uint8_t *)"abcdef", 6) == 1);
    CHECK(sink.exceeded == 0 && sink.bytes == 6);
    fclose(file);
}

static void memory_sink_marks_exceeded(void) {
    uint8_t buffer[4];
    naula_fetch_sink sink;
    naula_fetch_sink_start_memory(&sink, buffer, sizeof buffer);
    CHECK(naula_fetch_sink_admits(&sink, 4) == 1);
    CHECK(naula_fetch_sink_take(&sink, (const uint8_t *)"abcd", 4) == 1);
    CHECK(memcmp(buffer, "abcd", 4) == 0);
    CHECK(naula_fetch_sink_take(&sink, (const uint8_t *)"e", 1) == 0);
    CHECK(sink.exceeded == 1);
    naula_fetch_sink_start_memory(&sink, buffer, sizeof buffer);
    CHECK(naula_fetch_sink_admits(&sink, 5) == 0);
    CHECK(sink.exceeded == 1);
}

int main(void) {
    naula_json_use_allocator();
    sink_refuses_past_its_limit();
    an_unlimited_sink_takes_everything();
    memory_sink_marks_exceeded();
    return check_summary("transport_sink_test");
}
