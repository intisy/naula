#ifdef _WIN32
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include "disk.h"
#include "layout_apply_internal.h"
#include "memory_testing.h"
#include "naula/json.h"
#include "naula/layout_apply.h"
#include "naula/memory.h"

#define PATH_SIZE 1024
#define HEX "aaaaaaaaaaaa"

static char base[PATH_SIZE];
static char root[PATH_SIZE];
static char banks[PATH_SIZE];

/** Joins its NULL-terminated parts into `out`, and stops the test rather than cut a path short. */
static void compose(char *out, ...) {
    va_list parts;
    va_start(parts, out);
    size_t length = 0;
    for (const char *part = va_arg(parts, const char *); part != NULL; part = va_arg(parts, const char *)) {
        size_t part_length = strlen(part);
        if (length + part_length >= PATH_SIZE) {
            fprintf(stderr, "a test path passes %d bytes\n", PATH_SIZE);
            abort();
        }
        memmove(out + length, part, part_length);
        length += part_length;
    }
    out[length] = '\0';
    va_end(parts);
}

static const char *at(const char *relative) {
    static char paths[16][PATH_SIZE];
    static size_t next;
    char *path = paths[next++ % 16];
    compose(path, root, "/", relative, NULL);
    return path;
}

static int is_inside_base(const char *path) {
    return strncmp(path, base, strlen(base)) == 0 && strstr(path, "..") == NULL;
}

/** 0 missing, 1 folder, 2 file, 3 link or other reparse point; never follows a link. */
static int kind_at(const char *path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        return 3;
    }
    return attributes & FILE_ATTRIBUTE_DIRECTORY ? 1 : 2;
#else
    struct stat found;
    if (lstat(path, &found) != 0) {
        return 0;
    }
    if (S_ISLNK(found.st_mode)) {
        return 3;
    }
    return S_ISDIR(found.st_mode) ? 1 : 2;
#endif
}

typedef struct names {
    char **items;
    size_t count;
} names;

static int compare_names(const void *left, const void *right) {
    return strcmp(*(char *const *)left, *(char *const *)right);
}

static names list_names(const char *folder) {
    names found = {NULL, 0};
    size_t capacity = 0;
#ifdef _WIN32
    char pattern[PATH_SIZE];
    compose(pattern, folder, "/*", NULL);
    WIN32_FIND_DATAA entry;
    HANDLE search = FindFirstFileA(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) {
        return found;
    }
    do {
        const char *name = entry.cFileName;
#else
    DIR *listing = opendir(folder);
    if (listing == NULL) {
        return found;
    }
    struct dirent *entry;
    while ((entry = readdir(listing)) != NULL) {
        const char *name = entry->d_name;
#endif
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        if (found.count == capacity) {
            capacity = capacity == 0 ? 8 : capacity * 2;
            found.items = realloc(found.items, capacity * sizeof *found.items);
        }
        size_t length = strlen(name);
        found.items[found.count] = malloc(length + 1);
        memcpy(found.items[found.count], name, length + 1);
        found.count++;
#ifdef _WIN32
    } while (FindNextFileA(search, &entry));
    FindClose(search);
#else
    }
    closedir(listing);
#endif
    if (found.count > 1) {
        qsort(found.items, found.count, sizeof *found.items, compare_names);
    }
    return found;
}

static void release_names(names *found) {
    for (size_t index = 0; index < found->count; index++) {
        free(found->items[index]);
    }
    free(found->items);
}

/** The test's own removal, which never enters a link and refuses anything outside the test's folder. */
static void remove_tree(const char *path) {
    if (!is_inside_base(path)) {
        fprintf(stderr, "refusing to remove %s outside %s\n", path, base);
        abort();
    }
    int kind = kind_at(path);
    if (kind == 1) {
        names found = list_names(path);
        for (size_t index = 0; index < found.count; index++) {
            char child[PATH_SIZE];
            compose(child, path, "/", found.items[index], NULL);
            remove_tree(child);
        }
        release_names(&found);
    }
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        RemoveDirectoryA(path);
    } else {
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
        DeleteFileA(path);
    }
#else
    if (kind == 1) {
        rmdir(path);
    } else if (kind != 0) {
        unlink(path);
    }
#endif
}

static void make_folder(const char *path) {
#ifdef _WIN32
    CreateDirectoryA(path, NULL);
#else
    mkdir(path, 0777);
#endif
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        check_failures++;
        return;
    }
    fwrite(text, 1, strlen(text), file);
    fclose(file);
}

static char *read_text(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    size_t capacity = 4096;
    size_t length = 0;
    char *text = malloc(capacity);
    for (;;) {
        size_t got = fread(text + length, 1, capacity - length - 1, file);
        length += got;
        if (got == 0) {
            break;
        }
        if (length + 1 == capacity) {
            capacity *= 2;
            text = realloc(text, capacity);
        }
    }
    fclose(file);
    text[length] = '\0';
    return text;
}

static int file_holds(const char *path, const char *expected) {
    char *text = read_text(path);
    int holds = text != NULL && strcmp(text, expected) == 0;
    free(text);
    return holds;
}

static void make_link(const char *path, const char *target) {
    unsigned long error = 0;
    if (disk_link_make(path, target, &error) != NAULA_OK) {
        fprintf(stderr, "cannot link %s to %s: %lu\n", path, target, error);
        check_failures++;
    }
}

static void unlink_link(const char *path) {
    unsigned long error = 0;
    if (disk_link_remove(path, &error) != NAULA_OK) {
        fprintf(stderr, "cannot remove the link %s: %lu\n", path, error);
        check_failures++;
    }
}

static int links_to(const char *path, const char *target) {
    char *found = NULL;
    unsigned long error = 0;
    if (kind_at(path) != 3 || disk_link_target(path, &found, &error) != NAULA_OK) {
        return 0;
    }
    int same = apply_same_target(found, target);
    naula_memory_release(found);
    return same;
}

/** A folder holding `.git`, so the applier reads it as a checkout. */
static void make_checkout(const char *path, const char *marker) {
    char child[PATH_SIZE];
    make_folder(path);
    compose(child, path, "/.git", NULL);
    make_folder(child);
    compose(child, path, "/.git/HEAD", NULL);
    write_text(child, "ref: refs/heads/development\n");
    compose(child, path, "/README.md", NULL);
    write_text(child, marker);
}

static void fresh(const char *name) {
    compose(root, base, "/", name, "/root", NULL);
    compose(banks, base, "/", name, "/banks", NULL);
    char folder[PATH_SIZE];
    compose(folder, base, "/", name, NULL);
    remove_tree(folder);
    make_folder(base);
    make_folder(folder);
    make_folder(root);
    make_folder(banks);
}

static void finish(const char *name) {
    char folder[PATH_SIZE];
    compose(folder, base, "/", name, NULL);
    remove_tree(folder);
}

#define MAX_STEPS 16

typedef struct plan_text {
    naula_layout_step steps[MAX_STEPS];
    char paths[MAX_STEPS][PATH_SIZE];
    char targets[MAX_STEPS][PATH_SIZE];
    naula_layout_plan plan;
} plan_text;

static plan_text plans;

static void plan_start(void) {
    memset(&plans, 0, sizeof plans);
    plans.plan.size = sizeof plans.plan;
    plans.plan.steps = plans.steps;
}

static void plan_add(naula_layout_step_kind kind, const char *path, const char *target, const char *content) {
    size_t index = plans.plan.step_count++;
    naula_layout_step *step = &plans.steps[index];
    step->size = sizeof *step;
    step->kind = kind;
    if (path != NULL) {
        compose(plans.paths[index], path, NULL);
        step->path = plans.paths[index];
    }
    if (target != NULL) {
        compose(plans.targets[index], target, NULL);
        step->target = plans.targets[index];
    }
    step->content = (char *)content;
}

static naula_apply_options options_for(const char *memory_banks) {
    naula_apply_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.root = root;
    options.memory_banks = memory_banks;
    return options;
}

static naula_refusal refusal;

static naula_status apply_plan(void) {
    naula_apply_options options = options_for(banks);
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    return naula_layout_apply(&plans.plan, &options, &refusal);
}

static int refused_with(naula_status status, const char *code) {
    if (status == NAULA_REFUSED && strcmp(refusal.code, code) == 0) {
        return 1;
    }
    fprintf(stderr, "expected a refusal %s, got status %d code %s: %s\n", code, (int)status, refusal.code,
            refusal.detail);
    return 0;
}

static void record_link(const char *path, const char *target) {
    link_record record = {NULL, 0};
    naula_refusal ignored;
    ignored.size = sizeof ignored;
    CHECK(apply_check_root(root, 1, &ignored) == NAULA_OK);
    CHECK(link_record_read(root, &record, &ignored) == NAULA_OK);
    CHECK(link_record_put(root, &record, path, target, &ignored) == NAULA_OK);
    link_record_release(&record);
}

static int is_recorded(const char *path, const char *target) {
    link_record record = {NULL, 0};
    naula_refusal ignored;
    ignored.size = sizeof ignored;
    if (link_record_read(root, &record, &ignored) != NAULA_OK) {
        return 0;
    }
    const naula_layout_link *found = link_record_find(&record, path);
    int recorded = found != NULL && (target == NULL || apply_same_target(found->target, target));
    link_record_release(&record);
    return recorded;
}

static char *unfinished_journal(void) {
    char *journal_file = NULL;
    naula_refusal ignored;
    ignored.size = sizeof ignored;
    return naula_journal_unfinished(root, &journal_file, &ignored) == NAULA_OK ? journal_file : NULL;
}

static naula_status resume(const char *journal_file) {
    naula_apply_options options = options_for(banks);
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    return naula_journal_resume(journal_file, &options, &refusal);
}

