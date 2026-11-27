#ifdef _WIN32
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "disk.h"
#include "layout_apply_internal.h"
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
    naula_apply_test_stop_at(0);
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
    naula_apply_test_stop_at(0);
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
    char *journal_file = unfinished_journal();
    CHECK(journal_file != NULL);
    CHECK(resume(journal_file) == NAULA_OK);
    CHECK(kind_at(at("old")) == 0);
    CHECK(file_holds(at("repos/" HEX "/README.md"), "held checkout"));
    CHECK(links_to(at("name"), at("repos/" HEX)));
    CHECK(unfinished_journal() == NULL);
    naula_memory_release(journal_file);
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
    naula_apply_test_stop_at(0);
    CHECK(kind_at(at("name")) == 0);
    CHECK(!is_recorded(at("name"), NULL));

    fresh("record");
    make_folder(at("repos"));
    make_checkout(at("repos/" HEX), "checkout");
    naula_apply_test_stop_at(4);
    CHECK(refused_with(apply_plan(), "test-stopped"));
    naula_apply_test_stop_at(0);
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
        naula_apply_test_stop_at(0);
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

static void a_journal_elsewhere_or_for_another_root_is_refused(void) {
    fresh("journal-place");
    plan_start();
    plan_add(NAULA_MAKE_FOLDER, at("org"), NULL, NULL);
    naula_apply_test_stop_at(2);
    CHECK(refused_with(apply_plan(), "test-stopped"));
    naula_apply_test_stop_at(0);
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
    {"an_overwritten_claude_file_keeps_its_old_content", an_overwritten_claude_file_keeps_its_old_content},
    {"a_write_through_a_link_at_the_file_is_refused", a_write_through_a_link_at_the_file_is_refused},
    {"a_step_outside_the_root_or_on_naulas_files_is_refused", a_step_outside_the_root_or_on_naulas_files_is_refused},
    {"dry_run_changes_nothing", dry_run_changes_nothing},
    {"root_file_round_trips", root_file_round_trips},
    {"a_resume_after_a_crash_at_every_boundary_converges", a_resume_after_a_crash_at_every_boundary_converges},
    {"a_journal_elsewhere_or_for_another_root_is_refused", a_journal_elsewhere_or_for_another_root_is_refused},
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
