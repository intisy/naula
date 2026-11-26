#include "schema_pattern.h"

#include <stdlib.h>
#include <string.h>

#define UNBOUNDED (-1)
#define BYTE_SET_SIZE 32

typedef enum node_kind { NODE_BYTES, NODE_GROUP, NODE_AHEAD, NODE_NOT_AHEAD, NODE_START, NODE_END } node_kind;

typedef struct sequence sequence;

typedef struct choice {
    sequence *sequences;
    size_t count;
} choice;

typedef struct node {
    node_kind kind;
    unsigned char bytes[BYTE_SET_SIZE];
    choice choice;
    int least;
    int most;
} node;

struct sequence {
    node *nodes;
    size_t count;
};

struct schema_pattern {
    choice top;
};

typedef struct reader {
    const char *text;
    size_t at;
    int failed;
} reader;

typedef struct subject {
    const unsigned char *bytes;
    size_t length;
} subject;

/** What follows a match: the rest of a sequence, or one more pass of a repeated group. */
typedef struct continuation {
    const sequence *sequence;
    size_t index;
    const node *repeat;
    int done;
    size_t start;
    const struct continuation *next;
} continuation;

static void parse_choice(reader *in, choice *out);

static char peek(const reader *in) {
    return in->text[in->at];
}

static int accept(reader *in, char character) {
    if (peek(in) == character) {
        in->at++;
        return 1;
    }
    return 0;
}

static void add_byte(unsigned char *set, unsigned value) {
    set[value / 8] |= (unsigned char)(1u << (value % 8));
}

static void add_range(unsigned char *set, unsigned low, unsigned high) {
    for (unsigned value = low; value <= high; value++) {
        add_byte(set, value);
    }
}

static int has_byte(const unsigned char *set, unsigned char value) {
    return (set[value / 8] >> (value % 8)) & 1u;
}

static void add_space(unsigned char *set) {
    add_range(set, '\t', '\r');
    add_byte(set, ' ');
}

static void add_set(unsigned char *set, const unsigned char *other, int negated) {
    for (size_t index = 0; index < BYTE_SET_SIZE; index++) {
        set[index] |= negated ? (unsigned char)~other[index] : other[index];
    }
}

static int hex_value(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return character >= 'A' && character <= 'F' ? character - 'A' + 10 : -1;
}

static int control_escape(char character) {
    static const char letters[] = "tnrvf";
    static const char values[] = "\t\n\r\v\f";
    const char *found = character == '\0' ? NULL : strchr(letters, character);
    return found == NULL ? -1 : (unsigned char)values[found - letters];
}

/** Reads the escape after a backslash into `set`; answers the single byte it names, or -1 for a set such as \s. */
static int read_escape(reader *in, unsigned char *set) {
    char escaped = peek(in);
    unsigned char class_set[BYTE_SET_SIZE] = {0};
    if (escaped == '\0') {
        in->failed = 1;
        return -1;
    }
    in->at++;
    if (escaped == 'd') {
        add_range(set, '0', '9');
        return -1;
    }
    if (escaped == 's' || escaped == 'S') {
        add_space(class_set);
        add_set(set, class_set, escaped == 'S');
        return -1;
    }
    if (escaped == 'x') {
        int high = hex_value(peek(in));
        int low = high < 0 ? -1 : hex_value(in->text[in->at + 1]);
        if (low < 0) {
            in->failed = 1;
            return -1;
        }
        in->at += 2;
        add_byte(set, (unsigned)(high * 16 + low));
        return high * 16 + low;
    }
    if (control_escape(escaped) >= 0) {
        add_byte(set, (unsigned)control_escape(escaped));
        return control_escape(escaped);
    }
    if (strchr("\\/.-^$()[]{}|*+?", escaped) == NULL) {
        in->failed = 1;
        return -1;
    }
    add_byte(set, (unsigned char)escaped);
    return (unsigned char)escaped;
}

static int read_class_member(reader *in, unsigned char *set) {
    char character = peek(in);
    if (character == '\0') {
        in->failed = 1;
        return -1;
    }
    in->at++;
    if (character == '\\') {
        return read_escape(in, set);
    }
    add_byte(set, (unsigned char)character);
    return (unsigned char)character;
}

