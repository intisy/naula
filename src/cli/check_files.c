#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "check_internal.h"
#include "cli.h"
#include "naula/memory.h"

#define FILE_SIZE_LIMIT (4u * 1024u * 1024u)
#define BINARY_PROBE 8000u

static const char *const build_extensions[] = {".gradle", ".kts", ".ps1", ".sh", ".yml", ".yaml", ".properties",
                                               ".cmake"};
static const char *const source_extensions[] = {".ts", ".tsx", ".mts", ".js", ".mjs"};
static const char *const record_folders[] = {"spec", "specs", "plan", "plans", "archive", "superpowers",
                                             "measurements", "sdd-records"};
static const char *const record_files[] = {"TASKS.md", "ECOSYSTEM.md", "TRAPS.md"};

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static int ends_with(const char *text, const char *ending) {
    size_t length = strlen(text);
    size_t ending_length = strlen(ending);
    return length >= ending_length && strcmp(text + length - ending_length, ending) == 0;
}

static int has_extension(const char *path, const char *const *extensions, size_t count) {
    for (size_t index = 0; index < count; index++) {
        if (ends_with(path, extensions[index])) {
            return 1;
        }
    }
    return 0;
}

static int is_build_file(const char *path) {
    const char *name = base_name(path);
    return has_extension(path, build_extensions, sizeof build_extensions / sizeof build_extensions[0]) ||
           strcmp(name, "CMakeLists.txt") == 0 || strcmp(name, "Makefile") == 0 || strcmp(name, "package.json") == 0 ||
           (strncmp(name, "tsconfig", 8) == 0 && ends_with(name, ".json"));
}

static int is_source_file(const char *path) {
    return has_extension(path, source_extensions, sizeof source_extensions / sizeof source_extensions[0]);
}

static int is_workflow(const char *path) {
    return strncmp(path, ".github/workflows/", 18) == 0;
}

static int segment_is_record_folder(const char *segment, size_t length) {
    for (size_t index = 0; index < sizeof record_folders / sizeof record_folders[0]; index++) {
        const char *folder = record_folders[index];
        if (strlen(folder) == length) {
            size_t at = 0;
            while (at < length && tolower((unsigned char)segment[at]) == folder[at]) {
                at++;
            }
            if (at == length) {
                return 1;
            }
        }
    }
    return 0;
}

static int names_a_date(const char *path) {
    for (const char *at = path; *at != '\0'; at++) {
        int matches = 1;
        for (size_t offset = 0; offset < 10 && matches; offset++) {
            char letter = at[offset];
            matches = offset == 4 || offset == 7 ? letter == '-' : letter >= '0' && letter <= '9';
        }
        if (matches) {
            return 1;
        }
    }
    return 0;
}

/** A record keeps what was true when it was written, names included, so the literal rule never reads one. */
static int same_ignoring_case(const char *left, const char *right) {
    for (; *left != '\0' && *right != '\0'; left++, right++) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return 0;
        }
    }
    return *left == *right;
}

/** workspace's RECORD_FILE (workspace/src/checks.ts), which matches ignoring case, folders and files alike. */
static int is_record(const char *path) {
    for (size_t index = 0; index < sizeof record_files / sizeof record_files[0]; index++) {
        if (same_ignoring_case(base_name(path), record_files[index])) {
            return 1;
        }
    }
    for (const char *segment = path; *segment != '\0';) {
        const char *end = strchr(segment, '/');
        if (end == NULL) {
            break;
        }
        if (segment_is_record_folder(segment, (size_t)(end - segment))) {
            return 1;
        }
        segment = end + 1;
    }
    return names_a_date(path);
}

static size_t depth_of(const char *path) {
    if (is_workflow(path)) {
        return 0;
    }
    size_t depth = 0;
    for (const char *at = path; *at != '\0'; at++) {
        depth += *at == '/';
    }
    return depth;
}

static int is_comment_line(const char *line, size_t length) {
    size_t at = 0;
    while (at < length && (line[at] == ' ' || line[at] == '\t')) {
        at++;
    }
    return at < length && (line[at] == '#' || line[at] == '*' || (line[at] == '/' && at + 1 < length &&
                                                                    (line[at + 1] == '/' || line[at + 1] == '*')));
}

