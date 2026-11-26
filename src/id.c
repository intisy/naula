#include "naula/id.h"

#include <string.h>

#include "naula/memory.h"

#define HEX_SHORT 40u
#define HEX_LONG 64u
#define FOLDER_DIGITS 12u

static const char GIT_PREFIX[] = "git:";
static const char LOCAL_PREFIX[] = "local:";
static const char *const REFUSAL_FIX =
    "write the id as git:<root commit hex> or local:<name>, with lowercase path segments";

static int is_lowercase_hex(char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
}

static int is_segment_character(char character) {
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '.' ||
           character == '_' || character == '-';
}

static int is_forbidden_in_name(char character) {
    return character == '\0' || character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\v' ||
           character == '\f';
}

static int starts_with(const char *text, size_t length, const char *prefix) {
    size_t prefix_length = strlen(prefix);
    return length >= prefix_length && memcmp(text, prefix, prefix_length) == 0;
}

static naula_status refuse_at(naula_refusal *refusal, size_t offset, const char *reason) {
    naula_refuse(refusal, "invalid_id", REFUSAL_FIX, "offset %zu: %s", offset, reason);
    return NAULA_REFUSED;
}

static size_t hex_run_length(const char *text, size_t length, size_t start) {
    size_t at = start;
    while (at < length && is_lowercase_hex(text[at])) {
        at++;
    }
    return at - start;
}

static int is_object_id_length(size_t run) {
    return run == HEX_SHORT || run == HEX_LONG;
}

/** Digits that still belong to an object id: a run past 40 or 64 digits is refused at the digit that overflows. */
static size_t accepted_digits(size_t run) {
    size_t wanted = run >= HEX_LONG ? HEX_LONG : (run >= HEX_SHORT ? HEX_SHORT : run);
    return wanted;
}

static size_t segment_end(const char *text, size_t length, size_t start) {
    size_t at = start;
    while (at < length && text[at] != '/') {
        at++;
    }
    return at;
}

static int is_dot_segment(const char *text, size_t start, size_t end) {
    size_t width = end - start;
    return (width == 1 && text[start] == '.') || (width == 2 && text[start] == '.' && text[start + 1] == '.');
}

static size_t first_bad_segment_character(const char *text, size_t start, size_t end) {
    size_t at;
    for (at = start; at < end; at++) {
        if (!is_segment_character(text[at])) {
            return at;
        }
    }
    return end;
}

static naula_status check_segments(const char *text, size_t length, size_t start, naula_refusal *refusal) {
    size_t at = start;
    while (at < length) {
        size_t begin = at + 1;
        size_t end = segment_end(text, length, begin);
        size_t bad;
        if (end == begin) {
            return refuse_at(refusal, begin, "an empty path segment");
        }
        if (is_dot_segment(text, begin, end)) {
            return refuse_at(refusal, begin, "a path segment may not be . or ..");
        }
        bad = first_bad_segment_character(text, begin, end);
        if (bad != end) {
            return refuse_at(refusal, bad, "a path segment holds only a-z, 0-9, dot, underscore and hyphen");
        }
        at = end;
    }
    return NAULA_OK;
}

static naula_status check_git(const char *text, size_t length, naula_refusal *refusal, size_t *after_head) {
    size_t start = sizeof GIT_PREFIX - 1;
    size_t run = hex_run_length(text, length, start);
    if (!is_object_id_length(run)) {
        return refuse_at(refusal, start + accepted_digits(run), "the root commit is 40 or 64 lowercase hex digits");
    }
    *after_head = start + run;
    if (*after_head < length && text[*after_head] != '/') {
        return refuse_at(refusal, *after_head, "the root commit is 40 or 64 lowercase hex digits");
    }
    return NAULA_OK;
}

static naula_status check_local(const char *text, size_t length, naula_refusal *refusal, size_t *after_head) {
    size_t start = sizeof LOCAL_PREFIX - 1;
    size_t at = start;
    while (at < length && text[at] != '/' && !is_forbidden_in_name(text[at])) {
        at++;
    }
    if (at == start) {
        return refuse_at(refusal, start, "a local id needs a name");
    }
    if (at < length && text[at] != '/') {
        return refuse_at(refusal, at, "a local name holds no whitespace or NUL");
    }
    *after_head = at;
    return NAULA_OK;
}