static void parse_class(reader *in, node *out) {
    unsigned char members[BYTE_SET_SIZE] = {0};
    int negated = accept(in, '^');
    while (!in->failed && peek(in) != ']') {
        int low = read_class_member(in, members);
        if (low >= 0 && peek(in) == '-' && in->text[in->at + 1] != ']' && in->text[in->at + 1] != '\0') {
            unsigned char ignored[BYTE_SET_SIZE] = {0};
            in->at++;
            int high = read_class_member(in, ignored);
            if (high < low) {
                in->failed = 1;
                return;
            }
            add_range(members, (unsigned)low, (unsigned)high);
        }
    }
    if (!accept(in, ']')) {
        in->failed = 1;
        return;
    }
    add_set(out->bytes, members, negated);
}

static int read_count(reader *in, int *value) {
    int digits = 0;
    *value = 0;
    while (peek(in) >= '0' && peek(in) <= '9') {
        *value = *value * 10 + (peek(in) - '0');
        in->at++;
        digits++;
    }
    return digits > 0;
}

static void parse_braces(reader *in, node *out) {
    if (!read_count(in, &out->least)) {
        in->failed = 1;
        return;
    }
    out->most = out->least;
    if (accept(in, ',')) {
        out->most = read_count(in, &out->most) ? out->most : UNBOUNDED;
    }
    if (!accept(in, '}') || (out->most != UNBOUNDED && out->most < out->least)) {
        in->failed = 1;
    }
}

static void parse_quantifier(reader *in, node *out) {
    char character = peek(in);
    if (character != '*' && character != '+' && character != '?' && character != '{') {
        return;
    }
    if (out->kind != NODE_BYTES && out->kind != NODE_GROUP) {
        in->failed = 1;
        return;
    }
    in->at++;
    if (character == '{') {
        parse_braces(in, out);
    } else {
        out->least = character == '+' ? 1 : 0;
        out->most = character == '?' ? 1 : UNBOUNDED;
    }
    if (peek(in) == '?' || peek(in) == '*' || peek(in) == '+' || peek(in) == '{') {
        in->failed = 1;
    }
}

static void parse_group(reader *in, node *out) {
    out->kind = NODE_GROUP;
    if (accept(in, '?')) {
        if (accept(in, '=')) {
            out->kind = NODE_AHEAD;
        } else if (accept(in, '!')) {
            out->kind = NODE_NOT_AHEAD;
        } else if (!accept(in, ':')) {
            in->failed = 1;
            return;
        }
    }
    parse_choice(in, &out->choice);
    if (!accept(in, ')')) {
        in->failed = 1;
    }
}

static void parse_atom(reader *in, node *out) {
    char character = peek(in);
    memset(out, 0, sizeof *out);
    out->kind = NODE_BYTES;
    out->least = 1;
    out->most = 1;
    in->at++;
    if (character == '^') {
        out->kind = NODE_START;
    } else if (character == '$') {
        out->kind = NODE_END;
    } else if (character == '(') {
        parse_group(in, out);
    } else if (character == '[') {
        parse_class(in, out);
    } else if (character == '\\') {
        read_escape(in, out->bytes);
    } else if (strchr(".*+?{}]", character) != NULL) {
        in->failed = 1;
    } else {
        add_byte(out->bytes, (unsigned char)character);
    }
}

static void parse_sequence(reader *in, sequence *out) {
    size_t capacity = 0;
    out->nodes = NULL;
    out->count = 0;
    while (!in->failed && peek(in) != '\0' && peek(in) != '|' && peek(in) != ')') {
        if (out->count == capacity) {
            capacity = capacity == 0 ? 8 : capacity * 2;
            node *grown = realloc(out->nodes, capacity * sizeof *grown);
            if (grown == NULL) {
                in->failed = 1;
                return;
            }
            out->nodes = grown;
        }
        parse_atom(in, &out->nodes[out->count]);
        out->count++;
        if (!in->failed) {
            parse_quantifier(in, &out->nodes[out->count - 1]);
        }
    }
}