static unsigned long own_process_id(void) {
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

/** A process id no process holds: past Linux's largest pid, and one Windows answers as invalid. */
#define GONE_PROCESS_ID 2147480000ul

static void write_lock(unsigned long process_id) {
    char text[32];
    snprintf(text, sizeof text, "%lu\n", process_id);
    make_folder(at(".naula-journal"));
    write_text(at(".naula-journal/lock"), text);
}

/** A crash leaves the lock behind; the person resuming removes it, as the stale lock's fix says. */
static void after_a_simulated_crash(void) {
    naula_apply_test_stop_at(0);
    CHECK(kind_at(at(".naula-journal/lock")) == 2);
    remove_tree(at(".naula-journal/lock"));
}

static void append_snapshot(char **text, size_t *length, size_t *capacity, const char *line) {
    size_t line_length = strlen(line);
    while (*length + line_length + 1 > *capacity) {
        *capacity = *capacity == 0 ? 4096 : *capacity * 2;
        *text = realloc(*text, *capacity);
    }
    memcpy(*text + *length, line, line_length + 1);
    *length += line_length;
}

/** Every path under `folder`, what it is, a file's bytes and a link's target, journals left out. */
static void snapshot_into(const char *folder, const char *shown, char **text, size_t *length, size_t *capacity) {
    names found = list_names(folder);
    for (size_t index = 0; index < found.count; index++) {
        const char *name = found.items[index];
        size_t name_length = strlen(name);
        if (name_length > 6 && strcmp(name + name_length - 6, ".jsonl") == 0) {
            continue;
        }
        char child[PATH_SIZE];
        char child_shown[PATH_SIZE];
        compose(child, folder, "/", name, NULL);
        compose(child_shown, shown, "/", name, NULL);
        int kind = kind_at(child);
        if (kind == 3) {
            char *target = NULL;
            unsigned long error = 0;
            disk_link_target(child, &target, &error);
            append_snapshot(text, length, capacity, "link ");
            append_snapshot(text, length, capacity, child_shown);
            append_snapshot(text, length, capacity, " -> ");
            append_snapshot(text, length, capacity, target == NULL ? "?" : target);
            append_snapshot(text, length, capacity, "\n");
            naula_memory_release(target);
        } else if (kind == 2) {
            char *bytes = read_text(child);
            append_snapshot(text, length, capacity, "file ");
            append_snapshot(text, length, capacity, child_shown);
            append_snapshot(text, length, capacity, " = ");
            append_snapshot(text, length, capacity, bytes == NULL ? "?" : bytes);
            append_snapshot(text, length, capacity, "\n");
            free(bytes);
        } else if (kind == 1) {
            append_snapshot(text, length, capacity, "folder ");
            append_snapshot(text, length, capacity, child_shown);
            append_snapshot(text, length, capacity, "\n");
            snapshot_into(child, child_shown, text, length, capacity);
        }
    }
    release_names(&found);
}

static char *snapshot(const char *name) {
    char folder[PATH_SIZE];
    compose(folder, base, "/", name, NULL);
    char *text = NULL;
    size_t length = 0;
    size_t capacity = 0;
    append_snapshot(&text, &length, &capacity, "");
    snapshot_into(folder, "", &text, &length, &capacity);
    return text;
}

static void memory_bank_key_matches_the_measured_form(void) {
    char key[256];
    naula_memory_bank_key("F:\\Documents\\GitHub\\spisor", key, sizeof key);
    check_text(key, "F--Documents-GitHub-spisor", "windows path");
    naula_memory_bank_key("F:/Documents/GitHub/spisor/repos/0123456789ab", key, sizeof key);
    check_text(key, "F--Documents-GitHub-spisor-repos-0123456789ab", "forward slashes");
    naula_memory_bank_key("/home/finn/root/org", key, sizeof key);
    check_text(key, "-home-finn-root-org", "posix path");
    naula_memory_bank_key("F:\\Documents", key, 5);
    check_text(key, "", "a key that does not fit is never cut short");
    naula_memory_bank_key("C:\\Users\\finn\\AppData\\Local\\Temp\\naula_key.probe+x", key, sizeof key);
    check_text(key, "C--Users-finn-AppData-Local-Temp-naula-key-probe-x", "measured with Claude Code 2.1.332");
    naula_memory_bank_key("C:\\Users\\finn\\.claude\\verbrauch", key, sizeof key);
    check_text(key, "C--Users-finn--claude-verbrauch", "measured with Claude Code 2.1.332");
    naula_memory_bank_key("c:/a_b", key, sizeof key);
    check_text(key, "c--a-b", "an underscore, and the drive letter keeps its case");
    naula_memory_bank_key("/a.b", key, sizeof key);
    check_text(key, "-a-b", "a dot");
    naula_memory_bank_key("/a+b", key, sizeof key);
    check_text(key, "-a-b", "a plus");
    naula_memory_bank_key("/a b", key, sizeof key);
    check_text(key, "-a-b", "a space");
    naula_memory_bank_key("/a\xc3\xbc" "b", key, sizeof key);
    check_text(key, "-a-b", "a two-byte character is one UTF-16 unit");
    naula_memory_bank_key("/a\xe2\x82\xac" "b", key, sizeof key);
    check_text(key, "-a-b", "a three-byte character is one UTF-16 unit");
    naula_memory_bank_key("/a\xf0\x9f\x98\x80" "b", key, sizeof key);
    check_text(key, "-a--b", "a four-byte character is two UTF-16 units");
    naula_memory_bank_key("/a\xff" "z", key, sizeof key);
    check_text(key, "-a-z", "a byte that is not UTF-8");
}

/**
 * Past 200 units Claude Code keeps the first 200, then `-` and a base 36 hash of the path. The first vector is the
 * folder Claude Code 2.1.332 made for a probe run on 2026-11-27; the others are its bundled hash function evaluated
 * independently, one over a character past U+FFFF.
 */
static void a_long_memory_bank_key_ends_in_claude_codes_hash(void) {
    char path[512];
    char key[512];
    char expected[512];
    compose(path, "C:\\Users\\finn\\AppData\\Local\\Temp\\claude\\e1cb3709954c7f49\\a9c280f0-bdb8-4a46-86bc-81d337debcf1"
                  "\\scratchpad\\keyprobe\\naula_long_key_probe.",
            NULL);
    size_t length = strlen(path);
    memset(path + length, 'x', 90);
    path[length + 90] = '\0';
    compose(expected, "C--Users-finn-AppData-Local-Temp-claude-e1cb3709954c7f49-a9c280f0-bdb8-4a46-86bc-81d337debcf1-"
                      "scratchpad-keyprobe-naula-long-key-probe-",
            NULL);
    length = strlen(expected);
    memset(expected + length, 'x', 200 - length);
    compose(expected + 200, "-i1xwsc", NULL);
    naula_memory_bank_key(path, key, sizeof key);
    check_text(key, expected, "measured with Claude Code 2.1.332");

    memset(path, 'a', 201);
    path[0] = '/';
    path[201] = '\0';
    memset(expected, 'a', 200);
    expected[0] = '-';
    compose(expected + 200, "-b6ymvl", NULL);
    naula_memory_bank_key(path, key, sizeof key);
    check_text(key, expected, "201 units");
    path[200] = '\0';
    naula_memory_bank_key(path, key, sizeof key);
    expected[200] = '\0';
    check_text(key, expected, "200 units keep no hash");

    memset(path, 'b', 198);
    path[0] = '/';
    compose(path + 198, "\xf0\x9f\x98\x80" "c", NULL);
    memset(expected, 'b', 200);
    expected[0] = '-';
    expected[198] = '-';
    expected[199] = '-';
    compose(expected + 200, "-79yvtf", NULL);
    naula_memory_bank_key(path, key, sizeof key);
    check_text(key, expected, "a character past U+FFFF is two units, hashed as its surrogates");
    naula_memory_bank_key(path, key, 207);
    check_text(key, "", "a long key that does not fit is never cut short");
}

static void apply_makes_folders_links_and_files(void) {
    fresh("makes");
    make_checkout(at("old"), "checkout");
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("spisor"), NULL, NULL);
    plan_add(NAULA_MAKE_FOLDER, at("spisor/repos"), NULL, NULL);
    plan_add(NAULA_MOVE_CHECKOUT, at("old"), at("spisor/repos/" HEX), NULL);
    plan_add(NAULA_MAKE_FOLDER, at("spisor/shared"), NULL, NULL);
    plan_add(NAULA_WRITE_FILE, at("spisor/CLAUDE.md"), NULL, "@docs/RULES.md\n");
    plan_add(NAULA_MAKE_LINK, at("spisor/shared/library"), at("spisor/repos/" HEX), NULL);
    plan_add(NAULA_REPORT, at("spisor/x"), NULL, "a note");
    CHECK(apply_plan() == NAULA_OK);
    CHECK(kind_at(at("old")) == 0);
    CHECK(file_holds(at("spisor/repos/" HEX "/README.md"), "checkout"));
    CHECK(file_holds(at("spisor/CLAUDE.md"), "@docs/RULES.md\n"));
    CHECK(links_to(at("spisor/shared/library"), at("spisor/repos/" HEX)));
    CHECK(file_holds(at("spisor/shared/library/README.md"), "checkout"));
    CHECK(is_recorded(at("spisor/shared/library"), at("spisor/repos/" HEX)));
    CHECK(unfinished_journal() == NULL);
    finish("makes");
}

static void applying_a_plan_twice_changes_nothing(void) {
    fresh("twice");
    make_checkout(at("old"), "checkout");
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("repos"), NULL, NULL);
    plan_add(NAULA_MOVE_CHECKOUT, at("old"), at("repos/" HEX), NULL);
    plan_add(NAULA_WRITE_FILE, at("CLAUDE.md"), NULL, "@docs/RULES.md\n");
    plan_add(NAULA_MAKE_LINK, at("docs"), at("repos/" HEX), NULL);
    CHECK(apply_plan() == NAULA_OK);
    char *first = snapshot("twice");
    CHECK(apply_plan() == NAULA_OK);
    char *second = snapshot("twice");
    CHECK(strcmp(first, second) == 0);
    free(first);
    free(second);
    finish("twice");
}

static char *last_line(const char *path) {
    char *text = read_text(path);
    if (text == NULL) {
        return NULL;
    }
    size_t length = strlen(text);
    while (length > 0 && text[length - 1] == '\n') {
        text[--length] = '\0';
    }
    char *newline = strrchr(text, '\n');
    char *line = newline == NULL ? text : newline + 1;
    size_t line_length = strlen(line);
    char *copy = malloc(line_length + 1);
    memcpy(copy, line, line_length + 1);
    free(text);
    return copy;
}

static void apply_writes_the_journal_before_each_step(void) {
    fresh("before");
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("spisor"), NULL, NULL);
    naula_apply_test_stop_at(2);
    CHECK(refused_with(apply_plan(), "test-stopped"));
    after_a_simulated_crash();
    char *journal_file = unfinished_journal();
    CHECK(journal_file != NULL);
    CHECK(kind_at(at("spisor")) == 0);
    char *line = journal_file == NULL ? NULL : last_line(journal_file);
    CHECK(line != NULL && strcmp(line, "{\"step\":0,\"state\":\"intended\"}") == 0);
    free(line);
    naula_memory_release(journal_file);

    fresh("before");
    naula_apply_test_stop_at(3);
    CHECK(refused_with(apply_plan(), "test-stopped"));
    after_a_simulated_crash();
    journal_file = unfinished_journal();
    CHECK(kind_at(at("spisor")) == 1);
    line = journal_file == NULL ? NULL : last_line(journal_file);
    CHECK(line != NULL && strcmp(line, "{\"step\":0,\"state\":\"intended\"}") == 0);
    free(line);
    CHECK(resume(journal_file) == NAULA_OK);
    line = journal_file == NULL ? NULL : last_line(journal_file);
    CHECK(line != NULL && strcmp(line, "{\"state\":\"finished\"}") == 0);
    free(line);
    naula_memory_release(journal_file);
    finish("before");
}

static char *only_journal(void);

static void apply_stops_at_a_held_folder_and_resumes(void) {
#ifdef _WIN32
    fresh("held");
    make_checkout(at("old"), "held checkout");
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("repos"), NULL, NULL);
    plan_add(NAULA_MOVE_CHECKOUT, at("old"), at("repos/" HEX), NULL);
    plan_add(NAULA_MAKE_LINK, at("name"), at("repos/" HEX), NULL);
    HANDLE held = CreateFileA(at("old/README.md"), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(held != INVALID_HANDLE_VALUE);
    CHECK(refused_with(apply_plan(), "folder-held"));
    CHECK(strstr(refusal.detail, "old") != NULL);
    CHECK(kind_at(at("old")) == 1);
    CHECK(kind_at(at("repos")) == 1);
    CHECK(kind_at(at("repos/" HEX)) == 0);
    CHECK(kind_at(at("name")) == 0);
    CloseHandle(held);
    CHECK(unfinished_journal() == NULL);
    char *journal_file = only_journal();
    CHECK(journal_file != NULL);
    CHECK(refused_with(resume(journal_file), "journal-stopped"));
    CHECK(kind_at(at("old")) == 1);
    CHECK(apply_plan() == NAULA_OK);
    CHECK(kind_at(at("old")) == 0);
    CHECK(file_holds(at("repos/" HEX "/README.md"), "held checkout"));
    CHECK(links_to(at("name"), at("repos/" HEX)));
    CHECK(unfinished_journal() == NULL);
    free(journal_file);
    finish("held");
#else
    printf("apply_stops_at_a_held_folder_and_resumes: skipped, a held folder still renames on POSIX\n");
#endif
}

static const char *bank_of(const char *real_path) {
    static char paths[4][PATH_SIZE];
    static size_t next;
    char key[PATH_SIZE];
    naula_memory_bank_key(real_path, key, sizeof key);
    char *path = paths[next++ % 4];
    compose(path, banks, "/", key, NULL);
    return path;
}

static void apply_renames_the_memory_bank_of_a_moved_checkout(void) {
    fresh("bank");
    make_checkout(at("old"), "checkout");
    char old_bank[PATH_SIZE];
    char memory[PATH_SIZE];
    compose(old_bank, bank_of(at("old")), NULL);
    make_folder(old_bank);
    compose(memory, old_bank, "/MEMORY.md", NULL);
    write_text(memory, "remembered");
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("repos"), NULL, NULL);
    plan_add(NAULA_MOVE_CHECKOUT, at("old"), at("repos/" HEX), NULL);
    CHECK(apply_plan() == NAULA_OK);
    CHECK(kind_at(old_bank) == 0);
    char moved[PATH_SIZE];
    compose(moved, bank_of(at("repos/" HEX)), "/MEMORY.md", NULL);
    CHECK(file_holds(moved, "remembered"));
    finish("bank");
}

static void apply_refuses_an_existing_memory_bank_target(void) {
    fresh("bank-taken");
    make_checkout(at("old"), "checkout");
    char old_bank[PATH_SIZE];
    char new_bank[PATH_SIZE];
    compose(old_bank, bank_of(at("old")), NULL);
    compose(new_bank, bank_of(at("repos/" HEX)), NULL);
    make_folder(old_bank);
    make_folder(new_bank);
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("repos"), NULL, NULL);
    plan_add(NAULA_MOVE_CHECKOUT, at("old"), at("repos/" HEX), NULL);
    CHECK(refused_with(apply_plan(), "memory-bank-exists"));
    CHECK(kind_at(at("old")) == 1);
    CHECK(kind_at(at("repos/" HEX)) == 0);
    CHECK(kind_at(old_bank) == 1);
    CHECK(kind_at(new_bank) == 1);
    finish("bank-taken");
}

static void removing_a_link_leaves_its_target_intact(void) {
    fresh("unlink");
    make_folder(at("repos"));
    make_checkout(at("repos/" HEX), "kept");
    make_link(at("name"), at("repos/" HEX));
    record_link(at("name"), at("repos/" HEX));
    plan_start();
    plan_add(NAULA_REMOVE_LINK, at("name"), at("repos/" HEX), NULL);
    CHECK(apply_plan() == NAULA_OK);
    CHECK(kind_at(at("name")) == 0);
    CHECK(file_holds(at("repos/" HEX "/README.md"), "kept"));
    CHECK(file_holds(at("repos/" HEX "/.git/HEAD"), "ref: refs/heads/development\n"));
    CHECK(!is_recorded(at("name"), NULL));
    finish("unlink");
}

static void remove_link_refuses_a_folder(void) {
    fresh("unlink-folder");
    make_folder(at("name"));
    write_text(at("name/work.txt"), "work");
    make_folder(at("target"));
    record_link(at("name"), at("target"));
    plan_start();
    plan_add(NAULA_REMOVE_LINK, at("name"), at("target"), NULL);
    CHECK(refused_with(apply_plan(), "not-a-link"));
    CHECK(file_holds(at("name/work.txt"), "work"));
    finish("unlink-folder");
}

