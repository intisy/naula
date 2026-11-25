#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "cJSON.h"
#include "json_document.h"
#include "memory_testing.h"
#include "naula/json.h"

static naula_status parse_bytes(const char *bytes, size_t length, cJSON **root, const char **reason) {
    *root = NULL;
    *reason = NULL;
    return naula_json_parse((const uint8_t *)bytes, length, root, reason);
}

static naula_status parse_text(const char *text, cJSON **root, const char **reason) {
    return parse_bytes(text, strlen(text), root, reason);
}

static void expect_malformed(const char *bytes, size_t length, const char *label) {
    cJSON *root = NULL;
    const char *reason = NULL;
    naula_status status = parse_bytes(bytes, length, &root, &reason);
    if (status != NAULA_MALFORMED || root != NULL || reason == NULL) {
        fprintf(stderr, "%s: expected malformed, got status %d\n", label, (int)status);
        check_failures++;
    }
    cJSON_Delete(root);
}

static void json_parses(void) {
    cJSON *root = NULL;
    const char *reason = NULL;
    CHECK(parse_text(" { \"a\": [ 1, -2.5e3, true, null, \"\\u00e9\\n\" ] }\r\n", &root, &reason) == NAULA_OK);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root, "a")) == 5);
    cJSON_Delete(root);
}

static void trailing_data_is_refused(void) {
    const char *text = "{ \"a\": 1 } { \"b\": 2 }";
    expect_malformed(text, strlen(text), "a second value");
    expect_malformed("{} x", 4, "trailing text");
    expect_malformed("{}\x0c", 3, "a form feed after the value");
}

static void text_that_is_not_utf8_is_refused(void) {
    expect_malformed("{ \"a\": \"\xc3\" }", 10, "a cut sequence");
    expect_malformed("{ \"a\": \"\xc0\xaf\" }", 11, "an overlong form");
    expect_malformed("{ \"a\": \"\xed\xa0\x80\" }", 12, "an encoded surrogate");
    expect_malformed("{ \"a\": \"\xf4\x90\x80\x80\" }", 13, "past U+10FFFF");
}

static void a_nul_is_refused_raw_or_escaped(void) {
    expect_malformed("{ \"a\": \"\\u0000\" }", 17, "an escaped NUL");
    expect_malformed("{ \"a\": \"x\0y\" }", 13, "a NUL in a string");
    expect_malformed("{}\0", 3, "a NUL after the value");
}

static void grammar_cjson_tolerates_is_refused(void) {
    expect_malformed("{ \"a\": 01 }", 11, "a leading zero");
    expect_malformed("{ \"a\": 1. }", 11, "a bare decimal point");
    expect_malformed("{ \"a\": \"\t\" }", 11, "a raw tab in a string");
    expect_malformed("\xef\xbb\xbf{}", 5, "a byte order mark");
    expect_malformed("", 0, "nothing");
}

static void an_unpaired_surrogate_reads_as_the_replacement_character(void) {
    cJSON *root = NULL;
    const char *reason = NULL;
    CHECK(parse_text("[ \"\\ud800\", \"\\udc00x\", \"\\ud83d\\ude00\" ]", &root, &reason) == NAULA_OK);
    if (root != NULL) {
        check_text(cJSON_GetArrayItem(root, 0)->valuestring, "\xef\xbf\xbd", "a lone high surrogate");
        check_text(cJSON_GetArrayItem(root, 1)->valuestring, "\xef\xbf\xbdx", "a lone low surrogate");
        check_text(cJSON_GetArrayItem(root, 2)->valuestring, "\xf0\x9f\x98\x80", "a surrogate pair");
    }
    cJSON_Delete(root);
}

static char *nested_arrays(size_t depth) {
    char *text = malloc(2 * depth + 1);
    memset(text, '[', depth);
    memset(text + depth, ']', depth);
    text[2 * depth] = '\0';
    return text;
}

static void nesting_past_cjson_is_malformed_not_out_of_memory(void) {
    char *deepest = nested_arrays(CJSON_NESTING_LIMIT);
    cJSON *root = NULL;
    const char *reason = NULL;
    CHECK(parse_text(deepest, &root, &reason) == NAULA_OK);
    cJSON_Delete(root);
    free(deepest);
    char *deeper = nested_arrays(CJSON_NESTING_LIMIT + 1);
    expect_malformed(deeper, strlen(deeper), "one level past cJSON's limit");
    free(deeper);
}

static void a_failed_allocation_is_out_of_memory_never_malformed(void) {
    const char *text = "{ \"name\": \"naula\", \"topics\": [ \"tools\", \"git\" ], \"nested\": { \"a\": [ 1, 2 ] } }";
    cJSON *root = NULL;
    const char *reason = NULL;
    naula_memory_fail_at(0);
    CHECK(parse_text(text, &root, &reason) == NAULA_OK);
    uint64_t attempts = naula_memory_attempts();
    cJSON_Delete(root);
    CHECK(attempts > 3);
    for (uint64_t attempt = 1; attempt <= attempts; attempt++) {
        naula_memory_fail_at(attempt);
        naula_status status = parse_text(text, &root, &reason);
        naula_memory_fail_at(0);
        if (status != NAULA_OUT_OF_MEMORY || root != NULL) {
            fprintf(stderr, "failing allocation %llu answered %d\n", (unsigned long long)attempt, (int)status);
            check_failures++;
            cJSON_Delete(root);
        }
    }
}

int main(void) {
    naula_json_use_allocator();
    json_parses();
    trailing_data_is_refused();
    text_that_is_not_utf8_is_refused();
    a_nul_is_refused_raw_or_escaped();
    grammar_cjson_tolerates_is_refused();
    an_unpaired_surrogate_reads_as_the_replacement_character();
    nesting_past_cjson_is_malformed_not_out_of_memory();
    a_failed_allocation_is_out_of_memory_never_malformed();
    return check_summary("json_test");
}