static naula_status check_grammar(const char *text, size_t length, naula_refusal *refusal, uint32_t *kind,
                                  size_t *after_head) {
    naula_status status;
    if (starts_with(text, length, GIT_PREFIX)) {
        *kind = NAULA_ID_GIT;
        status = check_git(text, length, refusal, after_head);
    } else if (starts_with(text, length, LOCAL_PREFIX)) {
        *kind = NAULA_ID_LOCAL;
        status = check_local(text, length, refusal, after_head);
    } else {
        return refuse_at(refusal, 0, "an id starts with git: or local:");
    }
    return status != NAULA_OK ? status : check_segments(text, length, *after_head, refusal);
}

static size_t count_segments(const char *text, size_t length, size_t from) {
    size_t count = 0;
    size_t at;
    for (at = from; at < length; at++) {
        count += text[at] == '/';
    }
    return count;
}

static char *copy_range(const char *text, size_t start, size_t end) {
    char *copy = naula_memory_allocate(end - start + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text + start, end - start);
    copy[end - start] = '\0';
    return copy;
}

static int fill_segments(naula_id *id, const char *text, size_t length, size_t from) {
    size_t at = from;
    size_t count = count_segments(text, length, from);
    if (count == 0) {
        return 1;
    }
    id->segments = naula_memory_allocate(count * sizeof *id->segments);
    if (id->segments == NULL) {
        return 0;
    }
    memset(id->segments, 0, count * sizeof *id->segments);
    id->segment_count = count;
    for (count = 0; at < length; count++) {
        size_t end = segment_end(text, length, at + 1);
        id->segments[count] = copy_range(text, at + 1, end);
        if (id->segments[count] == NULL) {
            return 0;
        }
        at = end;
    }
    return 1;
}

static int fill_head(naula_id *id, const char *text, size_t after_head) {
    if (id->kind == NAULA_ID_GIT) {
        memcpy(id->commit, text + sizeof GIT_PREFIX - 1, after_head - (sizeof GIT_PREFIX - 1));
        return 1;
    }
    id->local_name = copy_range(text, sizeof LOCAL_PREFIX - 1, after_head);
    return id->local_name != NULL;
}

naula_status naula_id_parse(const char *text, size_t length, naula_id *id, naula_refusal *refusal) {
    size_t after_head = 0;
    naula_status status;
    if (id == NULL) {
        return NAULA_REFUSED;
    }
    memset(id, 0, sizeof *id);
    id->size = (uint32_t)sizeof *id;
    if (text == NULL) {
        return refuse_at(refusal, 0, "an id starts with git: or local:");
    }
    status = check_grammar(text, length, refusal, &id->kind, &after_head);
    if (status != NAULA_OK) {
        id->kind = 0;
        return status;
    }
    if (!fill_head(id, text, after_head) || !fill_segments(id, text, length, after_head)) {
        naula_id_release(id);
        return NAULA_OUT_OF_MEMORY;
    }
    return NAULA_OK;
}

int naula_id_is_valid(const char *text, size_t length) {
    uint32_t kind = 0;
    size_t after_head = 0;
    return text != NULL && check_grammar(text, length, NULL, &kind, &after_head) == NAULA_OK;
}

void naula_id_release(naula_id *id) {
    size_t at;
    if (id == NULL) {
        return;
    }
    for (at = 0; id->segments != NULL && at < id->segment_count; at++) {
        naula_memory_release(id->segments[at]);
    }
    naula_memory_release(id->segments);
    naula_memory_release(id->local_name);
    memset(id, 0, sizeof *id);
    id->size = (uint32_t)sizeof *id;
}

static int sign_of(int difference) {
    return difference < 0 ? -1 : (difference > 0 ? 1 : 0);
}

static int compare_segments(const naula_id *left, const naula_id *right) {
    size_t at;
    for (at = 0; at < left->segment_count && at < right->segment_count; at++) {
        int difference = strcmp(left->segments[at], right->segments[at]);
        if (difference != 0) {
            return sign_of(difference);
        }
    }
    if (left->segment_count == right->segment_count) {
        return 0;
    }
    return left->segment_count < right->segment_count ? -1 : 1;
}

int naula_id_compare(const naula_id *left, const naula_id *right) {
    int head;
    if (left->kind != right->kind) {
        return left->kind == NAULA_ID_LOCAL ? -1 : 1;
    }
    head = left->kind == NAULA_ID_LOCAL ? strcmp(left->local_name, right->local_name)
                                        : strcmp(left->commit, right->commit);
    return head != 0 ? sign_of(head) : compare_segments(left, right);
}

void naula_id_folder(const naula_id *id, char folder[13]) {
    memset(folder, 0, FOLDER_DIGITS + 1);
    if (id->kind == NAULA_ID_GIT) {
        memcpy(folder, id->commit, FOLDER_DIGITS);
    }
}