static void remove_link_refuses_a_link_to_another_target(void) {
    fresh("unlink-other");
    make_folder(at("a"));
    make_folder(at("b"));
    write_text(at("a/file.txt"), "a");
    make_link(at("name"), at("a"));
    record_link(at("name"), at("a"));
    plan_start();
    plan_add(NAULA_REMOVE_LINK, at("name"), at("b"), NULL);
    CHECK(refused_with(apply_plan(), "link-target-changed"));
    CHECK(links_to(at("name"), at("a")));

    unlink_link(at("name"));
    make_link(at("name"), at("b"));
    CHECK(refused_with(apply_plan(), "link-unrecorded"));
    CHECK(links_to(at("name"), at("b")));
    CHECK(file_holds(at("a/file.txt"), "a"));
    finish("unlink-other");
}

static void remove_clone_refuses_a_link(void) {
    fresh("clone-link");
    make_checkout(at("real"), "real clone");
    make_link(at("clone"), at("real"));
    plan_start();
    plan_add(NAULA_REMOVE_CLONE, at("clone"), NULL, NULL);
    CHECK(refused_with(apply_plan(), "clone-is-link"));
    CHECK(links_to(at("clone"), at("real")));
    CHECK(file_holds(at("real/README.md"), "real clone"));
    finish("clone-link");
}

static void removing_a_clone_never_follows_a_link_inside_it(void) {
    fresh("clone-inner");
    make_folder(at("sibling"));
    make_folder(at("sibling/deep"));
    write_text(at("sibling/one.txt"), "one");
    write_text(at("sibling/deep/two.txt"), "two");
    make_checkout(at("clone"), "archived");
    make_folder(at("clone/sub"));
    write_text(at("clone/sub/file.txt"), "inside");
    make_link(at("clone/inner"), at("sibling"));
    make_link(at("clone/sub/deeper"), at("sibling/deep"));
    char *sibling_before = snapshot("clone-inner/root/sibling");
    plan_start();
    plan_add(NAULA_REMOVE_CLONE, at("clone"), NULL, NULL);
    CHECK(apply_plan() == NAULA_OK);
    CHECK(kind_at(at("clone")) == 0);
    char *sibling_after = snapshot("clone-inner/root/sibling");
    CHECK(strcmp(sibling_before, sibling_after) == 0);
    CHECK(file_holds(at("sibling/one.txt"), "one"));
    CHECK(file_holds(at("sibling/deep/two.txt"), "two"));
    free(sibling_before);
    free(sibling_after);
    finish("clone-inner");
}

static void remove_clone_refuses_a_folder_that_is_no_checkout(void) {
    fresh("clone-plain");
    make_folder(at("plain"));
    write_text(at("plain/work.txt"), "work");
    plan_start();
    plan_add(NAULA_REMOVE_CLONE, at("plain"), NULL, NULL);
    CHECK(refused_with(apply_plan(), "not-a-checkout"));
    CHECK(file_holds(at("plain/work.txt"), "work"));
    finish("clone-plain");
}

static void a_step_through_a_link_below_the_root_is_refused(void) {
    fresh("through");
    make_folder(at("real"));
    make_folder(at("real/repos"));
    make_link(at("via"), at("real"));
    make_checkout(at("old"), "checkout");
    make_folder(at("repos"));
    make_checkout(at("repos/" HEX), "linked");

    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("via/new"), NULL, NULL);
    CHECK(refused_with(apply_plan(), "through-link"));
    CHECK(kind_at(at("real/new")) == 0);

    plan_start();
    plan_add(NAULA_WRITE_FILE, at("via/CLAUDE.md"), NULL, "@docs/RULES.md\n");
    CHECK(refused_with(apply_plan(), "through-link"));
    CHECK(kind_at(at("real/CLAUDE.md")) == 0);

    plan_start();
    plan_add(NAULA_MOVE_CHECKOUT, at("old"), at("via/repos/" HEX), NULL);
    CHECK(refused_with(apply_plan(), "through-link"));
    CHECK(kind_at(at("old")) == 1);
    CHECK(kind_at(at("real/repos/" HEX)) == 0);

    plan_start();
    plan_add(NAULA_MAKE_LINK, at("name"), at("via/repos"), NULL);
    CHECK(refused_with(apply_plan(), "through-link"));
    CHECK(kind_at(at("name")) == 0);

    plan_start();
    plan_add(NAULA_MAKE_LINK, at("via/name"), at("repos/" HEX), NULL);
    CHECK(refused_with(apply_plan(), "through-link"));
    CHECK(kind_at(at("real/name")) == 0);

    plan_start();
    plan_add(NAULA_REMOVE_CLONE, at("via/repos"), NULL, NULL);
    CHECK(refused_with(apply_plan(), "through-link"));
    CHECK(kind_at(at("real/repos")) == 1);
    finish("through");
}

static void make_link_replaces_only_a_link(void) {
    fresh("relink");
    make_folder(at("repos"));
    make_checkout(at("repos/" HEX), "new");
    make_checkout(at("repos/bbbbbbbbbbbb"), "old");

    make_folder(at("folder"));
    write_text(at("folder/work.txt"), "work");
    write_text(at("file"), "a file");
    make_link(at("foreign"), at("repos/bbbbbbbbbbbb"));
    make_link(at("recorded"), at("repos/bbbbbbbbbbbb"));
    record_link(at("recorded"), at("repos/bbbbbbbbbbbb"));

    plan_start();
    plan_add(NAULA_MAKE_LINK, at("folder"), at("repos/" HEX), NULL);
    CHECK(refused_with(apply_plan(), "path-occupied"));
    CHECK(file_holds(at("folder/work.txt"), "work"));

    plan_start();
    plan_add(NAULA_MAKE_LINK, at("file"), at("repos/" HEX), NULL);
    CHECK(refused_with(apply_plan(), "path-occupied"));
    CHECK(file_holds(at("file"), "a file"));

    plan_start();
    plan_add(NAULA_MAKE_LINK, at("foreign"), at("repos/" HEX), NULL);
    CHECK(refused_with(apply_plan(), "path-occupied"));
    CHECK(links_to(at("foreign"), at("repos/bbbbbbbbbbbb")));
    CHECK(!is_recorded(at("foreign"), NULL));

    plan_start();
    plan_add(NAULA_MAKE_LINK, at("recorded"), at("repos/" HEX), NULL);
    CHECK(apply_plan() == NAULA_OK);
    CHECK(links_to(at("recorded"), at("repos/" HEX)));
    CHECK(is_recorded(at("recorded"), at("repos/" HEX)));
    CHECK(file_holds(at("repos/bbbbbbbbbbbb/README.md"), "old"));
    finish("relink");
}

static void the_record_holds_a_link_only_once_it_is_made_and_until_it_is_removed(void) {
    fresh("record");
    make_folder(at("repos"));
    make_checkout(at("repos/" HEX), "checkout");
    plan_start();
    plan_add(NAULA_MAKE_LINK, at("name"), at("repos/" HEX), NULL);
    naula_apply_test_stop_at(2);
    CHECK(refused_with(apply_plan(), "test-stopped"));
    after_a_simulated_crash();
    CHECK(kind_at(at("name")) == 0);
    CHECK(!is_recorded(at("name"), NULL));

    fresh("record");
    make_folder(at("repos"));
    make_checkout(at("repos/" HEX), "checkout");
    naula_apply_test_stop_at(4);
    CHECK(refused_with(apply_plan(), "test-stopped"));
    after_a_simulated_crash();
    CHECK(links_to(at("name"), at("repos/" HEX)));
    CHECK(!is_recorded(at("name"), NULL));
    char *journal_file = unfinished_journal();
    CHECK(resume(journal_file) == NAULA_OK);
    naula_memory_release(journal_file);
    CHECK(is_recorded(at("name"), at("repos/" HEX)));

    plan_start();
    plan_add(NAULA_MAKE_LINK, at("missing"), at("repos/cccccccccccc"), NULL);
    CHECK(refused_with(apply_plan(), "target-missing"));
    CHECK(!is_recorded(at("missing"), NULL));

    plan_start();
    plan_add(NAULA_REMOVE_LINK, at("name"), at("repos/" HEX), NULL);
    CHECK(apply_plan() == NAULA_OK);
    CHECK(!is_recorded(at("name"), NULL));
    finish("record");
}

static void the_record_is_refused_when_it_or_the_root_is_a_link(void) {
    fresh("record-link");
    char real_root[PATH_SIZE];
    compose(real_root, root, NULL);
    char linked_root[PATH_SIZE];
    compose(linked_root, base, "/record-link/linked", NULL);
    make_link(linked_root, real_root);
    compose(root, linked_root, NULL);
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("org"), NULL, NULL);
    CHECK(refused_with(apply_plan(), "root-is-link"));
    naula_layout_links scanned;
    CHECK(naula_layout_links_scan(root, &scanned, &refusal) == NAULA_REFUSED);
    compose(root, real_root, NULL);
    CHECK(kind_at(at("org")) == 0);
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("org"), NULL, NULL);

    char elsewhere[PATH_SIZE];
    compose(elsewhere, base, "/record-link/elsewhere", NULL);
    make_folder(elsewhere);
    make_link(at(".naula-journal"), elsewhere);
    CHECK(refused_with(apply_plan(), "record-is-link"));
    CHECK(naula_layout_links_scan(root, &scanned, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "record-is-link", "a journal folder that is a link");
    unlink_link(at(".naula-journal"));

    make_folder(at(".naula-journal"));
    make_link(at(".naula-journal/links.json"), elsewhere);
    CHECK(refused_with(apply_plan(), "record-is-link"));
    CHECK(naula_layout_links_scan(root, &scanned, &refusal) == NAULA_REFUSED);
    unlink_link(at(".naula-journal/links.json"));
    CHECK(kind_at(at("org")) == 0);

    make_link(at(".naula.json"), elsewhere);
    naula_root_file file;
    CHECK(naula_root_file_read(root, &file, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "record-is-link", "a root file that is a link");
    finish("record-link");
}

static const naula_layout_link *scanned_at(const naula_layout_links *links, const char *path) {
    for (size_t index = 0; index < links->count; index++) {
        if (apply_same_path(links->links[index].path, path)) {
            return &links->links[index];
        }
    }
    return NULL;
}

static void a_scanned_link_is_made_by_naula_only_while_it_points_at_the_recorded_target(void) {
    fresh("scan");
    make_folder(at("repos"));
    make_checkout(at("repos/" HEX), "one");
    make_checkout(at("repos/bbbbbbbbbbbb"), "two");
    make_link(at("repos/" HEX "/inside"), at("repos/bbbbbbbbbbbb"));
    make_folder(at("tools"));
    plan_start();
    plan_add(NAULA_MAKE_LINK, at("tools/naula"), at("repos/" HEX), NULL);
    CHECK(apply_plan() == NAULA_OK);
    make_link(at("tools/hand"), at("repos/" HEX));

    naula_layout_links scanned;
    CHECK(naula_layout_links_scan(root, &scanned, &refusal) == NAULA_OK);
    const naula_layout_link *made = scanned_at(&scanned, at("tools/naula"));
    const naula_layout_link *hand = scanned_at(&scanned, at("tools/hand"));
    CHECK(made != NULL && made->made_by_naula == 1);
    CHECK(made != NULL && apply_same_target(made->target, at("repos/" HEX)));
    CHECK(hand != NULL && hand->made_by_naula == 0);
    CHECK(scanned_at(&scanned, at("repos/" HEX "/inside")) == NULL);
    CHECK(scanned.count == 2);
    naula_layout_links_release(&scanned);

    unlink_link(at("tools/naula"));
    make_link(at("tools/naula"), at("repos/bbbbbbbbbbbb"));
    CHECK(naula_layout_links_scan(root, &scanned, &refusal) == NAULA_OK);
    made = scanned_at(&scanned, at("tools/naula"));
    CHECK(made != NULL && made->made_by_naula == 0);
    naula_layout_links_release(&scanned);
    finish("scan");
}

static int a_journal_holds(const char *first, const char *second) {
    char folder[PATH_SIZE];
    compose(folder, root, "/.naula-journal", NULL);
    names found = list_names(folder);
    int holds = 0;
    for (size_t index = 0; index < found.count && !holds; index++) {
        char path[PATH_SIZE];
        compose(path, folder, "/", found.items[index], NULL);
        char *text = read_text(path);
        holds = text != NULL && strstr(text, first) != NULL && strstr(text, second) != NULL;
        free(text);
    }
    release_names(&found);
    return holds;
}

static void a_vanished_recorded_link_is_forgotten_by_the_scan(void) {
    fresh("vanished");
    make_folder(at("repos"));
    make_checkout(at("repos/" HEX), "one");
    make_folder(at("tools"));
    plan_start();
    plan_add(NAULA_MAKE_LINK, at("tools/naula"), at("repos/" HEX), NULL);
    CHECK(apply_plan() == NAULA_OK);
    unlink_link(at("tools/naula"));
    CHECK(is_recorded(at("tools/naula"), NULL));

    naula_layout_links scanned;
    CHECK(naula_layout_links_scan(root, &scanned, &refusal) == NAULA_OK);
    CHECK(scanned.count == 0);
    naula_layout_links_release(&scanned);
    CHECK(!is_recorded(at("tools/naula"), NULL));
    CHECK(a_journal_holds("\"state\":\"forgotten\"", at("tools/naula")));
    CHECK(kind_at(at(".naula-journal/lock")) == 0);

    make_link(at("tools/naula"), at("repos/" HEX));
    CHECK(naula_layout_links_scan(root, &scanned, &refusal) == NAULA_OK);
    const naula_layout_link *hand = scanned_at(&scanned, at("tools/naula"));
    CHECK(hand != NULL && hand->made_by_naula == 0);
    naula_layout_links_release(&scanned);
    finish("vanished");
}

static int hex_digit(char character) {
    return character >= 'a' ? character - 'a' + 10 : character - '0';
}

static char *previous_in_journal(const char *journal_file) {
    char *text = read_text(journal_file);
    char *found = NULL;
    for (char *line = text; line != NULL && *line != '\0' && found == NULL;) {
        char *end = strchr(line, '\n');
        size_t length = end == NULL ? strlen(line) : (size_t)(end - line);
        cJSON *json = NULL;
        const char *reason = NULL;
        if (naula_json_parse((const uint8_t *)line, length, &json, &reason) == NAULA_OK) {
            const cJSON *previous = cJSON_GetObjectItemCaseSensitive(json, "previous");
            if (cJSON_IsString(previous)) {
                size_t hex_length = strlen(previous->valuestring);
                found = malloc(hex_length / 2 + 1);
                for (size_t index = 0; index < hex_length / 2; index++) {
                    found[index] = (char)(hex_digit(previous->valuestring[index * 2]) * 16 +
                                          hex_digit(previous->valuestring[index * 2 + 1]));
                }
                found[hex_length / 2] = '\0';
            }
        }
        cJSON_Delete(json);
        line = end == NULL ? NULL : end + 1;
    }
    free(text);
    return found;
}

static char *only_journal(void) {
    char folder[PATH_SIZE];
    compose(folder, root, "/.naula-journal", NULL);
    names found = list_names(folder);
    char *path = NULL;
    for (size_t index = 0; index < found.count; index++) {
        size_t length = strlen(found.items[index]);
        if (length > 6 && strcmp(found.items[index] + length - 6, ".jsonl") == 0) {
            path = malloc(PATH_SIZE);
            compose(path, folder, "/", found.items[index], NULL);
        }
    }
    release_names(&found);
    return path;
}

static void an_overwritten_claude_file_keeps_its_old_content(void) {
    fresh("overwrite");
    write_text(at("CLAUDE.md"), "hand written\nrules kept here\n");
    plan_start();
    plan_add(NAULA_WRITE_FILE, at("CLAUDE.md"), NULL, "@docs/RULES.md\n");
    CHECK(apply_plan() == NAULA_OK);
    CHECK(file_holds(at("CLAUDE.md"), "@docs/RULES.md\n"));
    char *journal_file = only_journal();
    char *previous = journal_file == NULL ? NULL : previous_in_journal(journal_file);
    CHECK(previous != NULL && strcmp(previous, "hand written\nrules kept here\n") == 0);
    free(previous);
    free(journal_file);
    finish("overwrite");
}

static void a_write_through_a_link_at_the_file_is_refused(void) {
    fresh("file-link");
    make_folder(at("elsewhere"));
    make_link(at("CLAUDE.md"), at("elsewhere"));
    plan_start();
    plan_add(NAULA_WRITE_FILE, at("CLAUDE.md"), NULL, "@docs/RULES.md\n");
    CHECK(refused_with(apply_plan(), "through-link"));
    CHECK(links_to(at("CLAUDE.md"), at("elsewhere")));
    finish("file-link");
}

static void a_step_outside_the_root_or_on_naulas_files_is_refused(void) {
    fresh("outside");
    char outside[PATH_SIZE];
    compose(outside, base, "/outside/elsewhere", NULL);
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, outside, NULL, NULL);
    CHECK(refused_with(apply_plan(), "apply-input-invalid"));
    CHECK(kind_at(outside) == 0);
    plan_start();
    plan_add(NAULA_WRITE_FILE, at(".naula-journal/links.json"), NULL, "{\"links\":[]}");
    CHECK(refused_with(apply_plan(), "apply-input-invalid"));
    plan_start();
    plan_add(NAULA_REMOVE_CLONE, root, NULL, NULL);
    CHECK(refused_with(apply_plan(), "apply-input-invalid"));
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("a/../b"), NULL, NULL);
    CHECK(refused_with(apply_plan(), "apply-input-invalid"));
    CHECK(kind_at(at(".naula-journal")) == 0);
    finish("outside");
}

