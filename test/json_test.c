#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "cJSON.h"
#include "forge_common.h"
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

#define EXPECT_MALFORMED_LITERAL(literal, label) expect_malformed(literal, sizeof literal - 1, label)

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
    EXPECT_MALFORMED_LITERAL("{} x", "trailing text");
    EXPECT_MALFORMED_LITERAL("{}\x0c", "a form feed after the value");
}

static void text_that_is_not_utf8_is_refused(void) {
    EXPECT_MALFORMED_LITERAL("{ \"a\": \"\xc3\" }", "a cut sequence");
    EXPECT_MALFORMED_LITERAL("{ \"a\": \"\xc0\xaf\" }", "an overlong form");
    EXPECT_MALFORMED_LITERAL("{ \"a\": \"\xed\xa0\x80\" }", "an encoded surrogate");
    EXPECT_MALFORMED_LITERAL("{ \"a\": \"\xf4\x90\x80\x80\" }", "past U+10FFFF");
}

static void a_nul_is_refused_raw_or_escaped(void) {
    EXPECT_MALFORMED_LITERAL("{ \"a\": \"\\u0000\" }", "an escaped NUL");
    EXPECT_MALFORMED_LITERAL("{ \"a\": \"x\0y\" }", "a NUL in a string");
    EXPECT_MALFORMED_LITERAL("{}\0", "a NUL after the value");
}

static void grammar_cjson_tolerates_is_refused(void) {
    EXPECT_MALFORMED_LITERAL("{ \"a\": 01 }", "a leading zero");
    EXPECT_MALFORMED_LITERAL("{ \"a\": 1. }", "a bare decimal point");
    EXPECT_MALFORMED_LITERAL("{ \"a\": \"\t\" }", "a raw tab in a string");
    EXPECT_MALFORMED_LITERAL("\xef\xbb\xbf{}", "a byte order mark");
    EXPECT_MALFORMED_LITERAL("", "nothing");
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

static char *printed_keeping_last_value(const char *text) {
    cJSON *root = NULL;
    const char *reason = NULL;
    naula_status status = naula_json_parse_keeping_last_value((const uint8_t *)text, strlen(text), &root, &reason);
    char *printed = status == NAULA_OK ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return printed;
}

static void check_kept(const char *text, const char *expected) {
    char *printed = printed_keeping_last_value(text);
    check_text(printed, expected, text);
    cJSON_free(printed);
}

static void a_duplicated_key_keeps_its_first_position_and_its_last_value(void) {
    check_kept("{\"id\":\"a\",\"id\":\"b\"}", "{\"id\":\"b\"}");
    check_kept("{\"x\":1,\"y\":2,\"x\":3}", "{\"x\":3,\"y\":2}");
    check_kept("{\"x\":1,\"y\":2,\"x\":3,\"x\":4}", "{\"x\":4,\"y\":2}");
    check_kept("[{\"o\":{\"k\":1,\"k\":{\"m\":1,\"m\":2}}}]", "[{\"o\":{\"k\":{\"m\":2}}}]");
    check_kept("{\"K\":1,\"k\":2}", "{\"K\":1,\"k\":2}");
    check_kept("[\"x\",\"x\"]", "[\"x\",\"x\"]");
}

static void the_plain_parse_keeps_every_duplicated_member(void) {
    cJSON *root = NULL;
    const char *reason = NULL;
    CHECK(parse_text("{\"id\":\"a\",\"id\":\"b\"}", &root, &reason) == NAULA_OK);
    CHECK(cJSON_GetArraySize(root) == 2);
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "id")), "a", "first member found");
    cJSON_Delete(root);
}

static void keeping_the_last_value_refuses_what_the_plain_parse_refuses(void) {
    static const char *const refused[] = {"", "{} x", "{ \"a\": 01 }", "\"a\\u0000b\"", "\xef\xbb\xbf{}",
                                          "{ \"a\": \"\xc0\xaf\" }"};
    for (size_t index = 0; index < sizeof refused / sizeof refused[0]; index++) {
        cJSON *root = NULL;
        const char *reason = NULL;
        naula_status status = naula_json_parse_keeping_last_value((const uint8_t *)refused[index],
                                                                  strlen(refused[index]), &root, &reason);
        if (status != NAULA_MALFORMED || root != NULL || reason == NULL) {
            fprintf(stderr, "keeping the last value answered %d for %s\n", (int)status, refused[index]);
            check_failures++;
        }
        cJSON_Delete(root);
    }
    char *deeper = nested_arrays(CJSON_NESTING_LIMIT + 1);
    cJSON *root = NULL;
    const char *reason = NULL;
    CHECK(naula_json_parse_keeping_last_value((const uint8_t *)deeper, strlen(deeper), &root, &reason) ==
          NAULA_MALFORMED);
    CHECK(root == NULL);
    free(deeper);
}

static void keeping_the_last_value_without_memory_is_out_of_memory(void) {
    const char *text = "{ \"k\": 1, \"o\": { \"m\": [1], \"m\": [2, 3] }, \"k\": { \"z\": true } }";
    cJSON *root = NULL;
    const char *reason = NULL;
    naula_memory_fail_at(0);
    CHECK(naula_json_parse_keeping_last_value((const uint8_t *)text, strlen(text), &root, &reason) == NAULA_OK);
    uint64_t attempts = naula_memory_attempts();
    cJSON_Delete(root);
    for (uint64_t attempt = 1; attempt <= attempts; attempt++) {
        naula_memory_fail_at(attempt);
        naula_status status = naula_json_parse_keeping_last_value((const uint8_t *)text, strlen(text), &root, &reason);
        naula_memory_fail_at(0);
        if (status != NAULA_OUT_OF_MEMORY || root != NULL) {
            fprintf(stderr, "keeping: failing allocation %llu answered %d\n", (unsigned long long)attempt, (int)status);
            check_failures++;
            cJSON_Delete(root);
        }
    }
}