static int is_word_or_dot(char letter) {
    return isalnum((unsigned char)letter) || letter == '_' || letter == '.';
}

static int is_separator(char letter) {
    return letter == '/' || letter == '\\';
}

/** How many `..` the run starting at `at` climbs, 0 when it is not a run of parent segments. */
static size_t climb_at(const char *line, size_t length, size_t at) {
    if (at > 0 && is_word_or_dot(line[at - 1])) {
        return 0;
    }
    size_t count = 0;
    size_t cursor = at;
    while (cursor + 1 < length && line[cursor] == '.' && line[cursor + 1] == '.') {
        count++;
        cursor += 2;
        if (cursor + 2 < length && is_separator(line[cursor]) && line[cursor + 1] == '.' && line[cursor + 2] == '.') {
            cursor++;
            continue;
        }
        break;
    }
    if (count == 0) {
        return 0;
    }
    char next = cursor < length ? line[cursor] : '\0';
    int ends = next == '\0' || is_separator(next) || next == '"' || next == '\'' || next == ' ' || next == '\t' ||
               next == ')';
    return ends ? count : 0;
}

static int has_root_parent(const char *line, size_t length) {
    static const char *const anchors[] = {"rootDir", "rootProject.projectDir"};
    for (size_t index = 0; index < 2; index++) {
        size_t anchor_length = strlen(anchors[index]);
        for (size_t at = 0; at + anchor_length <= length; at++) {
            if (memcmp(line + at, anchors[index], anchor_length) != 0) {
                continue;
            }
            size_t cursor = at + anchor_length;
            cursor += cursor < length && line[cursor] == '}';
            if ((cursor + 2 < length && is_separator(line[cursor]) && line[cursor + 1] == '.' && line[cursor + 2] == '.') ||
                (cursor + 7 <= length && memcmp(line + cursor, ".parent", 7) == 0)) {
                return 1;
            }
        }
    }
    return 0;
}

static int build_line_climbs(const char *line, size_t length, size_t depth) {
    if (is_comment_line(line, length)) {
        return 0;
    }
    if (has_root_parent(line, length)) {
        return 1;
    }
    for (size_t at = 0; at < length; at++) {
        if (line[at] == '.' && climb_at(line, length, at) > depth) {
            return 1;
        }
    }
    return 0;
}

static int source_line_climbs(const char *line, size_t length, size_t depth) {
    static const char opening[] = "new URL(";
    for (size_t at = 0; at + sizeof opening < length; at++) {
        if (memcmp(line + at, opening, sizeof opening - 1) != 0) {
            continue;
        }
        size_t cursor = at + sizeof opening - 1;
        char quote = line[cursor];
        if (quote != '"' && quote != '\'' && quote != '`') {
            continue;
        }
        size_t count = 0;
        cursor++;
        while (cursor + 2 < length && line[cursor] == '.' && line[cursor + 1] == '.' && line[cursor + 2] == '/') {
            count++;
            cursor += 3;
        }
        if (count > depth) {
            return 1;
        }
    }
    return 0;
}

static int matches_account(const char *text, size_t length, size_t at, const char *account, size_t *end) {
    size_t account_length = strlen(account);
    if (at + account_length + 1 >= length || (at > 0 && (is_word_or_dot(text[at - 1]) || text[at - 1] == '-'))) {
        return 0;
    }
    for (size_t index = 0; index < account_length; index++) {
        if (tolower((unsigned char)text[at + index]) != tolower((unsigned char)account[index])) {
            return 0;
        }
    }
    size_t cursor = at + account_length;
    if (text[cursor] != '/') {
        return 0;
    }
    size_t name_start = ++cursor;
    while (cursor < length && (is_word_or_dot(text[cursor]) || text[cursor] == '-')) {
        cursor++;
    }
    *end = cursor;
    return cursor > name_start && isalnum((unsigned char)text[name_start]);
}

/** The first `account/name` in the line, its text copied out; NULL when the line names none. */
static char *literal_in(const char *line, size_t length, const char *const *accounts, size_t account_count) {
    for (size_t at = 0; at < length; at++) {
        for (size_t index = 0; index < account_count; index++) {
            size_t end = 0;
            if (matches_account(line, length, at, accounts[index], &end)) {
                return cli_format("%.*s", (int)(end - at), line + at);
            }
        }
    }
    return NULL;
}