static size_t reported_steps;

static void count_reports(void *context, const naula_layout_step *step, naula_status status) {
    (void)context;
    (void)step;
    reported_steps += status == NAULA_OK;
}

static void dry_run_changes_nothing(void) {
    fresh("dry");
    make_checkout(at("old"), "checkout");
    write_text(at("CLAUDE.md"), "old");
    char *before = snapshot("dry");
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("repos"), NULL, NULL);
    plan_add(NAULA_MOVE_CHECKOUT, at("old"), at("repos/" HEX), NULL);
    plan_add(NAULA_WRITE_FILE, at("CLAUDE.md"), NULL, "new");
    plan_add(NAULA_MAKE_LINK, at("docs"), at("repos/" HEX), NULL);
    plan_add(NAULA_REMOVE_CLONE, at("old"), NULL, NULL);
    naula_apply_options options = options_for(banks);
    options.dry_run = 1;
    options.report = count_reports;
    reported_steps = 0;
    CHECK(naula_layout_apply(&plans.plan, &options, &refusal) == NAULA_OK);
    CHECK(reported_steps == 5);
    char *after = snapshot("dry");
    CHECK(strcmp(before, after) == 0);
    CHECK(kind_at(at(".naula-journal")) == 0);
    free(before);
    free(after);
    finish("dry");
}

static naula_place place(uint64_t number) {
    naula_place made;
    memset(&made, 0, sizeof made);
    made.size = sizeof made;
    made.kind = NAULA_HOME_PROVIDER;
    made.provider = "github";
    made.number = number;
    return made;
}

static void root_file_round_trips(void) {
    fresh("root-file");
    naula_place top_place = place(1472199689);
    naula_place added_places[] = {place(7), place(8)};
    naula_place extra_place = place(42);
    naula_reference added[2];
    memset(added, 0, sizeof added);
    added[0].size = sizeof added[0];
    added[0].id = "git:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    added[0].at = &added_places[0];
    added[0].at_count = 2;
    added[1].size = sizeof added[1];
    added[1].url = "https://example.com/org/docs.git";
    naula_reference extra[2];
    memset(extra, 0, sizeof extra);
    extra[0].size = sizeof extra[0];
    extra[0].id = "git:cccccccccccccccccccccccccccccccccccccccc";
    extra[0].at = &extra_place;
    extra[0].at_count = 1;
    extra[1].size = sizeof extra[1];
    extra[1].url = "https://example.com/fork.git";
    char *dropped[] = {"pinaxis"};
    char *extra_orgs[] = {"slimefun", NULL};
    naula_root_file file;
    memset(&file, 0, sizeof file);
    file.size = sizeof file;
    file.top.size = sizeof file.top;
    file.top.id = "git:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    file.top.at = &top_place;
    file.top.at_count = 1;
    file.added = added;
    file.added_count = 2;
    file.dropped = dropped;
    file.dropped_count = 1;
    file.extra = extra;
    file.extra_count = 2;
    file.extra_orgs = extra_orgs;

    naula_root_file read;
    CHECK(naula_root_file_read(root, &read, &refusal) == NAULA_NOT_FOUND);
    CHECK(naula_root_file_write(root, &file, &refusal) == NAULA_OK);
    char *first = read_text(at(".naula.json"));
    CHECK(naula_root_file_read(root, &read, &refusal) == NAULA_OK);
    check_text(read.top.id, file.top.id, "top id");
    CHECK(read.top.at_count == 1 && read.top.at[0].number == 1472199689);
    CHECK(read.added_count == 2);
    CHECK(read.added_count == 2 && read.added[0].at_count == 2 && read.added[0].at[1].number == 8);
    CHECK(read.added_count == 2 && read.added[1].url != NULL &&
          strcmp(read.added[1].url, "https://example.com/org/docs.git") == 0);
    CHECK(read.dropped_count == 1 && strcmp(read.dropped[0], "pinaxis") == 0);
    CHECK(read.extra_count == 2 && read.extra_orgs[0] != NULL && strcmp(read.extra_orgs[0], "slimefun") == 0);
    CHECK(read.extra_count == 2 && read.extra_orgs[1] == NULL);
    CHECK(naula_root_file_write(root, &read, &refusal) == NAULA_OK);
    char *second = read_text(at(".naula.json"));
    CHECK(first != NULL && second != NULL && strcmp(first, second) == 0);
    naula_root_file_release(&read);
    free(first);
    free(second);

    write_text(at(".naula.json"), "{\"top\":{\"id\":\"git:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"at\":[\"github:1\"]},"
                                  "\"claims\":[]}");
    CHECK(naula_root_file_read(root, &read, &refusal) == NAULA_REFUSED);
    finish("root-file");
}

/** Everything a resumed run must converge on: a link removed, a clone removed, a move with its bank, a file, links. */
static void lay_out_the_crash_scene(void) {
    fresh("crash");
    make_checkout(at("old"), "moved checkout");
    make_checkout(at("archived"), "archived clone");
    make_folder(at("target-x"));
    make_link(at("stale"), at("target-x"));
    record_link(at("stale"), at("target-x"));
    write_text(at("CLAUDE.md"), "hand written");
    char bank[PATH_SIZE];
    compose(bank, bank_of(at("old")), NULL);
    make_folder(bank);
    char memory[PATH_SIZE];
    compose(memory, bank, "/MEMORY.md", NULL);
    write_text(memory, "remembered");
    make_link(at("other"), at("target-x"));
    record_link(at("other"), at("target-x"));
    plan_start();
    plan_add(NAULA_REMOVE_LINK, at("stale"), at("target-x"), NULL);
    plan_add(NAULA_MAKE_FOLDER, at("org"), NULL, NULL);
    plan_add(NAULA_MAKE_FOLDER, at("org/repos"), NULL, NULL);
    plan_add(NAULA_MOVE_CHECKOUT, at("old"), at("org/repos/" HEX), NULL);
    plan_add(NAULA_REMOVE_CLONE, at("archived"), NULL, NULL);
    plan_add(NAULA_WRITE_FILE, at("CLAUDE.md"), NULL, "@docs/RULES.md\n");
    plan_add(NAULA_MAKE_LINK, at("org/name"), at("org/repos/" HEX), NULL);
    plan_add(NAULA_MAKE_LINK, at("other"), at("org/repos/" HEX), NULL);
    plan_add(NAULA_REPORT, at("org/x"), NULL, "a note");
}

static void a_resume_after_a_crash_at_every_boundary_converges(void) {
    lay_out_the_crash_scene();
    naula_apply_test_stop_at(0);
    CHECK(apply_plan() == NAULA_OK);
    unsigned long boundaries = naula_apply_test_boundaries();
    char *expected = snapshot("crash");
    CHECK(strstr(expected, "link /root/org/name") != NULL);
    CHECK(strstr(expected, "/root/stale") == NULL);
    CHECK(strstr(expected, "/root/archived") == NULL);
    CHECK(boundaries > 20);
    for (unsigned long stop = 1; stop <= boundaries; stop++) {
        lay_out_the_crash_scene();
        naula_apply_test_stop_at(stop);
        naula_status stopped = apply_plan();
        after_a_simulated_crash();
        if (!refused_with(stopped, "test-stopped")) {
            check_failures++;
            continue;
        }
        char *journal_file = unfinished_journal();
        if (journal_file != NULL) {
            naula_status resumed = resume(journal_file);
            if (resumed != NAULA_OK) {
                fprintf(stderr, "resume after boundary %lu: %s %s\n", stop, refusal.code, refusal.detail);
                check_failures++;
            }
            naula_memory_release(journal_file);
        }
        char *converged = snapshot("crash");
        if (strcmp(converged, expected) != 0) {
            fprintf(stderr, "after a crash at boundary %lu the disk differs:\n%s\nexpected:\n%s\n", stop, converged,
                    expected);
            check_failures++;
        }
        CHECK(unfinished_journal() == NULL);
        free(converged);
    }
    free(expected);
    finish("crash");
}

/**
 * Fails each allocation of a run in turn. A failed run leaves no block live and the disk either untouched or
 * journalled, so the next run, a resume when the journal is unfinished, converges on the uninterrupted layout. An
 * allocation failing while the lock is released leaves the lock naming this process, which the next run reports.
 */
