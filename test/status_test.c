#include <stdio.h>
#include <string.h>

#include "memory_testing.h"
#include "naula/status.h"

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static void refuse_fills_code_detail_and_fix(void) {
    static const char fix[] = "do the thing";
    naula_refusal refusal;
    memset(&refusal, 0xAB, sizeof refusal);
    naula_refuse(&refusal, "x", fix, "name %s has %d parts", "a", 3);
    CHECK(refusal.size == sizeof refusal);
    CHECK(strcmp(refusal.code, "x") == 0);
    CHECK(refusal.fix == fix);
    CHECK(strcmp(refusal.detail, "name a has 3 parts") == 0);
}

static void refuse_truncates_a_long_detail_with_a_terminator(void) {
    char long_text[2001];
    naula_refusal refusal;
    memset(long_text, 'y', 2000);
    long_text[2000] = '\0';
    memset(&refusal, 0xAB, sizeof refusal);
    naula_refuse(&refusal, "x", NULL, "%s", long_text);
    CHECK(refusal.detail[1023] == 0);
    CHECK(strlen(refusal.detail) == 1023);
    CHECK(refusal.fix == NULL);
}

static void counting_allocator_fails_the_named_attempt(void) {
    void *first;
    void *second;
    naula_memory_fail_at(2);
    first = naula_memory_allocate(8);
    second = naula_memory_allocate(8);
    CHECK(first != NULL);
    CHECK(second == NULL);
    naula_memory_release(first);
    naula_memory_fail_at(0);
}

int main(void) {
    refuse_fills_code_detail_and_fix();
    refuse_truncates_a_long_detail_with_a_terminator();
    counting_allocator_fails_the_named_attempt();
    return failures == 0 ? 0 : 1;
}
