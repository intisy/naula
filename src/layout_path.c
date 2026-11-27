#include <string.h>

#include "layout_internal.h"
#include "naula/memory.h"

static const char *const RESERVED_NAMES[] = {"repos",   "archive",     "CLAUDE.md",      "CLAUDE.local.md",
                                             ".claude", ".naula.json", ".naula-journal", NULL};

static char folded(char character) {
    return character >= 'A' && character <= 'Z' ? (char)(character - 'A' + 'a') : character;
}

static int is_separator(char character) {
    return character == '/' || character == '\\';
}

int layout_same_name(const char *left, const char *right) {
    for (; *left != '\0' && *right != '\0'; left++, right++) {
        if (folded(*left) != folded(*right)) {
            return 0;
        }
    }
    return *left == *right;
}

int layout_is_reserved(const char *name) {
    for (size_t index = 0; RESERVED_NAMES[index] != NULL; index++) {
        if (layout_same_name(name, RESERVED_NAMES[index])) {
            return 1;
        }
    }
    return 0;
}

char *layout_join(const char *parent, const char *name) {
    size_t parent_length = strlen(parent);
    size_t name_length = strlen(name);
    char *joined = naula_memory_allocate(parent_length + name_length + 2);
    if (joined != NULL) {
        memcpy(joined, parent, parent_length);
        joined[parent_length] = '/';
        memcpy(joined + parent_length + 1, name, name_length + 1);
    }
    return joined;
}

char *layout_concatenate(const char *first, const char *second) {
    size_t first_length = strlen(first);
    size_t second_length = strlen(second);
    char *joined = naula_memory_allocate(first_length + second_length + 1);
    if (joined != NULL) {
        memcpy(joined, first, first_length);
        memcpy(joined + first_length, second, second_length + 1);
    }
    return joined;
}

static size_t without_trailing_separators(const char *path) {
    size_t length = strlen(path);
    while (length > 0 && is_separator(path[length - 1])) {
        length--;
    }
    return length;
}

static int same_character(char left, char right) {
    return (is_separator(left) && is_separator(right)) || folded(left) == folded(right);
}

static int encloses(const char *outer, const char *inner, int strictly) {
    size_t outer_length = without_trailing_separators(outer);
    size_t inner_length = without_trailing_separators(inner);
    if (inner_length < outer_length) {
        return 0;
    }
    for (size_t index = 0; index < outer_length; index++) {
        if (!same_character(outer[index], inner[index])) {
            return 0;
        }
    }
    if (inner_length == outer_length) {
        return !strictly;
    }
    return is_separator(inner[outer_length]);
}

int layout_path_encloses(const char *outer, const char *inner) {
    return encloses(outer, inner, 0);
}

int layout_path_strictly_encloses(const char *outer, const char *inner) {
    return encloses(outer, inner, 1);
}

int layout_path_equal(const char *left, const char *right) {
    size_t left_length = without_trailing_separators(left);
    if (left_length != without_trailing_separators(right)) {
        return 0;
    }
    for (size_t index = 0; index < left_length; index++) {
        if (left[index] != right[index] && !(is_separator(left[index]) && is_separator(right[index]))) {
            return 0;
        }
    }
    return 1;
}

const char *layout_basename(const char *path) {
    const char *name = path;
    for (const char *character = path; *character != '\0'; character++) {
        if (is_separator(*character) && character[1] != '\0') {
            name = character + 1;
        }
    }
    return name;
}

const char *layout_below_root(const char *root, const char *path) {
    size_t root_length = strlen(root);
    return path[root_length] == '/' ? path + root_length + 1 : path + root_length;
}

static size_t segment_count(const char *path) {
    if (*path == '\0') {
        return 0;
    }
    size_t count = 1;
    for (const char *character = path; *character != '\0'; character++) {
        count += *character == '/';
    }
    return count;
}

static const char *after_segments(const char *path, size_t common) {
    for (; common > 0 && *path != '\0'; common--) {
        const char *slash = strchr(path, '/');
        path = slash == NULL ? path + strlen(path) : slash + 1;
    }
    return path;
}

static size_t common_segments(const char *from, const char *to) {
    size_t common = 0;
    while (*from != '\0' && *to != '\0') {
        size_t from_length = strcspn(from, "/");
        size_t to_length = strcspn(to, "/");
        if (from_length != to_length || memcmp(from, to, from_length) != 0) {
            break;
        }
        common++;
        from += from_length + (from[from_length] == '/');
        to += to_length + (to[to_length] == '/');
    }
    return common;
}

char *layout_relative(const char *from, const char *to) {
    size_t common = common_segments(from, to);
    size_t climbs = segment_count(from) - common;
    const char *rest = after_segments(to, common);
    size_t rest_length = strlen(rest);
    char *relative = naula_memory_allocate(climbs * 3 + rest_length + 1);
    if (relative == NULL) {
        return NULL;
    }
    char *cursor = relative;
    for (size_t index = 0; index < climbs; index++) {
        memcpy(cursor, "../", 3);
        cursor += 3;
    }
    memcpy(cursor, rest, rest_length + 1);
    if (rest_length == 0 && cursor > relative) {
        cursor[-1] = '\0';
    }
    return relative;
}

int layout_same_repository(const naula_id *left, const naula_id *right) {
    if (left->kind != right->kind) {
        return 0;
    }
    if (left->kind == NAULA_ID_GIT) {
        return strcmp(left->commit, right->commit) == 0;
    }
    return left->local_name != NULL && right->local_name != NULL && strcmp(left->local_name, right->local_name) == 0;
}

/** Compared as text, never parsed, so matching a reference cannot run out of memory and quietly answer no. */
static int reference_id_names(const char *text, const naula_id *id) {
    size_t length = strlen(text);
    if (id->kind != NAULA_ID_GIT || length < 4 || memcmp(text, "git:", 4) != 0 || !naula_id_is_valid(text, length)) {
        return 0;
    }
    const char *hex = text + 4;
    size_t hex_length = strcspn(hex, "/");
    return hex_length == strlen(id->commit) && memcmp(hex, id->commit, hex_length) == 0;
}

int layout_reference_names(const naula_reference *reference, const naula_id *id, const naula_home *primary) {
    if (reference->id != NULL) {
        return reference_id_names(reference->id, id);
    }
    return reference->url != NULL && primary != NULL && primary->kind == NAULA_HOME_URL && primary->url != NULL &&
           strcmp(reference->url, primary->url) == 0;
}