static void survives_every_allocation_failure(void) {
    lay_out_the_crash_scene();
    naula_memory_fail_at(0);
    CHECK(apply_plan() == NAULA_OK);
    uint64_t attempts = naula_memory_attempts();
    char *expected = snapshot("crash");
    CHECK(attempts > 100);
    for (uint64_t failing = 1; failing <= attempts; failing++) {
        lay_out_the_crash_scene();
        uint64_t live_before = naula_memory_live_blocks();
        naula_memory_fail_at(failing);
        naula_status status = apply_plan();
        naula_memory_fail_at(0);
        if (naula_memory_live_blocks() != live_before) {
            fprintf(stderr, "allocation %llu failed: %llu blocks left live\n", (unsigned long long)failing,
                    (unsigned long long)(naula_memory_live_blocks() - live_before));
            check_failures++;
        }
        if (status != NAULA_OUT_OF_MEMORY) {
            fprintf(stderr, "allocation %llu failed: status %d %s %s\n", (unsigned long long)failing, (int)status,
                    refusal.code, refusal.detail);
            check_failures++;
            continue;
        }
        if (kind_at(at(".naula-journal/lock")) != 0) {
            char held[32];
            snprintf(held, sizeof held, "%lu\n", own_process_id());
            CHECK(file_holds(at(".naula-journal/lock"), held));
            remove_tree(at(".naula-journal/lock"));
        }
        char *journal_file = unfinished_journal();
        naula_status converged = journal_file != NULL ? resume(journal_file) : apply_plan();
        naula_memory_release(journal_file);
        if (converged != NAULA_OK) {
            fprintf(stderr, "after allocation %llu failed the next run answered %d %s %s\n",
                    (unsigned long long)failing, (int)converged, refusal.code, refusal.detail);
            check_failures++;
        }
        char *state = snapshot("crash");
        if (strcmp(state, expected) != 0) {
            fprintf(stderr, "after allocation %llu failed the disk differs:\n%s\n", (unsigned long long)failing,
                    state);
            check_failures++;
        }
        free(state);
    }
    free(expected);
    finish("crash");
}

static void a_journal_elsewhere_or_for_another_root_is_refused(void) {
    fresh("journal-place");
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("org"), NULL, NULL);
    naula_apply_test_stop_at(2);
    CHECK(refused_with(apply_plan(), "test-stopped"));
    after_a_simulated_crash();
    char *journal_file = unfinished_journal();
    char *text = journal_file == NULL ? NULL : read_text(journal_file);
    char copy[PATH_SIZE];
    compose(copy, base, "/journal-place/copy.jsonl", NULL);
    if (text != NULL) {
        write_text(copy, text);
    }
    CHECK(refused_with(resume(copy), "journal-elsewhere"));
    CHECK(kind_at(at("org")) == 0);
    free(text);
    naula_memory_release(journal_file);
    finish("journal-place");
}

static void a_root_another_naula_holds_is_refused(void) {
    fresh("locked");
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("org"), NULL, NULL);
    naula_apply_test_stop_at(2);
    CHECK(refused_with(apply_plan(), "test-stopped"));
    naula_apply_test_stop_at(0);
    char *journal_file = unfinished_journal();
    CHECK(journal_file != NULL);
    char held[32];
    snprintf(held, sizeof held, "%lu\n", own_process_id());
    CHECK(file_holds(at(".naula-journal/lock"), held));
    CHECK(refused_with(apply_plan(), "root-locked"));
    CHECK(refused_with(resume(journal_file), "root-locked"));
    CHECK(kind_at(at("org")) == 0);
    CHECK(file_holds(at(".naula-journal/lock"), held));
    remove_tree(at(".naula-journal/lock"));
    CHECK(resume(journal_file) == NAULA_OK);
    CHECK(kind_at(at("org")) == 1);
    CHECK(kind_at(at(".naula-journal/lock")) == 0);
    naula_memory_release(journal_file);

    plan_start();
    plan_add(NAULA_MAKE_LINK, at("name"), at("missing"), NULL);
    CHECK(refused_with(apply_plan(), "target-missing"));
    CHECK(kind_at(at(".naula-journal/lock")) == 0);
    finish("locked");
}

static void a_stale_lock_is_reported_and_never_taken(void) {
    fresh("stale-lock");
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("org"), NULL, NULL);
    naula_apply_test_stop_at(2);
    CHECK(refused_with(apply_plan(), "test-stopped"));
    after_a_simulated_crash();
    char *journal_file = unfinished_journal();
    write_lock(GONE_PROCESS_ID);
    CHECK(refused_with(apply_plan(), "stale-lock"));
    CHECK(strstr(refusal.detail, "2147480000") != NULL);
    CHECK(refusal.fix != NULL && strstr(refusal.fix, "remove") != NULL);
    CHECK(refused_with(resume(journal_file), "stale-lock"));
    CHECK(file_holds(at(".naula-journal/lock"), "2147480000\n"));
    CHECK(kind_at(at("org")) == 0);

    remove_tree(at(".naula-journal/lock"));
    make_folder(at("elsewhere"));
    make_link(at(".naula-journal/lock"), at("elsewhere"));
    CHECK(refused_with(apply_plan(), "record-is-link"));
    CHECK(refused_with(resume(journal_file), "record-is-link"));
    CHECK(kind_at(at("org")) == 0);
    naula_memory_release(journal_file);
    finish("stale-lock");
}

static void a_second_apply_waits_for_an_unfinished_journal(void) {
    fresh("unfinished");
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("org"), NULL, NULL);
    naula_apply_test_stop_at(3);
    CHECK(refused_with(apply_plan(), "test-stopped"));
    after_a_simulated_crash();
    char *journal_file = unfinished_journal();
    CHECK(journal_file != NULL);
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("other"), NULL, NULL);
    CHECK(refused_with(apply_plan(), "journal-unfinished"));
    CHECK(refusal.fix != NULL && strstr(refusal.fix, "naula_journal_resume") != NULL);
    CHECK(journal_file != NULL && strstr(refusal.detail, journal_file) != NULL);
    CHECK(kind_at(at("other")) == 0);
    CHECK(kind_at(at(".naula-journal/lock")) == 0);
    CHECK(resume(journal_file) == NAULA_OK);
    CHECK(apply_plan() == NAULA_OK);
    CHECK(kind_at(at("other")) == 1);
    naula_memory_release(journal_file);
    finish("unfinished");
}

static void a_run_refused_before_any_change_leaves_nothing_to_resume(void) {
    fresh("stopped-clean");
    make_folder(at("repos"));
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("org"), NULL, NULL);
    plan_add(NAULA_MAKE_LINK, at("name"), at("repos/" HEX), NULL);
    CHECK(refused_with(apply_plan(), "target-missing"));
    CHECK(unfinished_journal() == NULL);
    make_checkout(at("repos/" HEX), "now there");
    CHECK(apply_plan() == NAULA_OK);
    CHECK(links_to(at("name"), at("repos/" HEX)));
    finish("stopped-clean");
}

static void a_run_refused_halfway_through_a_step_must_be_resumed(void) {
#ifdef _WIN32
    fresh("stopped-halfway");
    make_checkout(at("archived"), "archived");
    make_folder(at("archived/a"));
    write_text(at("archived/a/first.txt"), "first");
    make_folder(at("archived/z"));
    write_text(at("archived/z/held.txt"), "held");
    HANDLE held = CreateFileA(at("archived/z/held.txt"), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(held != INVALID_HANDLE_VALUE);
    plan_start();
    plan_add(NAULA_REMOVE_CLONE, at("archived"), NULL, NULL);
    CHECK(refused_with(apply_plan(), "folder-held"));
    CHECK(strstr(refusal.detail, "system error 32)") != NULL);
    CHECK(kind_at(at("archived/a")) == 0);
    char *journal_file = unfinished_journal();
    CHECK(journal_file != NULL);
    CHECK(refused_with(apply_plan(), "journal-unfinished"));
    CloseHandle(held);
    CHECK(resume(journal_file) == NAULA_OK);
    CHECK(kind_at(at("archived")) == 0);
    naula_memory_release(journal_file);
    finish("stopped-halfway");
#else
    printf("a_run_refused_halfway_through_a_step_must_be_resumed: skipped, an open file is removed on POSIX\n");
#endif
}

typedef struct swap_race {
    char watched[PATH_SIZE];
    char swapped[PATH_SIZE];
    char moved_to[PATH_SIZE];
    char outside[PATH_SIZE];
    volatile int removal_returned;
    int swapped_in;
} swap_race;

/** Waits for the removal to take its first file, then moves a folder it has not reached yet and links it outside. */
static void swap_once_removal_starts(swap_race *race) {
    while (kind_at(race->watched) != 0 && !race->removal_returned) {
    }
#ifdef _WIN32
    int moved = MoveFileA(race->swapped, race->moved_to) != 0;
#else
    int moved = rename(race->swapped, race->moved_to) == 0;
#endif
    if (moved) {
        unsigned long error = 0;
        race->swapped_in = disk_link_make(race->swapped, race->outside, &error) == NAULA_OK;
    }
}

#ifdef _WIN32
static DWORD WINAPI swap_thread(LPVOID context) {
    swap_once_removal_starts(context);
    return 0;
}
#else
static void *swap_thread(void *context) {
    swap_once_removal_starts(context);
    return NULL;
}
#endif

static naula_status remove_clone_while_racing(swap_race *race) {
    plan_start();
    plan_add(NAULA_REMOVE_CLONE, at("victim"), NULL, NULL);
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, swap_thread, race, 0, NULL);
    naula_status status = apply_plan();
    race->removal_returned = 1;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_t thread;
    pthread_create(&thread, NULL, swap_thread, race);
    naula_status status = apply_plan();
    race->removal_returned = 1;
    pthread_join(thread, NULL);
#endif
    return status;
}

#define SWAP_ROUNDS 8
#define SWAP_FILES 2000

static void lay_out_a_victim(swap_race *race, const char *outside) {
    fresh("swap");
    make_checkout(at("victim"), "victim");
    make_folder(at("victim/a"));
    for (int index = 0; index < SWAP_FILES; index++) {
        char name[64];
        snprintf(name, sizeof name, "victim/a/f%05d.txt", index);
        write_text(at(name), "x");
    }
    make_folder(at("victim/z"));
    write_text(at("victim/z/z.txt"), "z");
    memset(race, 0, sizeof *race);
    compose(race->watched, at("victim/a/f00000.txt"), NULL);
    compose(race->swapped, at("victim/z"), NULL);
    compose(race->moved_to, base, "/swap/z-moved", NULL);
    compose(race->outside, outside, NULL);
}

/**
 * Another process renames a folder of the clone away mid-removal and puts a link to a folder outside the root in its
 * place. The outside folder must come through byte for byte, whether the removal then finishes or refuses.
 */
static void removing_a_clone_never_follows_a_folder_swapped_for_a_link_while_it_runs(void) {
    char outside[PATH_SIZE];
    char keep[PATH_SIZE];
    compose(outside, base, "/swap-outside", NULL);
    compose(keep, outside, "/keep.txt", NULL);
    remove_tree(outside);
    make_folder(outside);
    write_text(keep, "must survive");
    char *before = snapshot("swap-outside");
    int swaps = 0;
    for (int round = 0; round < SWAP_ROUNDS; round++) {
        swap_race race;
        lay_out_a_victim(&race, outside);
        naula_status status = remove_clone_while_racing(&race);
        CHECK(status == NAULA_OK || status == NAULA_REFUSED);
        char *after = snapshot("swap-outside");
        if (strcmp(before, after) != 0) {
            fprintf(stderr, "round %d: the folder outside the root changed:\n%s\n", round, after);
            check_failures++;
        }
        free(after);
        swaps += race.swapped_in;
        if (kind_at(at("victim/z")) == 3) {
            unlink_link(at("victim/z"));
        }
        finish("swap");
    }
    printf("removing_a_clone_never_follows_a_folder_swapped_for_a_link_while_it_runs: %d of %d rounds swapped\n",
           swaps, SWAP_ROUNDS);
#ifdef _WIN32
    CHECK(swaps > 0);
#endif
    free(before);
    remove_tree(outside);
}

static void a_resumed_move_refused_after_its_checkout_moved_stays_unfinished(void) {
    fresh("resume-moved");
    make_folder(at("repos"));
    make_checkout(at("old"), "checkout");
    char old_bank[PATH_SIZE];
    char new_bank[PATH_SIZE];
    compose(old_bank, bank_of(at("old")), NULL);
    compose(new_bank, bank_of(at("repos/" HEX)), NULL);
    make_folder(old_bank);
    plan_start();
    plan_add(NAULA_MOVE_CHECKOUT, at("old"), at("repos/" HEX), NULL);
    naula_apply_test_stop_at(3);
    CHECK(refused_with(apply_plan(), "test-stopped"));
    after_a_simulated_crash();
    CHECK(kind_at(at("old")) == 0);
    CHECK(kind_at(at("repos/" HEX)) == 1);
    CHECK(kind_at(old_bank) == 1);
    make_folder(new_bank);
    char *journal_file = unfinished_journal();
    CHECK(journal_file != NULL);
    CHECK(refused_with(resume(journal_file), "memory-bank-exists"));
    char *still = unfinished_journal();
    CHECK(still != NULL);
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("other"), NULL, NULL);
    CHECK(refused_with(apply_plan(), "journal-unfinished"));
    remove_tree(new_bank);
    CHECK(journal_file != NULL && resume(journal_file) == NAULA_OK);
    CHECK(kind_at(old_bank) == 0);
    CHECK(kind_at(new_bank) == 1);
    naula_memory_release(journal_file);
    naula_memory_release(still);
    finish("resume-moved");
}