static const char *const refused_by_the_grammar[] = {
    "\xEF\xBB\xBF{}", "\v{}", "{\f}", "{\x01}", "[\xC2\xA0" "1]", "\"a\tb\"", "\"a\x1f" "b\"",
    "01", "-01", "1.", "-", "1e", "1e+", ".5", "+1", "[1,]", "{\"a\":1,}", "\"\\'\"",
    "\"\\u12\"", "\"\\x41\"", "tru", "nul", "[1 2]", "{\"a\" 1}", "{a:1}", "\"open", "", "{", "1 2", "{\"a\":}",
};

static const char *const accepted_by_the_grammar[] = {
    " \t\r\n{} \t\r\n", "-0", "0.25", "1.5E+3", "1e-2", "-12.5e10", "\"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u00e9\"",
    "\"a\xE2\x80\xA8z\"", "[true,false,null,[],{}]", "{\"\":0}",
};

static void the_grammar_is_json_parse_s(void) {
    for (size_t index = 0; index < sizeof refused_by_the_grammar / sizeof refused_by_the_grammar[0]; index++) {
        const char *text = refused_by_the_grammar[index];
        if (naula_json_is_grammar(text, strlen(text))) {
            fprintf(stderr, "the grammar accepted %s\n", text);
            check_failures++;
        }
    }
    for (size_t index = 0; index < sizeof accepted_by_the_grammar / sizeof accepted_by_the_grammar[0]; index++) {
        const char *text = accepted_by_the_grammar[index];
        if (!naula_json_is_grammar(text, strlen(text))) {
            fprintf(stderr, "the grammar refused %s\n", text);
            check_failures++;
        }
    }
    CHECK(naula_json_is_grammar("\"a\"!!", 3));
}

static void utf8_is_checked_on_its_own(void) {
    static const uint8_t accepted[] = {'c', 'a', 'f', 0xC3, 0xA9, 0xE6, 0x97, 0xA5, 0xF0, 0x9F, 0x98, 0x80};
    static const uint8_t stray_continuation[] = {0x80};
    static const uint8_t lone_ff[] = {0xFF};
    static const uint8_t overlong[] = {0xC0, 0xAF};
    static const uint8_t surrogate[] = {0xED, 0xA0, 0x80};
    static const uint8_t past_unicode[] = {0xF4, 0x90, 0x80, 0x80};
    static const uint8_t truncated[] = {0xE6, 0x97};
    CHECK(naula_json_is_valid_utf8(accepted, sizeof accepted));
    CHECK(naula_json_is_valid_utf8(accepted, 0));
    CHECK(!naula_json_is_valid_utf8(stray_continuation, sizeof stray_continuation));
    CHECK(!naula_json_is_valid_utf8(lone_ff, sizeof lone_ff));
    CHECK(!naula_json_is_valid_utf8(overlong, sizeof overlong));
    CHECK(!naula_json_is_valid_utf8(surrogate, sizeof surrogate));
    CHECK(!naula_json_is_valid_utf8(past_unicode, sizeof past_unicode));
    CHECK(!naula_json_is_valid_utf8(truncated, sizeof truncated));
}

static naula_status parse_reply(const char *text, cJSON **json) {
    static const forge_flavor flavor = {"the test forge", "name a host", NULL};
    forge_reply reply = {(uint8_t *)text, strlen(text), NULL, 0};
    naula_refusal refusal;
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    naula_status status = NAULA_OK;
    *json = naula_forge_parse_reply(&flavor, &reply, &status, &refusal);
    return status;
}

static void a_provider_reply_without_memory_is_out_of_memory(void) {
    char text[] = "{ \"id\": 1222336612, \"name\": \"naula\", \"owner\": { \"login\": \"intisy\" } }";
    cJSON *json = NULL;
    naula_memory_fail_at(0);
    CHECK(parse_reply(text, &json) == NAULA_OK && json != NULL);
    uint64_t attempts = naula_memory_attempts();
    cJSON_Delete(json);
    for (uint64_t attempt = 1; attempt <= attempts; attempt++) {
        naula_memory_fail_at(attempt);
        naula_status status = parse_reply(text, &json);
        naula_memory_fail_at(0);
        if (status != NAULA_OUT_OF_MEMORY || json != NULL) {
            fprintf(stderr, "reply: failing allocation %llu answered %d\n", (unsigned long long)attempt, (int)status);
            check_failures++;
            cJSON_Delete(json);
        }
    }
    char trailing[] = "{ \"id\": 1 } x";
    CHECK(parse_reply(trailing, &json) == NAULA_MALFORMED && json == NULL);
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
    a_provider_reply_without_memory_is_out_of_memory();
    a_duplicated_key_keeps_its_first_position_and_its_last_value();
    the_plain_parse_keeps_every_duplicated_member();
    keeping_the_last_value_refuses_what_the_plain_parse_refuses();
    keeping_the_last_value_without_memory_is_out_of_memory();
    the_grammar_is_json_parse_s();
    utf8_is_checked_on_its_own();
    return check_summary("json_test");
}