/** A step's `uses:` key, as `uses:` or `- uses:` after the indentation; `uses:` anywhere else in a line is not one. */
static int is_uses_line(const char *line, size_t length) {
    size_t at = 0;
    while (at < length && (line[at] == ' ' || line[at] == '\t')) {
        at++;
    }
    if (at < length && line[at] == '-') {
        at++;
        while (at < length && (line[at] == ' ' || line[at] == '\t')) {
            at++;
        }
    }
    return at + 5 <= length && memcmp(line + at, "uses:", 5) == 0;
}

typedef struct file_scan {
    const char *path;
    int climbs;
    int source;
    size_t depth;
    int literals;
    const char *const *accounts;
    size_t account_count;
    check_findings *findings;
} file_scan;

static void scan_line(file_scan *scan, const char *line, size_t length, size_t number) {
    int climbs = scan->source ? source_line_climbs(line, length, scan->depth) : build_line_climbs(line, length, scan->depth);
    if (scan->climbs && climbs) {
        check_add(scan->findings, CHECK_ERROR, "climbs-out", "reach other repos through siblings/, never by a relative path",
                  "%s:%zu climbs out of the repo", scan->path, number);
        scan->climbs = 0;
    }
    if (scan->literals && !(is_workflow(scan->path) && is_uses_line(line, length))) {
        char *literal = literal_in(line, length, scan->accounts, scan->account_count);
        if (literal != NULL) {
            check_add(scan->findings, CHECK_ERROR, "owner-repo-literal",
                      "name the repo by its id in repo.json, never by owner and name",
                      "%s:%zu names %s, a literal owner and name of ours", scan->path, number, literal);
            scan->literals = 0;
        }
        naula_memory_release(literal);
    }
}

static int is_binary(const uint8_t *bytes, size_t length) {
    return memchr(bytes, '\0', length < BINARY_PROBE ? length : BINARY_PROBE) != NULL;
}

static naula_status scan_file(const char *repository, file_scan *scan) {
    char *path = cli_join(repository, scan->path);
    if (path == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    uint8_t *bytes = NULL;
    size_t length = 0;
    naula_refusal ignored = cli_fresh_refusal();
    naula_status status = cli_read_file(path, &bytes, &length, &ignored);
    naula_memory_release(path);
    if (status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    if (status != NAULA_OK || length > FILE_SIZE_LIMIT || is_binary(bytes, length)) {
        naula_memory_release(bytes);
        return NAULA_OK;
    }
    const char *text = (const char *)bytes;
    size_t number = 1;
    for (size_t start = 0; start < length && (scan->climbs || scan->literals); number++) {
        const char *end = memchr(text + start, '\n', length - start);
        size_t line_length = end == NULL ? length - start : (size_t)(end - (text + start));
        size_t trimmed = line_length > 0 && text[start + line_length - 1] == '\r' ? line_length - 1 : line_length;
        scan_line(scan, text + start, trimmed, number);
        start += line_length + 1;
    }
    naula_memory_release(bytes);
    return NAULA_OK;
}

naula_status check_tracked_files(const char *repository, const char *const *accounts, size_t account_count,
                                 check_findings *findings, naula_refusal *refusal) {
    const char *arguments[] = {"ls-files", "-z", NULL};
    naula_run_result result;
    naula_status status = cli_git(repository, arguments, &result, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    for (size_t start = 0; status == NAULA_OK && start < result.output_length;) {
        const char *path = result.output + start;
        size_t length = strlen(path);
        file_scan scan;
        memset(&scan, 0, sizeof scan);
        scan.path = path;
        scan.source = is_source_file(path);
        scan.climbs = is_build_file(path) || scan.source;
        scan.depth = depth_of(path);
        scan.literals = account_count > 0 && strcmp(path, "repo.json") != 0 && !is_record(path);
        scan.accounts = accounts;
        scan.account_count = account_count;
        scan.findings = findings;
        if (scan.climbs || scan.literals) {
            status = scan_file(repository, &scan);
        }
        start += length + 1;
    }
    naula_run_result_release(&result);
    return status;
}