static void a_resumed_link_replacement_refused_after_the_old_link_went_stays_unfinished(void) {
    fresh("resume-relink");
    make_folder(at("repos"));
    make_checkout(at("repos/old"), "old");
    make_checkout(at("repos/new"), "new");
    make_link(at("name"), at("repos/old"));
    record_link(at("name"), at("repos/old"));
    plan_start();
    plan_add(NAULA_MAKE_LINK, at("name"), at("repos/new"), NULL);
    naula_apply_test_stop_at(3);
    CHECK(refused_with(apply_plan(), "test-stopped"));
    after_a_simulated_crash();
    CHECK(kind_at(at("name")) == 0);
    remove_tree(at("repos/new"));
    char *journal_file = unfinished_journal();
    CHECK(journal_file != NULL);
    CHECK(refused_with(resume(journal_file), "target-missing"));
    char *still = unfinished_journal();
    CHECK(still != NULL);
    naula_memory_release(journal_file);
    naula_memory_release(still);
    finish("resume-relink");
}

static void a_stopped_journal_is_never_resumed(void) {
    fresh("resume-stopped");
    make_folder(at("repos"));
    make_checkout(at("keep"), "work the newer layout keeps");
    plan_start();
    plan_add(NAULA_MAKE_LINK, at("name"), at("repos/" HEX), NULL);
    plan_add(NAULA_REMOVE_CLONE, at("keep"), NULL, NULL);
    CHECK(refused_with(apply_plan(), "target-missing"));
    CHECK(unfinished_journal() == NULL);
    char *journal_file = only_journal();
    CHECK(journal_file != NULL);
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("org"), NULL, NULL);
    CHECK(apply_plan() == NAULA_OK);
    make_checkout(at("repos/" HEX), "later");
    CHECK(journal_file != NULL && refused_with(resume(journal_file), "journal-stopped"));
    CHECK(file_holds(at("keep/README.md"), "work the newer layout keeps"));
    CHECK(kind_at(at("name")) == 0);
    free(journal_file);
    finish("resume-stopped");
}

static void a_journal_a_newer_one_follows_is_never_resumed(void) {
    fresh("resume-superseded");
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("org"), NULL, NULL);
    naula_apply_test_stop_at(2);
    CHECK(refused_with(apply_plan(), "test-stopped"));
    after_a_simulated_crash();
    char *journal_file = unfinished_journal();
    CHECK(journal_file != NULL);
    record_link(at("gone"), at("org"));
    naula_layout_links scanned;
    CHECK(refused_with(naula_layout_links_scan(root, &scanned, &refusal), "journal-unfinished"));
    naula_layout_links_release(&scanned);
    CHECK(is_recorded(at("gone"), NULL));
    char newer[PATH_SIZE * 2];
    snprintf(newer, sizeof newer, "{\"journal\":1,\"root\":\"%s\",\"steps\":0}\n{\"state\":\"finished\"}\n", root);
    write_text(at(".naula-journal/9999999999-20261127T000000Z.jsonl"), newer);
    CHECK(journal_file != NULL && refused_with(resume(journal_file), "journal-superseded"));
    CHECK(strstr(refusal.detail, "9999999999-20261127T000000Z.jsonl") != NULL);
    CHECK(kind_at(at("org")) == 0);
    naula_memory_release(journal_file);
    finish("resume-superseded");
}

static void a_journal_torn_inside_its_opening_append_reads_as_stopped(void) {
    fresh("torn-opening");
    make_folder(at(".naula-journal"));
    char text[PATH_SIZE * 2];
    snprintf(text, sizeof text,
             "{\"journal\":1,\"root\":\"%s\",\"steps\":2}\n{\"step\":0,\"state\":\"planned\",\"kind\":1,\"path\":\"%s\"}"
             "\n{\"step\":1,\"sta",
             root, at("org"));
    write_text(at(".naula-journal/0000000001-20260101T000000Z.jsonl"), text);
    CHECK(unfinished_journal() == NULL);
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("org"), NULL, NULL);
    CHECK(apply_plan() == NAULA_OK);
    CHECK(kind_at(at("org")) == 1);
    CHECK(refused_with(resume(at(".naula-journal/0000000001-20260101T000000Z.jsonl")), "journal-stopped"));
    finish("torn-opening");
}

static void set_read_only_folder(const char *path, int read_only) {
#ifdef _WIN32
    SetFileAttributesA(path, read_only ? FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_DIRECTORY);
#else
    chmod(path, read_only ? 0555 : 0755);
#endif
}

static void a_clone_holding_a_read_only_folder_is_removed(void) {
    fresh("read-only-folder");
    make_checkout(at("clone"), "clone");
    make_folder(at("clone/ro"));
    write_text(at("clone/ro/inside.txt"), "inside");
    set_read_only_folder(at("clone/ro"), 1);
    plan_start();
    plan_add(NAULA_REMOVE_CLONE, at("clone"), NULL, NULL);
    naula_status status = apply_plan();
    if (status != NAULA_OK) {
        fprintf(stderr, "removing a clone with a read-only folder: %s %s\n", refusal.code, refusal.detail);
    }
    CHECK(status == NAULA_OK);
    CHECK(kind_at(at("clone")) == 0);
    if (kind_at(at("clone/ro")) == 1) {
        set_read_only_folder(at("clone/ro"), 0);
    }
    finish("read-only-folder");
}

static void removing_a_clone_leaves_a_hard_linked_read_only_file_read_only(void) {
#ifdef _WIN32
    fresh("hard-link");
    char outside[PATH_SIZE];
    compose(outside, base, "/hard-link/object", NULL);
    write_text(outside, "object");
    SetFileAttributesA(outside, FILE_ATTRIBUTE_READONLY);
    make_checkout(at("clone"), "clone");
    make_folder(at("clone/.git/objects"));
    CHECK(CreateHardLinkA(at("clone/.git/objects/object"), outside, NULL));
    plan_start();
    plan_add(NAULA_REMOVE_CLONE, at("clone"), NULL, NULL);
    CHECK(apply_plan() == NAULA_OK);
    CHECK(kind_at(at("clone")) == 0);
    CHECK(GetFileAttributesA(outside) & FILE_ATTRIBUTE_READONLY);
    CHECK(file_holds(outside, "object"));
    SetFileAttributesA(outside, FILE_ATTRIBUTE_NORMAL);
    finish("hard-link");
#else
    printf("removing_a_clone_leaves_a_hard_linked_read_only_file_read_only: skipped, POSIX unlinks without touching "
           "the mode\n");
#endif
}

#ifdef _WIN32
static void refused_as_input(const char *relative) {
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at(relative), NULL, NULL);
    if (!refused_with(apply_plan(), "apply-input-invalid")) {
        fprintf(stderr, "  for %s\n", relative);
    }
}
#endif

/** Windows opens these as naula's own files; elsewhere they are ordinary names. */
static void a_step_naming_naulas_files_by_an_alias_is_refused_on_windows(void) {
    fresh("alias");
    make_folder(at(".naula-journal"));
#ifdef _WIN32
    plan_start();
    plan_add(NAULA_WRITE_FILE, at(".naula-journal./links.json"), NULL, "{\"links\":[]}");
    CHECK(refused_with(apply_plan(), "apply-input-invalid"));
    CHECK(kind_at(at(".naula-journal/links.json")) == 0);
    refused_as_input(".naula-journal /org");
    refused_as_input(".naula.json.");
    refused_as_input("NAULA-~1/org");
    refused_as_input("naula~2.jso");
    refused_as_input("NAU~1");
    refused_as_input("NA1B2C~1");
#else
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("NAULA-~1"), NULL, NULL);
    plan_add(NAULA_MAKE_FOLDER, at(".naula-journal."), NULL, NULL);
    CHECK(apply_plan() == NAULA_OK);
    CHECK(kind_at(at("NAULA-~1")) == 1);
#endif
    finish("alias");
}

/** A tilde, a trailing dot or space is an ordinary name everywhere when it cannot stand for naula's own files. */
static void a_name_with_a_tilde_moves_on_every_platform(void) {
    fresh("tilde");
    make_folder(at("repos"));
    make_checkout(at("work~old"), "kept");
    make_folder(at("notes~1"));
    plan_start();
    plan_add(NAULA_MOVE_CHECKOUT, at("work~old"), at("repos/" HEX), NULL);
    plan_add(NAULA_MAKE_LINK, at("repos/a~b"), at("repos/" HEX), NULL);
    plan_add(NAULA_MAKE_FOLDER, at("notes~1/inner"), NULL, NULL);
    naula_status status = apply_plan();
    if (status != NAULA_OK) {
        fprintf(stderr, "a tilde: %s %s\n", refusal.code, refusal.detail);
    }
    CHECK(status == NAULA_OK);
    CHECK(file_holds(at("repos/" HEX "/README.md"), "kept"));
    CHECK(links_to(at("repos/a~b"), at("repos/" HEX)));
    CHECK(kind_at(at("notes~1/inner")) == 1);
    unlink_link(at("repos/a~b"));
    finish("tilde");
}

/**
 * The clock went back: a finished journal carries a stamp far in the future, and a later run crashes. Its journal
 * takes the next sequence, so it is the one unfinished and the one resumed.
 */
static void a_journal_stamped_in_the_future_never_blocks_a_resume(void) {
    fresh("clock-back");
    make_folder(at(".naula-journal"));
    char future[PATH_SIZE * 2];
    snprintf(future, sizeof future, "{\"journal\":1,\"root\":\"%s\",\"steps\":0}\n{\"state\":\"finished\"}\n", root);
    write_text(at(".naula-journal/0000000001-99991231T235959Z.jsonl"), future);
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("org"), NULL, NULL);
    naula_apply_test_stop_at(2);
    CHECK(refused_with(apply_plan(), "test-stopped"));
    after_a_simulated_crash();
    char *journal_file = unfinished_journal();
    CHECK(journal_file != NULL && strstr(journal_file, "0000000002-") != NULL);
    CHECK(journal_file != NULL && resume(journal_file) == NAULA_OK);
    CHECK(kind_at(at("org")) == 1);
    CHECK(unfinished_journal() == NULL);
    naula_memory_release(journal_file);
    finish("clock-back");
}

static void write_finished_journal(const char *name) {
    char text[PATH_SIZE * 2];
    char path[PATH_SIZE];
    snprintf(text, sizeof text, "{\"journal\":1,\"root\":\"%s\",\"steps\":0}\n{\"state\":\"finished\"}\n", root);
    compose(path, at(".naula-journal/"), name, NULL);
    make_folder(at(".naula-journal"));
    write_text(path, text);
}

static size_t journal_count(void) {
    names found = list_names(at(".naula-journal"));
    size_t count = 0;
    for (size_t index = 0; index < found.count; index++) {
        count += strstr(found.items[index], ".jsonl") != NULL;
    }
    release_names(&found);
    return count;
}

/**
 * A journal holding the largest sequence a name can carry leaves none for the next run, which is refused rather than
 * named past what any listing reads; and a longer sequence, which no listing can order, refuses every run and every
 * search for an unfinished journal until it is moved.
 */
static void a_journal_sequence_past_the_largest_refuses_the_run(void) {
    fresh("sequence-edge");
    write_finished_journal("9999999999999999999-20260101T000000Z.jsonl");
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("second"), NULL, NULL);
    CHECK(refused_with(apply_plan(), "journal-sequence-exhausted"));
    CHECK(strstr(refusal.detail, "9999999999999999999-20260101T000000Z.jsonl") != NULL);
    CHECK(kind_at(at("second")) == 0);
    CHECK(journal_count() == 1);
    CHECK(kind_at(at(".naula-journal/lock")) == 0);
    remove_tree(at(".naula-journal/9999999999999999999-20260101T000000Z.jsonl"));
    write_finished_journal("10000000000000000000-20260101T000000Z.jsonl");
    CHECK(refused_with(apply_plan(), "journal-sequence-invalid"));
    CHECK(kind_at(at("second")) == 0);
    char *journal_file = NULL;
    CHECK(refused_with(naula_journal_unfinished(root, &journal_file, &refusal), "journal-sequence-invalid"));
    CHECK(journal_file == NULL);
    remove_tree(at(".naula-journal/10000000000000000000-20260101T000000Z.jsonl"));
    write_finished_journal("1-20260101T000000Z.jsonl");
    CHECK(refused_with(apply_plan(), "journal-sequence-invalid"));
    finish("sequence-edge");
}

/**
 * A copied journal carries its original's sequence, so neither can be read as the newer: the search for an unfinished
 * journal, a resume of either and a new run all refuse, naming both, and nothing more is changed.
 */