static void parse_choice(reader *in, choice *out) {
    size_t capacity = 0;
    out->sequences = NULL;
    out->count = 0;
    do {
        if (out->count == capacity) {
            capacity = capacity == 0 ? 2 : capacity * 2;
            sequence *grown = realloc(out->sequences, capacity * sizeof *grown);
            if (grown == NULL) {
                in->failed = 1;
                return;
            }
            out->sequences = grown;
        }
        parse_sequence(in, &out->sequences[out->count]);
        out->count++;
    } while (!in->failed && accept(in, '|'));
}

static void release_choice(choice *alternatives) {
    for (size_t index = 0; index < alternatives->count; index++) {
        sequence *each = &alternatives->sequences[index];
        for (size_t at = 0; at < each->count; at++) {
            release_choice(&each->nodes[at].choice);
        }
        free(each->nodes);
    }
    free(alternatives->sequences);
}

schema_pattern *schema_pattern_compile(const char *text) {
    schema_pattern *pattern = calloc(1, sizeof *pattern);
    reader in = {text, 0, 0};
    if (pattern == NULL) {
        return NULL;
    }
    parse_choice(&in, &pattern->top);
    if (in.failed || peek(&in) != '\0') {
        schema_pattern_release(pattern);
        return NULL;
    }
    return pattern;
}

void schema_pattern_release(schema_pattern *pattern) {
    if (pattern != NULL) {
        release_choice(&pattern->top);
        free(pattern);
    }
}

static int match_sequence(const subject *in, const sequence *each, size_t index, size_t at,
                          const continuation *next);

static int match_choice(const subject *in, const choice *alternatives, size_t at, const continuation *next) {
    for (size_t index = 0; index < alternatives->count; index++) {
        if (match_sequence(in, &alternatives->sequences[index], 0, at, next)) {
            return 1;
        }
    }
    return 0;
}

static int repeat_group(const subject *in, const node *group, int done, size_t at, const continuation *after);

static int resume(const subject *in, const continuation *next, size_t at) {
    if (next == NULL) {
        return 1;
    }
    if (next->repeat == NULL) {
        return match_sequence(in, next->sequence, next->index, at, next->next);
    }
    if (at == next->start && next->done > next->repeat->least) {
        return 0;
    }
    return repeat_group(in, next->repeat, next->done, at, next->next);
}

static int repeat_group(const subject *in, const node *group, int done, size_t at, const continuation *after) {
    if (group->most == UNBOUNDED || done < group->most) {
        continuation again = {NULL, 0, group, done + 1, at, after};
        if (match_choice(in, &group->choice, at, &again)) {
            return 1;
        }
    }
    return done >= group->least && resume(in, after, at);
}

static int match_bytes(const subject *in, const sequence *each, size_t index, size_t at, const continuation *next) {
    const node *bytes = &each->nodes[index];
    size_t count = 0;
    while ((bytes->most == UNBOUNDED || count < (size_t)bytes->most) && at + count < in->length &&
           has_byte(bytes->bytes, in->bytes[at + count])) {
        count++;
    }
    for (;;) {
        if (count >= (size_t)bytes->least && match_sequence(in, each, index + 1, at + count, next)) {
            return 1;
        }
        if (count <= (size_t)bytes->least) {
            return 0;
        }
        count--;
    }
}

static int match_sequence(const subject *in, const sequence *each, size_t index, size_t at,
                          const continuation *next) {
    if (index == each->count) {
        return resume(in, next, at);
    }
    const node *current = &each->nodes[index];
    continuation rest = {each, index + 1, NULL, 0, 0, next};
    switch (current->kind) {
    case NODE_START:
        return at == 0 && match_sequence(in, each, index + 1, at, next);
    case NODE_END:
        return at == in->length && match_sequence(in, each, index + 1, at, next);
    case NODE_AHEAD:
    case NODE_NOT_AHEAD:
        if (match_choice(in, &current->choice, at, NULL) != (current->kind == NODE_AHEAD)) {
            return 0;
        }
        return match_sequence(in, each, index + 1, at, next);
    case NODE_GROUP:
        return repeat_group(in, current, 0, at, &rest);
    default:
        return match_bytes(in, each, index, at, next);
    }
}

int schema_pattern_search(const schema_pattern *pattern, const char *text, size_t length) {
    subject in = {(const unsigned char *)text, length};
    for (size_t start = 0; start <= length; start++) {
        if (match_choice(&in, &pattern->top, start, NULL)) {
            return 1;
        }
    }
    return 0;
}