static void two_journals_of_one_sequence_refuse_the_run_and_the_resume(void) {
    fresh("sequence-twice");
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("a"), NULL, NULL);
    plan_add(NAULA_MAKE_FOLDER, at("b"), NULL, NULL);
    naula_apply_test_stop_at(4);
    CHECK(refused_with(apply_plan(), "test-stopped"));
    after_a_simulated_crash();
    char *journal_file = unfinished_journal();
    CHECK(journal_file != NULL);
    char *text = journal_file == NULL ? NULL : read_text(journal_file);
    const char *own_name = journal_file == NULL ? "(none)" : strrchr(journal_file, '/') + 1;
    char copy[PATH_SIZE];
    compose(copy, at(".naula-journal/0000000001-20000101T000000Z.jsonl"), NULL);
    write_text(copy, text == NULL ? "" : text);
    char *found = NULL;
    CHECK(refused_with(naula_journal_unfinished(root, &found, &refusal), "journal-sequence-duplicate"));
    CHECK(found == NULL);
    CHECK(strstr(refusal.detail, own_name) != NULL);
    CHECK(strstr(refusal.detail, "0000000001-20000101T000000Z.jsonl") != NULL);
    CHECK(journal_file != NULL && refused_with(resume(journal_file), "journal-sequence-duplicate"));
    CHECK(refused_with(resume(copy), "journal-sequence-duplicate"));
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("other"), NULL, NULL);
    CHECK(refused_with(apply_plan(), "journal-sequence-duplicate"));
    CHECK(kind_at(at("b")) == 0);
    CHECK(kind_at(at("other")) == 0);
    CHECK(journal_count() == 2);
    free(text);
    naula_memory_release(journal_file);
    finish("sequence-twice");
}

static naula_status aliases_checked(void) {
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    return apply_check_aliases(root, &plans.plan, &refusal);
}

/** The identity check alone, past the rule on names: naula's own files by their own names, and an ordinary folder. */
static void a_step_into_naulas_files_is_refused_by_their_identity(void) {
    fresh("identity");
    make_folder(at(".naula-journal"));
    write_text(at(".naula.json"), "{}");
    make_folder(at("org"));
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at(".naula-journal/x"), NULL, NULL);
    CHECK(refused_with(aliases_checked(), "apply-input-invalid"));
    plan_start();
    plan_add(NAULA_MOVE_CHECKOUT, at("org/a"), at(".naula.json/b"), NULL);
    CHECK(refused_with(aliases_checked(), "apply-input-invalid"));
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("org/x"), NULL, NULL);
    plan_add(NAULA_MAKE_FOLDER, at("missing/x"), NULL, NULL);
    plan_add(NAULA_REPORT, NULL, NULL, NULL);
    CHECK(aliases_checked() == NAULA_OK);
    finish("identity");
}

/**
 * The 8.3 aliases Windows makes for `.naula-journal` and `.naula.json` once enough names collide, read back from the
 * volume: a step through either is refused by the rule on names and by the identity check alone.
 */
static void an_alias_windows_generates_for_naulas_files_is_refused(void) {
#ifdef _WIN32
    fresh("generated-alias");
    static const char *const colliding[] = {".naula-journalA", ".naula-journalB", ".naula-journalC", ".naula-journalD",
                                            ".naula-journalE", ".naula-journalF", ".naula-journalG", ".naula-journal",
                                            ".naula.jsonA",    ".naula.jsonB",    ".naula.jsonC",    ".naula.jsonD",
                                            ".naula.jsonE",    ".naula.jsonF",    ".naula.json"};
    for (size_t index = 0; index < sizeof colliding / sizeof colliding[0]; index++) {
        make_folder(at(colliding[index]));
    }
    static const char *const own[] = {".naula-journal", ".naula.json"};
    int aliases = 0;
    for (size_t index = 0; index < 2; index++) {
        char short_path[PATH_SIZE];
        DWORD got = GetShortPathNameA(at(own[index]), short_path, sizeof short_path);
        const char *alias = got == 0 || got >= sizeof short_path ? NULL : strrchr(short_path, '\\');
        alias = alias == NULL ? strrchr(short_path, '/') : alias;
        if (got == 0 || alias == NULL || _stricmp(alias + 1, own[index]) == 0) {
            continue;
        }
        aliases++;
        char step_path[PATH_SIZE];
        compose(step_path, root, "/", alias + 1, "/x", NULL);
        printf("an_alias_windows_generates_for_naulas_files_is_refused: %s is %s\n", own[index], alias + 1);
        plan_start();
        plan_add(NAULA_MAKE_FOLDER, step_path, NULL, NULL);
        CHECK(refused_with(apply_plan(), "apply-input-invalid"));
        CHECK(refused_with(aliases_checked(), "apply-input-invalid"));
        char made[PATH_SIZE];
        compose(made, at(own[index]), "/x", NULL);
        CHECK(kind_at(made) == 0);
    }
    if (aliases == 0) {
        printf("an_alias_windows_generates_for_naulas_files_is_refused: skipped, the volume makes no 8.3 names\n");
    } else {
        CHECK(aliases == 2);
    }
    finish("generated-alias");
#else
    printf("an_alias_windows_generates_for_naulas_files_is_refused: skipped, only Windows makes 8.3 aliases\n");
#endif
}

#define REFILL_WRITERS 3
#define REFILL_SECONDS 20
#define REFILL_FILES 100
#define REFILL_NAMES 10

typedef struct refill_race {
    char folder[PATH_SIZE];
    volatile int removal_returned;
} refill_race;

typedef struct refill_writer {
    refill_race *race;
    int number;
} refill_writer;

/** A file the writer holds open for a moment must not read as one another process holds against the removal. */
static void create_letting_others_delete(const char *path) {
#ifdef _WIN32
    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
#else
    int file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (file >= 0) {
        close(file);
    }
#endif
}

/**
 * Puts back the writer's few files as fast as the removal takes them, until it returns or the time is up, so a
 * removal that never stops still ends. A fixed set of names keeps every pass short on any file system; new names at
 * full speed outgrow a slow removal, whose first pass then outlasts the writers.
 */
static void keep_refilling(refill_writer *writer) {
    time_t deadline = time(NULL) + REFILL_SECONDS;
    for (long made = 0; !writer->race->removal_returned && time(NULL) < deadline; made++) {
        char path[PATH_SIZE + 64];
        snprintf(path, sizeof path, "%s/w%d-%02ld", writer->race->folder, writer->number, made % REFILL_NAMES);
        if (kind_at(path) == 0) {
            create_letting_others_delete(path);
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI refill_thread(LPVOID context) {
    keep_refilling(context);
    return 0;
}
#else
static void *refill_thread(void *context) {
    keep_refilling(context);
    return NULL;
}
#endif

static naula_status remove_clone_while_refilling(refill_race *race) {
    refill_writer writers[REFILL_WRITERS];
    plan_start();
    plan_add(NAULA_REMOVE_CLONE, at("clone"), NULL, NULL);
#ifdef _WIN32
    HANDLE threads[REFILL_WRITERS];
    for (int index = 0; index < REFILL_WRITERS; index++) {
        writers[index].race = race;
        writers[index].number = index;
        threads[index] = CreateThread(NULL, 0, refill_thread, &writers[index], 0, NULL);
    }
    naula_status status = apply_plan();
    race->removal_returned = 1;
    WaitForMultipleObjects(REFILL_WRITERS, threads, TRUE, INFINITE);
    for (int index = 0; index < REFILL_WRITERS; index++) {
        CloseHandle(threads[index]);
    }
#else
    pthread_t threads[REFILL_WRITERS];
    for (int index = 0; index < REFILL_WRITERS; index++) {
        writers[index].race = race;
        writers[index].number = index;
        pthread_create(&threads[index], NULL, refill_thread, &writers[index]);
    }
    naula_status status = apply_plan();
    race->removal_returned = 1;
    for (int index = 0; index < REFILL_WRITERS; index++) {
        pthread_join(threads[index], NULL);
    }
#endif
    return status;
}

#define REFILL_ROUNDS 10

/**
 * One removal raced by the writers: 1 when it stopped as `folder-refills` and its resume then finished, 0 when it won
 * the race outright because the writers left the folder empty for a whole listing, which a scheduler can do.
 */
static int refill_round(int round) {
    fresh("refill");
    make_checkout(at("clone"), "clone");
    make_folder(at("clone/refill"));
    for (int index = 0; index < REFILL_FILES; index++) {
        char name[64];
        snprintf(name, sizeof name, "clone/refill/f%05d", index);
        write_text(at(name), "x");
    }
    refill_race race;
    memset(&race, 0, sizeof race);
    compose(race.folder, at("clone/refill"), NULL);
    time_t started = time(NULL);
    naula_status status = remove_clone_while_refilling(&race);
    printf("a_folder_that_keeps_refilling_stops_the_removal: round %d answered %d %s after %ld s\n", round,
           (int)status, refusal.code, (long)(time(NULL) - started));
    int stopped = status == NAULA_REFUSED && strcmp(refusal.code, "folder-refills") == 0;
    CHECK(stopped || status == NAULA_OK);
    CHECK(kind_at(at(".naula-journal/lock")) == 0);
    char *journal_file = unfinished_journal();
    CHECK(stopped == (journal_file != NULL));
    if (stopped) {
        CHECK(kind_at(at("clone/refill")) == 1);
        CHECK(journal_file != NULL && resume(journal_file) == NAULA_OK);
    }
    CHECK(kind_at(at("clone")) == 0);
    naula_memory_release(journal_file);
    finish("refill");
    return stopped;
}

/**
 * Other programs keep writing into a folder of the clone while it is removed. The removal stops as `folder-refills`
 * rather than chase them for ever, leaving its journal to be resumed, and the resume finishes once they have stopped.
 * A round the removal wins outright proves nothing either way, so of several rounds at least one must stop; a removal
 * that chases the writers wins every round, once their time is up.
 */
static void a_folder_that_keeps_refilling_stops_the_removal(void) {
    int stopped = 0;
    for (int round = 0; round < REFILL_ROUNDS && !stopped; round++) {
        stopped = refill_round(round);
    }
    CHECK(stopped);
}

/** An outside copy of `org/clone`, for a link that stands in for `org` to point at. */
static void lay_out_an_outside_org(const char *outside) {
    char path[PATH_SIZE];
    remove_tree(outside);
    make_folder(outside);
    compose(path, outside, "/clone", NULL);
    make_checkout(path, "outside");
    compose(path, outside, "/clone/keep.txt", NULL);
    write_text(path, "must survive");
}

static void removing_a_clone_refuses_a_folder_above_it_that_is_a_link(void) {
    fresh("above");
    char outside[PATH_SIZE];
    compose(outside, base, "/above/outside-org", NULL);
    lay_out_an_outside_org(outside);
    char *before = snapshot("above/outside-org");
    make_link(at("org"), outside);
    disk_removal_refusal reason = DISK_REMOVAL_REFUSED;
    unsigned long error = 0;
    CHECK(disk_tree_remove(root, "org/clone", &reason, &error) == NAULA_REFUSED);
    char *after = snapshot("above/outside-org");
    CHECK(strcmp(before, after) == 0);
    free(before);
    free(after);
    unlink_link(at("org"));
    finish("above");
}

typedef struct above_race {
    char lock[PATH_SIZE];
    char org[PATH_SIZE];
    char moved[PATH_SIZE];
    char outside[PATH_SIZE];
    unsigned long delay;
    volatile int removal_returned;
    int swapped_in;
} above_race;

static void swap_above_once_the_run_starts(above_race *race) {
    while (kind_at(race->lock) == 0 && !race->removal_returned) {
    }
    for (volatile unsigned long spin = 0; spin < race->delay; spin++) {
    }
#ifdef _WIN32
    int moved = MoveFileA(race->org, race->moved) != 0;
#else
    int moved = rename(race->org, race->moved) == 0;
#endif
    if (moved) {
        unsigned long error = 0;
        race->swapped_in = disk_link_make(race->org, race->outside, &error) == NAULA_OK;
    }
}

#ifdef _WIN32
static DWORD WINAPI above_thread(LPVOID context) {
    swap_above_once_the_run_starts(context);
    return 0;
}
#else
static void *above_thread(void *context) {
    swap_above_once_the_run_starts(context);
    return NULL;
}
#endif

/** Best effort: the folder above the clone is swapped for a link while the run checks and removes it. */
static void removing_a_clone_never_follows_a_folder_above_it_swapped_for_a_link(void) {
    char outside[PATH_SIZE];
    compose(outside, base, "/above-race-outside", NULL);
    lay_out_an_outside_org(outside);
    char *before = snapshot("above-race-outside");
    int swaps = 0;
    for (unsigned long round = 0; round < SWAP_ROUNDS; round++) {
        fresh("above-race");
        make_folder(at("org"));
        make_checkout(at("org/clone"), "inside");
        for (int index = 0; index < 200; index++) {
            char name[64];
            snprintf(name, sizeof name, "org/clone/f%04d.txt", index);
            write_text(at(name), "x");
        }
        above_race race;
        memset(&race, 0, sizeof race);
        compose(race.lock, at(".naula-journal/lock"), NULL);
        compose(race.org, at("org"), NULL);
        compose(race.moved, base, "/above-race/org-moved", NULL);
        compose(race.outside, outside, NULL);
        race.delay = round * 20000ul;
        plan_start();
        plan_add(NAULA_REMOVE_CLONE, at("org/clone"), NULL, NULL);
#ifdef _WIN32
        HANDLE thread = CreateThread(NULL, 0, above_thread, &race, 0, NULL);
        naula_status status = apply_plan();
        race.removal_returned = 1;
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
#else
        pthread_t thread;
        pthread_create(&thread, NULL, above_thread, &race);
        naula_status status = apply_plan();
        race.removal_returned = 1;
        pthread_join(thread, NULL);
#endif
        CHECK(status == NAULA_OK || status == NAULA_REFUSED);
        char *after = snapshot("above-race-outside");
        if (strcmp(before, after) != 0) {
            fprintf(stderr, "round %lu: the folder outside the root changed:\n%s\n", round, after);
            check_failures++;
        }
        free(after);
        swaps += race.swapped_in;
        if (kind_at(at("org")) == 3) {
            unlink_link(at("org"));
        }
        finish("above-race");
    }
    printf("removing_a_clone_never_follows_a_folder_above_it_swapped_for_a_link: %d of %d rounds swapped\n", swaps,
           SWAP_ROUNDS);
    free(before);
    remove_tree(outside);
}

#define DEEP_LEVELS 3000

/** `levels` nested folders `d` below `path`, made relative to each other, since their full path passes any limit. */
static void make_deep(const char *path, int levels) {
#ifdef _WIN32
    static wchar_t wide[32768];
    size_t length = 0;
    const wchar_t prefix[] = L"\\\\?\\";
    for (size_t index = 0; prefix[index] != L'\0'; index++) {
        wide[length++] = prefix[index];
    }
    for (const char *character = path; *character != '\0'; character++) {
        wide[length++] = *character == '/' ? L'\\' : (wchar_t)(unsigned char)*character;
    }
    for (int level = 0; level < levels && length + 3 < sizeof wide / sizeof wide[0]; level++) {
        wide[length++] = L'\\';
        wide[length++] = L'd';
        wide[length] = L'\0';
        if (!CreateDirectoryW(wide, NULL)) {
            fprintf(stderr, "cannot make level %d\n", level);
            check_failures++;
            return;
        }
    }
#else
    int folder = open(path, O_RDONLY | O_DIRECTORY);
    for (int level = 0; level < levels && folder >= 0; level++) {
        mkdirat(folder, "d", 0777);
        int next = openat(folder, "d", O_RDONLY | O_DIRECTORY);
        close(folder);
        folder = next;
    }
    if (folder < 0) {
        fprintf(stderr, "cannot make the deep folders\n");
        check_failures++;
        return;
    }
    close(folder);
#endif
}

static void a_clone_thousands_of_folders_deep_is_removed(void) {
    fresh("deep");
    make_checkout(at("clone"), "deep");
    make_deep(at("clone"), DEEP_LEVELS);
    plan_start();
    plan_add(NAULA_REMOVE_CLONE, at("clone"), NULL, NULL);
    naula_status status = apply_plan();
    if (status != NAULA_OK) {
        fprintf(stderr, "removing a clone %d folders deep: %d %s %s\n", DEEP_LEVELS, (int)status, refusal.code,
                refusal.detail);
    }
    CHECK(status == NAULA_OK);
    CHECK(kind_at(at("clone")) == 0);
    finish("deep");
}

static void a_repointed_recorded_link_is_forgotten_by_the_scan(void) {
    fresh("repointed");
    make_folder(at("repos"));
    make_checkout(at("repos/" HEX), "one");
    make_checkout(at("repos/bbbbbbbbbbbb"), "two");
    make_folder(at("tools"));
    plan_start();
    plan_add(NAULA_MAKE_LINK, at("tools/naula"), at("repos/" HEX), NULL);
    CHECK(apply_plan() == NAULA_OK);
    unlink_link(at("tools/naula"));
    make_link(at("tools/naula"), at("repos/bbbbbbbbbbbb"));
    naula_layout_links scanned;
    CHECK(naula_layout_links_scan(root, &scanned, &refusal) == NAULA_OK);
    naula_layout_links_release(&scanned);
    CHECK(!is_recorded(at("tools/naula"), NULL));
    CHECK(a_journal_holds("\"state\":\"forgotten\"", at("tools/naula")));
    unlink_link(at("tools/naula"));
    make_link(at("tools/naula"), at("repos/" HEX));
    CHECK(naula_layout_links_scan(root, &scanned, &refusal) == NAULA_OK);
    const naula_layout_link *pointed_back = scanned_at(&scanned, at("tools/naula"));
    CHECK(pointed_back != NULL && pointed_back->made_by_naula == 0);
    naula_layout_links_release(&scanned);
    finish("repointed");
}

static void writing_the_root_file_takes_the_roots_lock(void) {
    fresh("root-file-lock");
    naula_place top_place = place(1472199689);
    naula_root_file file;
    memset(&file, 0, sizeof file);
    file.size = sizeof file;
    file.top.size = sizeof file.top;
    file.top.id = "git:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    file.top.at = &top_place;
    file.top.at_count = 1;
    write_lock(own_process_id());
    CHECK(refused_with(naula_root_file_write(root, &file, &refusal), "root-locked"));
    CHECK(kind_at(at(".naula.json")) == 0);
    remove_tree(at(".naula-journal/lock"));
    CHECK(naula_root_file_write(root, &file, &refusal) == NAULA_OK);
    CHECK(kind_at(at(".naula.json")) == 2);
    CHECK(kind_at(at(".naula-journal/lock")) == 0);
    finish("root-file-lock");
}

static void a_move_never_replaces_an_empty_folder(void) {
    fresh("move-no-replace");
    make_folder(at("from"));
    write_text(at("from/work.txt"), "work");
    make_folder(at("to"));
    int held = 0;
    unsigned long error = 0;
    CHECK(disk_move(at("from"), at("to"), &held, &error) == NAULA_REFUSED);
    CHECK(file_holds(at("from/work.txt"), "work"));
    CHECK(kind_at(at("to")) == 1);
    CHECK(kind_at(at("to/work.txt")) == 0);
    finish("move-no-replace");
}

typedef struct named_test {
    const char *name;
    void (*run)(void);
} named_test;

static const named_test TESTS[] = {
    {"memory_bank_key_matches_the_measured_form", memory_bank_key_matches_the_measured_form},
    {"apply_makes_folders_links_and_files", apply_makes_folders_links_and_files},
    {"applying_a_plan_twice_changes_nothing", applying_a_plan_twice_changes_nothing},
    {"apply_writes_the_journal_before_each_step", apply_writes_the_journal_before_each_step},
    {"apply_stops_at_a_held_folder_and_resumes", apply_stops_at_a_held_folder_and_resumes},
    {"apply_renames_the_memory_bank_of_a_moved_checkout", apply_renames_the_memory_bank_of_a_moved_checkout},
    {"apply_refuses_an_existing_memory_bank_target", apply_refuses_an_existing_memory_bank_target},
    {"removing_a_link_leaves_its_target_intact", removing_a_link_leaves_its_target_intact},
    {"remove_link_refuses_a_folder", remove_link_refuses_a_folder},
    {"remove_link_refuses_a_link_to_another_target", remove_link_refuses_a_link_to_another_target},
    {"remove_clone_refuses_a_link", remove_clone_refuses_a_link},
    {"removing_a_clone_never_follows_a_link_inside_it", removing_a_clone_never_follows_a_link_inside_it},
    {"remove_clone_refuses_a_folder_that_is_no_checkout", remove_clone_refuses_a_folder_that_is_no_checkout},
    {"a_step_through_a_link_below_the_root_is_refused", a_step_through_a_link_below_the_root_is_refused},
    {"make_link_replaces_only_a_link", make_link_replaces_only_a_link},
    {"the_record_holds_a_link_only_once_it_is_made_and_until_it_is_removed", the_record_holds_a_link_only_once_it_is_made_and_until_it_is_removed},
    {"the_record_is_refused_when_it_or_the_root_is_a_link", the_record_is_refused_when_it_or_the_root_is_a_link},
    {"a_scanned_link_is_made_by_naula_only_while_it_points_at_the_recorded_target", a_scanned_link_is_made_by_naula_only_while_it_points_at_the_recorded_target},
    {"a_vanished_recorded_link_is_forgotten_by_the_scan", a_vanished_recorded_link_is_forgotten_by_the_scan},
    {"an_overwritten_claude_file_keeps_its_old_content", an_overwritten_claude_file_keeps_its_old_content},
    {"a_write_through_a_link_at_the_file_is_refused", a_write_through_a_link_at_the_file_is_refused},
    {"a_step_outside_the_root_or_on_naulas_files_is_refused", a_step_outside_the_root_or_on_naulas_files_is_refused},
    {"dry_run_changes_nothing", dry_run_changes_nothing},
    {"root_file_round_trips", root_file_round_trips},
    {"a_resume_after_a_crash_at_every_boundary_converges", a_resume_after_a_crash_at_every_boundary_converges},
    {"a_journal_elsewhere_or_for_another_root_is_refused", a_journal_elsewhere_or_for_another_root_is_refused},
    {"survives_every_allocation_failure", survives_every_allocation_failure},
    {"a_root_another_naula_holds_is_refused", a_root_another_naula_holds_is_refused},
    {"a_stale_lock_is_reported_and_never_taken", a_stale_lock_is_reported_and_never_taken},
    {"a_second_apply_waits_for_an_unfinished_journal", a_second_apply_waits_for_an_unfinished_journal},
    {"a_run_refused_before_any_change_leaves_nothing_to_resume", a_run_refused_before_any_change_leaves_nothing_to_resume},
    {"a_run_refused_halfway_through_a_step_must_be_resumed", a_run_refused_halfway_through_a_step_must_be_resumed},
    {"a_long_memory_bank_key_ends_in_claude_codes_hash", a_long_memory_bank_key_ends_in_claude_codes_hash},
    {"removing_a_clone_never_follows_a_folder_swapped_for_a_link_while_it_runs", removing_a_clone_never_follows_a_folder_swapped_for_a_link_while_it_runs},
    {"a_resumed_move_refused_after_its_checkout_moved_stays_unfinished", a_resumed_move_refused_after_its_checkout_moved_stays_unfinished},
    {"a_resumed_link_replacement_refused_after_the_old_link_went_stays_unfinished", a_resumed_link_replacement_refused_after_the_old_link_went_stays_unfinished},
    {"a_stopped_journal_is_never_resumed", a_stopped_journal_is_never_resumed},
    {"a_journal_a_newer_one_follows_is_never_resumed", a_journal_a_newer_one_follows_is_never_resumed},
    {"a_journal_torn_inside_its_opening_append_reads_as_stopped", a_journal_torn_inside_its_opening_append_reads_as_stopped},
    {"a_clone_holding_a_read_only_folder_is_removed", a_clone_holding_a_read_only_folder_is_removed},
    {"removing_a_clone_leaves_a_hard_linked_read_only_file_read_only", removing_a_clone_leaves_a_hard_linked_read_only_file_read_only},
    {"a_step_naming_naulas_files_by_an_alias_is_refused_on_windows", a_step_naming_naulas_files_by_an_alias_is_refused_on_windows},
    {"a_name_with_a_tilde_moves_on_every_platform", a_name_with_a_tilde_moves_on_every_platform},
    {"a_journal_stamped_in_the_future_never_blocks_a_resume", a_journal_stamped_in_the_future_never_blocks_a_resume},
    {"removing_a_clone_refuses_a_folder_above_it_that_is_a_link", removing_a_clone_refuses_a_folder_above_it_that_is_a_link},
    {"removing_a_clone_never_follows_a_folder_above_it_swapped_for_a_link", removing_a_clone_never_follows_a_folder_above_it_swapped_for_a_link},
    {"a_clone_thousands_of_folders_deep_is_removed", a_clone_thousands_of_folders_deep_is_removed},
    {"a_repointed_recorded_link_is_forgotten_by_the_scan", a_repointed_recorded_link_is_forgotten_by_the_scan},
    {"writing_the_root_file_takes_the_roots_lock", writing_the_root_file_takes_the_roots_lock},
    {"a_move_never_replaces_an_empty_folder", a_move_never_replaces_an_empty_folder},
    {"a_journal_sequence_past_the_largest_refuses_the_run", a_journal_sequence_past_the_largest_refuses_the_run},
    {"two_journals_of_one_sequence_refuse_the_run_and_the_resume", two_journals_of_one_sequence_refuse_the_run_and_the_resume},
    {"a_step_into_naulas_files_is_refused_by_their_identity", a_step_into_naulas_files_is_refused_by_their_identity},
    {"an_alias_windows_generates_for_naulas_files_is_refused", an_alias_windows_generates_for_naulas_files_is_refused},
    {"a_folder_that_keeps_refilling_stops_the_removal", a_folder_that_keeps_refilling_stops_the_removal},
};

/** With a test's name as its argument, runs that test alone, so a mutation shows which test it turns red. */
int main(int argc, char **argv) {
    naula_json_use_allocator();
    snprintf(base, sizeof base, "%s/layout_apply", NAULA_TEST_SCRATCH);
    for (char *character = base; *character != '\0'; character++) {
        if (*character == '\\') {
            *character = '/';
        }
    }
    make_folder(base);
    int ran = 0;
    for (size_t index = 0; index < sizeof TESTS / sizeof TESTS[0]; index++) {
        if (argc < 2 || strcmp(argv[1], TESTS[index].name) == 0) {
            TESTS[index].run();
            ran++;
        }
    }
    if (ran == 0) {
        fprintf(stderr, "no test is named %s\n", argv[1]);
        return 2;
    }
    return check_summary("layout_apply_test");
}
