#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout_apply_internal.h"
#include "layout_internal.h"
#include "naula/memory.h"

static const char *const INPUT_FIX = "apply a plan naula_layout_plan_make made for this root, with every size set";
static const char *const LINK_FIX = "remove the link, or put what it points at in place by hand; naula never reaches "
                                    "through a link";
static const char *const OCCUPIED_FIX = "move what is there out of the way, then run naula sync again";
static const char *const HELD_FIX = "close every program, shell and session working in the folder, then run naula "
                                    "sync again to resume";
static const char *const DISK_FIX = "check the folder's permissions and free space, then run naula sync again to "
                                    "resume";
static const char *const REFILLS_FIX = "stop the program that keeps writing into the clone, then run naula sync "
                                       "again to resume";
static const char *const MOUNT_FIX = "unmount what is mounted inside the clone, then run naula sync again to resume";
static const char *const BANK_FIX = "merge or remove one of the two memory banks by hand, then run naula sync again";

typedef struct applier {
    const char *root;
    const char *memory_banks;
    void (*report)(void *context, const naula_layout_step *step, naula_status status);
    void *context;
    journal journal;
    link_record record;
    disk_owned_file *lock;
    int step_changed;
    naula_refusal *refusal;
} applier;

/** What a step will change, decided from the disk before its `intended` line is written. */
typedef struct decision {
    int change;
    int replaces_link;
    int moves_bank;
    char *previous;
    char *bank_from;
    char *bank_to;
} decision;

/** Where a path stands: what its last component is, and whether a folder above it is missing. */
typedef struct reach {
    disk_kind last;
    int parent_missing;
} reach;

#ifdef NAULA_TEST_HOOKS
static unsigned long test_stop_at;
static unsigned long test_reached;

void naula_apply_test_stop_at(unsigned long checkpoint) {
    test_stop_at = checkpoint;
    test_reached = 0;
}

unsigned long naula_apply_test_boundaries(void) {
    return test_reached;
}

static int stops_here(naula_refusal *refusal) {
    test_reached++;
    if (test_stop_at != 0 && test_reached == test_stop_at) {
        naula_refuse(refusal, "test-stopped", "resume the journal", "stopped at boundary %lu", test_reached);
        return 1;
    }
    return 0;
}
#else
static int stops_here(naula_refusal *refusal) {
    (void)refusal;
    return 0;
}
#endif

static int is_link_step(uint32_t kind) {
    return kind == NAULA_MAKE_LINK || kind == NAULA_REMOVE_LINK;
}

static int needs_target(uint32_t kind) {
    return kind == NAULA_MOVE_CHECKOUT || kind == NAULA_MAKE_LINK || kind == NAULA_REMOVE_LINK;
}

static int is_separator(char character) {
    return character == '/' || character == '\\';
}

#ifdef _WIN32
#define WINDOWS_ALIASES 1
#else
#define WINDOWS_ALIASES 0
#endif

static int is_hex_digit(char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

/**
 * Whether `first` could be the 8.3 alias Windows gives `.naula-journal` or `.naula.json`: one to six characters of
 * `NAULA-`, or `NA` and four hex digits once many names collide, then `~` and a digit.
 */
static int is_naula_short_alias(const char *first) {
    size_t prefix = strcspn(first, "~");
    if (first[prefix] != '~' || prefix == 0 || prefix > 6 || first[prefix + 1] < '0' || first[prefix + 1] > '9') {
        return 0;
    }
    char base[7];
    char wanted[7] = "NAULA-";
    memcpy(base, first, prefix);
    base[prefix] = '\0';
    wanted[prefix] = '\0';
    if (layout_same_name(base, wanted)) {
        return 1;
    }
    char head[3] = {base[0], base[1], '\0'};
    return prefix == 6 && layout_same_name(head, "NA") && is_hex_digit(base[2]) && is_hex_digit(base[3]) &&
           is_hex_digit(base[4]) && is_hex_digit(base[5]);
}

/**
 * A path below the root that names naula's own files there, which no step may touch. On Windows that includes the
 * names Windows opens as them: a trailing dot or space is dropped (`.naula-journal.`), and an 8.3 alias stands in.
 */
static int names_naula_files(const char *below) {
    size_t length = strcspn(below, "/\\");
    while (WINDOWS_ALIASES && length > 0 && (below[length - 1] == '.' || below[length - 1] == ' ')) {
        length--;
    }
    char first[32];
    if (length >= sizeof first) {
        return 0;
    }
    memcpy(first, below, length);
    first[length] = '\0';
    return layout_same_name(first, APPLY_JOURNAL_FOLDER) || layout_same_name(first, ".naula.json") ||
           (WINDOWS_ALIASES && is_naula_short_alias(first));
}

static int is_usable_path(const char *root, const char *path) {
    const char *below = path == NULL ? NULL : apply_below(root, path);
    return below != NULL && layout_is_clean_absolute(path) && !names_naula_files(below);
}

static naula_status refuse_input(naula_refusal *refusal, const char *why, size_t index) {
    naula_refuse(refusal, "apply-input-invalid", INPUT_FIX, "step %zu %s", index, why);
    return NAULA_REFUSED;
}

static naula_status check_step(const char *root, const naula_layout_step *step, size_t index,
                               naula_refusal *refusal) {
    if (step->size < offsetof(naula_layout_step, code) + sizeof step->code) {
        return refuse_input(refusal, "is smaller than its first shape", index);
    }
    if (step->kind < NAULA_MAKE_FOLDER || step->kind > NAULA_REPORT) {
        return refuse_input(refusal, "is of no kind naula knows", index);
    }
    if (step->kind == NAULA_REPORT) {
        return NAULA_OK;
    }
    if (!is_usable_path(root, step->path)) {
        return refuse_input(refusal, "has a path that is not a clean absolute path below the root, or names naula's "
                                     "own files there",
                            index);
    }
    if (needs_target(step->kind) && !is_usable_path(root, step->target)) {
        return refuse_input(refusal, "has a target that is not a clean absolute path below the root", index);
    }
    if (step->kind == NAULA_WRITE_FILE && step->content == NULL) {
        return refuse_input(refusal, "writes a file without content", index);
    }
    return NAULA_OK;
}

static naula_status check_input(const naula_layout_plan *plan, const naula_apply_options *options,
                                naula_refusal *refusal) {
    if (options == NULL || options->size < offsetof(naula_apply_options, context) + sizeof options->context ||
        plan == NULL || plan->size < offsetof(naula_layout_plan, step_count) + sizeof plan->step_count) {
        naula_refuse(refusal, "apply-input-invalid", INPUT_FIX, "the options or the plan are smaller than their first "
                                                                "shape");
        return NAULA_REFUSED;
    }
    const char *root = options->root;
    if (root == NULL || !layout_is_clean_absolute(root) || layout_filesystem_root_length(root) >= strlen(root)) {
        naula_refuse(refusal, "apply-input-invalid", INPUT_FIX, "the root is not a clean absolute folder below a "
                                                                "filesystem root");
        return NAULA_REFUSED;
    }
    for (size_t index = 0; index < plan->step_count; index++) {
        naula_status status = check_step(root, &plan->steps[index], index, refusal);
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

/** naula's own files at a root, as the disk identifies them; `has_` is clear for one that is not there. */
typedef struct naula_files {
    disk_identity journal_folder;
    disk_identity root_file;
    int has_journal_folder;
    int has_root_file;
} naula_files;

static int is_same_identity(const disk_identity *left, const disk_identity *right) {
    return left->volume == right->volume && left->high == right->high && left->low == right->low;
}

static naula_status identity_at(const char *path, disk_identity *identity, int *exists, naula_refusal *refusal) {
    unsigned long error = 0;
    naula_status status = disk_identity_of(path, identity, &error);
    *exists = status == NAULA_OK;
    if (status == NAULA_NOT_FOUND) {
        return NAULA_OK;
    }
    return status == NAULA_REFUSED ? apply_refuse_disk(refusal, "path-unreadable", DISK_FIX, "cannot read", path, error)
                                   : status;
}

static naula_status naula_files_at(const char *root, naula_files *files, naula_refusal *refusal) {
    char *journal_folder = layout_join(root, APPLY_JOURNAL_FOLDER);
    char *root_file = layout_join(root, ".naula.json");
    naula_status status = journal_folder == NULL || root_file == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    if (status == NAULA_OK) {
        status = identity_at(journal_folder, &files->journal_folder, &files->has_journal_folder, refusal);
    }
    if (status == NAULA_OK) {
        status = identity_at(root_file, &files->root_file, &files->has_root_file, refusal);
    }
    naula_memory_release(journal_folder);
    naula_memory_release(root_file);
    return status;
}

/** Refuses `path` when its first component below the root opens one of naula's own files, whatever it is named. */
static naula_status refuse_alias_on_disk(const char *root, const naula_files *files, const char *path, size_t index,
                                         naula_refusal *refusal) {
    char *first = naula_memory_duplicate_text(path);
    if (first == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    size_t cut = (size_t)(apply_below(root, path) - path);
    first[cut + strcspn(first + cut, "/\\")] = '\0';
    disk_identity found;
    int exists = 0;
    naula_status status = identity_at(first, &found, &exists, refusal);
    naula_memory_release(first);
    int is_alias = exists && ((files->has_journal_folder && is_same_identity(&found, &files->journal_folder)) ||
                              (files->has_root_file && is_same_identity(&found, &files->root_file)));
    if (status == NAULA_OK && is_alias) {
        return refuse_input(refusal, "names naula's own files at the root by another name the disk opens as them",
                            index);
    }
    return status;
}

naula_status apply_check_aliases(const char *root, const naula_layout_plan *plan, naula_refusal *refusal) {
    naula_files files;
    memset(&files, 0, sizeof files);
    naula_status status = naula_files_at(root, &files, refusal);
    if (status != NAULA_OK || (!files.has_journal_folder && !files.has_root_file)) {
        return status;
    }
    for (size_t index = 0; status == NAULA_OK && index < plan->step_count; index++) {
        const naula_layout_step *step = &plan->steps[index];
        if (step->kind == NAULA_REPORT) {
            continue;
        }
        status = refuse_alias_on_disk(root, &files, step->path, index, refusal);
        if (status == NAULA_OK && needs_target(step->kind)) {
            status = refuse_alias_on_disk(root, &files, step->target, index, refusal);
        }
    }
    return status;
}

static naula_status refuse_kind(applier *applying, const char *code, const char *fix, const char *path,
                                const char *why) {
    naula_refuse(applying->refusal, code, fix, "%s %s", path, why);
    return NAULA_REFUSED;
}

static naula_status kind_of(applier *applying, const char *path, disk_kind *kind) {
    unsigned long error = 0;
    naula_status status = disk_kind_of(path, kind, &error);
    return status == NAULA_REFUSED ? apply_refuse_disk(applying->refusal, "path-unreadable", DISK_FIX, "cannot read",
                                                       path, error)
                                   : status;
}

/** Refuses a link, other reparse point or file at any component of `path` between the root and its last one. */
static naula_status reach_of(applier *applying, const char *path, reach *where) {
    where->last = DISK_MISSING;
    where->parent_missing = 0;
    char *prefix = naula_memory_duplicate_text(path);
    if (prefix == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    size_t start = (size_t)(apply_below(applying->root, path) - path);
    naula_status status = NAULA_OK;
    for (size_t index = start; status == NAULA_OK && prefix[index] != '\0'; index++) {
        if (!is_separator(prefix[index])) {
            continue;
        }
        prefix[index] = '\0';
        disk_kind kind = DISK_MISSING;
        status = kind_of(applying, prefix, &kind);
        if (status == NAULA_OK && (kind == DISK_LINK || kind == DISK_OTHER)) {
            naula_refuse(applying->refusal, "through-link", LINK_FIX, "%s reaches through the link %s", path, prefix);
            status = NAULA_REFUSED;
        } else if (status == NAULA_OK && kind == DISK_FILE) {
            naula_refuse(applying->refusal, "path-occupied", OCCUPIED_FIX, "%s lies below the file %s", path, prefix);
            status = NAULA_REFUSED;
        } else if (status == NAULA_OK && kind == DISK_MISSING) {
            where->parent_missing = 1;
            naula_memory_release(prefix);
            return NAULA_OK;
        }
        prefix[index] = path[index];
    }
    naula_memory_release(prefix);
    return status == NAULA_OK ? kind_of(applying, path, &where->last) : status;
}

static naula_status refuse_missing_parent(applier *applying, const char *path) {
    return refuse_kind(applying, "parent-missing", "apply the whole plan in its order, which makes every folder "
                                                   "before what goes in it",
                       path, "has no parent folder yet");
}

static naula_status refuse_linked(applier *applying, const char *path) {
    return refuse_kind(applying, "through-link", LINK_FIX, path, "is a link or other reparse point");
}

static naula_status decide_folder(applier *applying, const naula_layout_step *step, decision *decided) {
    reach where = {DISK_MISSING, 0};
    naula_status status = reach_of(applying, step->path, &where);
    if (status != NAULA_OK || where.last == DISK_FOLDER) {
        return status;
    }
    if (where.last == DISK_LINK || where.last == DISK_OTHER) {
        return refuse_linked(applying, step->path);
    }
    if (where.last == DISK_FILE) {
        return refuse_kind(applying, "path-occupied", OCCUPIED_FIX, step->path, "is a file, where a folder goes");
    }
    if (where.parent_missing) {
        return refuse_missing_parent(applying, step->path);
    }
    decided->change = 1;
    return NAULA_OK;
}

static char *hex_of(const uint8_t *bytes, size_t length) {
    static const char digits[] = "0123456789abcdef";
    char *hex = naula_memory_allocate(length * 2 + 1);
    if (hex != NULL) {
        for (size_t index = 0; index < length; index++) {
            hex[index * 2] = digits[bytes[index] >> 4];
            hex[index * 2 + 1] = digits[bytes[index] & 15];
        }
        hex[length * 2] = '\0';
    }
    return hex;
}

/** A file with other bytes is kept, hex encoded, for its `intended` line before it is replaced. */
static naula_status decide_file(applier *applying, const naula_layout_step *step, decision *decided) {
    reach where = {DISK_MISSING, 0};
    naula_status status = reach_of(applying, step->path, &where);
    if (status != NAULA_OK) {
        return status;
    }
    if (where.last == DISK_LINK || where.last == DISK_OTHER) {
        return refuse_linked(applying, step->path);
    }
    if (where.last == DISK_FOLDER) {
        return refuse_kind(applying, "path-occupied", OCCUPIED_FIX, step->path, "is a folder, where a file goes");
    }
    if (where.last == DISK_MISSING) {
        decided->change = 1;
        return where.parent_missing ? refuse_missing_parent(applying, step->path) : NAULA_OK;
    }
    uint8_t *bytes = NULL;
    size_t length = 0;
    unsigned long error = 0;
    status = disk_file_read(step->path, &bytes, &length, &error);
    if (status == NAULA_REFUSED || status == NAULA_NOT_FOUND) {
        return apply_refuse_disk(applying->refusal, "path-unreadable", DISK_FIX, "cannot read", step->path, error);
    }
    if (status != NAULA_OK) {
        return status;
    }
    size_t wanted = strlen(step->content);
    if (length != wanted || memcmp(bytes, step->content, wanted) != 0) {
        decided->change = 1;
        decided->previous = hex_of(bytes, length);
        status = decided->previous == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    }
    naula_memory_release(bytes);
    return status;
}

static naula_status decide_bank(applier *applying, const naula_layout_step *step, decision *decided) {
    if (applying->memory_banks == NULL) {
        return NAULA_OK;
    }
    decided->bank_from = memory_bank_path(applying->memory_banks, step->path);
    decided->bank_to = memory_bank_path(applying->memory_banks, step->target);
    if (decided->bank_from == NULL || decided->bank_to == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    disk_kind from = DISK_MISSING;
    disk_kind to = DISK_MISSING;
    naula_status status = kind_of(applying, decided->bank_from, &from);
    if (status == NAULA_OK) {
        status = kind_of(applying, decided->bank_to, &to);
    }
    if (status != NAULA_OK || from == DISK_MISSING) {
        return status;
    }
    if (to != DISK_MISSING) {
        naula_refuse(applying->refusal, "memory-bank-exists", BANK_FIX,
                     "%s cannot move to %s: the memory bank %s is already there, beside %s's bank %s", step->path,
                     step->target, decided->bank_to, step->path, decided->bank_from);
        return NAULA_REFUSED;
    }
    decided->moves_bank = 1;
    return NAULA_OK;
}

static naula_status decide_move(applier *applying, const naula_layout_step *step, decision *decided) {
    reach from = {DISK_MISSING, 0};
    reach to = {DISK_MISSING, 0};
    naula_status status = reach_of(applying, step->path, &from);
    if (status == NAULA_OK) {
        status = reach_of(applying, step->target, &to);
    }
    if (status != NAULA_OK) {
        return status;
    }
    if (from.last == DISK_LINK || from.last == DISK_OTHER) {
        return refuse_kind(applying, "checkout-is-link", LINK_FIX, step->path, "is a link, where a checkout is moved");
    }
    if (from.last == DISK_FILE) {
        return refuse_kind(applying, "path-occupied", OCCUPIED_FIX, step->path, "is a file, where a checkout is");
    }
    if (from.last == DISK_MISSING && to.last != DISK_FOLDER) {
        return refuse_kind(applying, "checkout-missing", "clone the repo again with naula sync", step->path,
                           "is gone, and nothing was moved to the target");
    }
    if (from.last == DISK_FOLDER && to.last != DISK_MISSING) {
        return refuse_kind(applying, "path-occupied", OCCUPIED_FIX, step->target, "is taken, where a checkout moves");
    }
    if (from.last == DISK_FOLDER && to.parent_missing) {
        return refuse_missing_parent(applying, step->target);
    }
    decided->change = from.last == DISK_FOLDER;
    return decide_bank(applying, step, decided);
}

static naula_status read_link_target(applier *applying, const char *path, char **target) {
    unsigned long error = 0;
    naula_status status = disk_link_target(path, target, &error);
    return status == NAULA_REFUSED ? apply_refuse_disk(applying->refusal, "path-unreadable", DISK_FIX,
                                                       "cannot read the link", path, error)
                                   : status;
}

/** Whether the link at `path` points at exactly `target`, read from the disk. */
static naula_status link_points_at(applier *applying, const char *path, const char *target, int *points) {
    char *found = NULL;
    *points = 0;
    naula_status status = read_link_target(applying, path, &found);
    if (status == NAULA_OK) {
        *points = apply_same_target(found, target);
    }
    naula_memory_release(found);
    return status;
}

static naula_status decide_link_at_path(applier *applying, const naula_layout_step *step, int resuming,
                                        decision *decided) {
    int points = 0;
    naula_status status = link_points_at(applying, step->path, step->target, &points);
    const naula_layout_link *recorded = link_record_find(&applying->record, step->path);
    if (status != NAULA_OK) {
        return status;
    }
    if (points && (resuming || (recorded != NULL && apply_same_target(recorded->target, step->target)))) {
        return NAULA_OK;
    }
    int recorded_as_found = 0;
    if (recorded != NULL) {
        status = link_points_at(applying, step->path, recorded->target, &recorded_as_found);
    }
    if (status != NAULA_OK) {
        return status;
    }
    if (!recorded_as_found) {
        return refuse_kind(applying, "path-occupied", OCCUPIED_FIX, step->path,
                           "is a link naula has no record of making, so naula leaves it");
    }
    decided->change = 1;
    decided->replaces_link = 1;
    return NAULA_OK;
}

static naula_status decide_make_link(applier *applying, const naula_layout_step *step, int resuming,
                                     decision *decided) {
    reach target = {DISK_MISSING, 0};
    reach where = {DISK_MISSING, 0};
    naula_status status = reach_of(applying, step->target, &target);
    if (status == NAULA_OK && target.last != DISK_FOLDER) {
        return refuse_kind(applying, "target-missing", "apply the plan's moves first, which put every checkout in "
                                                       "place before it is linked",
                           step->target, "is not a real folder to link to");
    }
    if (status == NAULA_OK) {
        status = reach_of(applying, step->path, &where);
    }
    if (status != NAULA_OK) {
        return status;
    }
    if (where.last == DISK_LINK) {
        return decide_link_at_path(applying, step, resuming, decided);
    }
    if (where.last != DISK_MISSING) {
        return refuse_kind(applying, "path-occupied", OCCUPIED_FIX, step->path, "is taken by something not a link");
    }
    decided->change = 1;
    return where.parent_missing ? refuse_missing_parent(applying, step->path) : NAULA_OK;
}

static naula_status decide_remove_link(applier *applying, const naula_layout_step *step, decision *decided) {
    reach where = {DISK_MISSING, 0};
    reach target = {DISK_MISSING, 0};
    naula_status status = reach_of(applying, step->path, &where);
    if (status == NAULA_OK) {
        status = reach_of(applying, step->target, &target);
    }
    if (status == NAULA_OK && (target.last == DISK_LINK || target.last == DISK_OTHER)) {
        return refuse_linked(applying, step->target);
    }
    if (status != NAULA_OK || where.last == DISK_MISSING) {
        return status;
    }
    if (where.last != DISK_LINK) {
        return refuse_kind(applying, "not-a-link", OCCUPIED_FIX, step->path,
                           "is not a junction or symlink, and naula removes only a link here, never a folder");
    }
    const naula_layout_link *recorded = link_record_find(&applying->record, step->path);
    int points = 0;
    status = link_points_at(applying, step->path, step->target, &points);
    if (status != NAULA_OK) {
        return status;
    }
    if (!points) {
        return refuse_kind(applying, "link-target-changed", OCCUPIED_FIX, step->path,
                           "points somewhere other than the target the plan removes it for");
    }
    if (recorded == NULL || !apply_same_target(recorded->target, step->target)) {
        return refuse_kind(applying, "link-unrecorded", OCCUPIED_FIX, step->path,
                           "is a link naula has no record of making, so naula leaves it");
    }
    decided->change = 1;
    return NAULA_OK;
}

static naula_status refuse_not_a_checkout(applier *applying, const char *path) {
    return refuse_kind(applying, "not-a-checkout", OCCUPIED_FIX, path, "holds no .git, so it is not a clone naula "
                                                                       "removes");
}

/** A removal cut short may already have taken `.git`, so a resumed one asks only for the real folder. */
static naula_status decide_remove_clone(applier *applying, const naula_layout_step *step, int resuming,
                                        decision *decided) {
    reach where = {DISK_MISSING, 0};
    naula_status status = reach_of(applying, step->path, &where);
    if (status != NAULA_OK || where.last == DISK_MISSING) {
        return status;
    }
    if (where.last == DISK_LINK || where.last == DISK_OTHER) {
        return refuse_kind(applying, "clone-is-link", LINK_FIX, step->path,
                           "is a link, and naula removes only the real folder of a clone");
    }
    if (where.last != DISK_FOLDER) {
        return refuse_not_a_checkout(applying, step->path);
    }
    if (resuming) {
        decided->change = 1;
        return NAULA_OK;
    }
    char *dot_git = layout_join(step->path, ".git");
    disk_kind kind = DISK_MISSING;
    status = dot_git == NULL ? NAULA_OUT_OF_MEMORY : kind_of(applying, dot_git, &kind);
    naula_memory_release(dot_git);
    if (status == NAULA_OK && kind == DISK_MISSING) {
        return refuse_not_a_checkout(applying, step->path);
    }
    decided->change = status == NAULA_OK;
    return status;
}

static naula_status decide(applier *applying, const naula_layout_step *step, int resuming, decision *decided) {
    switch (step->kind) {
    case NAULA_MAKE_FOLDER:
        return decide_folder(applying, step, decided);
    case NAULA_WRITE_FILE:
        return decide_file(applying, step, decided);
    case NAULA_MAKE_LINK:
        return decide_make_link(applying, step, resuming, decided);
    case NAULA_MOVE_CHECKOUT:
        return decide_move(applying, step, decided);
    case NAULA_REMOVE_LINK:
        return decide_remove_link(applying, step, decided);
    case NAULA_REMOVE_CLONE:
        return decide_remove_clone(applying, step, resuming, decided);
    default:
        return NAULA_OK;
    }
}

static naula_status refuse_change(applier *applying, naula_status status, const char *what, const char *path,
                                  unsigned long error) {
    return status == NAULA_REFUSED ? apply_refuse_disk(applying->refusal, "change-refused", DISK_FIX, what, path,
                                                       error)
                                   : status;
}

static naula_status perform_move(applier *applying, const naula_layout_step *step, const decision *decided) {
    unsigned long error = 0;
    int held = 0;
    if (decided->change) {
        naula_status status = disk_move(step->path, step->target, &held, &error);
        if (status == NAULA_REFUSED && held) {
            return apply_refuse_disk(applying->refusal, "folder-held", HELD_FIX, "cannot move", step->path, error);
        }
        if (status != NAULA_OK) {
            return refuse_change(applying, status, "cannot move", step->path, error);
        }
        applying->step_changed = 1;
        if (stops_here(applying->refusal)) {
            return NAULA_REFUSED;
        }
    }
    if (!decided->moves_bank) {
        return NAULA_OK;
    }
    naula_status status = disk_move(decided->bank_from, decided->bank_to, &held, &error);
    return status == NAULA_REFUSED ? apply_refuse_disk(applying->refusal, "memory-bank-move-failed", HELD_FIX,
                                                       "cannot rename the memory bank", decided->bank_from, error)
                                   : status;
}

static naula_status perform_make_link(applier *applying, const naula_layout_step *step, const decision *decided) {
    unsigned long error = 0;
    if (decided->replaces_link) {
        naula_status status = disk_link_remove(step->path, &error);
        if (status != NAULA_OK) {
            return refuse_change(applying, status, "cannot remove the link", step->path, error);
        }
        applying->step_changed = 1;
        if (stops_here(applying->refusal)) {
            return NAULA_REFUSED;
        }
    }
    naula_status status = disk_link_make(step->path, step->target, &error);
    return refuse_change(applying, status, "cannot make the link", step->path, error);
}

static naula_status perform_remove_clone(applier *applying, const naula_layout_step *step) {
    unsigned long error = 0;
    disk_removal_refusal reason = DISK_REMOVAL_REFUSED;
    naula_status status = disk_tree_remove(applying->root, apply_below(applying->root, step->path), &reason, &error);
    if (status == NAULA_REFUSED && reason == DISK_REMOVAL_HELD) {
        return apply_refuse_disk(applying->refusal, "folder-held", HELD_FIX, "cannot remove", step->path, error);
    }
    if (status == NAULA_REFUSED && reason == DISK_REMOVAL_REFILLED) {
        naula_refuse(applying->refusal, "folder-refills", REFILLS_FIX,
                     "%s kept filling with new entries while naula removed it, %d listings after it was first emptied",
                     step->path, DISK_REMOVAL_REFILL_PASSES);
        return NAULA_REFUSED;
    }
    if (status == NAULA_REFUSED && reason == DISK_REMOVAL_MOUNT_INSIDE) {
        naula_refuse(applying->refusal, "mount-inside", MOUNT_FIX,
                     "%s holds a folder another file system is mounted on, which naula never empties", step->path);
        return NAULA_REFUSED;
    }
    return refuse_change(applying, status, "cannot remove", step->path, error);
}

/** The error is read only after the call that sets it has returned, never in the same argument list. */
static naula_status perform(applier *applying, const naula_layout_step *step, const decision *decided) {
    unsigned long error = 0;
    naula_status status = NAULA_OK;
    if (!decided->change && !decided->moves_bank) {
        return NAULA_OK;
    }
    switch (step->kind) {
    case NAULA_MAKE_FOLDER:
        status = disk_folder_make(step->path, &error);
        return refuse_change(applying, status, "cannot make", step->path, error);
    case NAULA_WRITE_FILE:
        status = disk_file_replace(step->path, (const uint8_t *)step->content, strlen(step->content), &error);
        return refuse_change(applying, status, "cannot write", step->path, error);
    case NAULA_MAKE_LINK:
        return perform_make_link(applying, step, decided);
    case NAULA_MOVE_CHECKOUT:
        return perform_move(applying, step, decided);
    case NAULA_REMOVE_LINK:
        status = disk_link_remove(step->path, &error);
        return refuse_change(applying, status, "cannot remove the link", step->path, error);
    case NAULA_REMOVE_CLONE:
        applying->step_changed = 1;
        return perform_remove_clone(applying, step);
    default:
        return NAULA_OK;
    }
}

/**
 * Brings the record in line with a link step the journal says is done: a made link is recorded only while the disk
 * holds it with exactly its target, and a removed one is forgotten only once no such link is there.
 */
static naula_status settle_record(applier *applying, const naula_layout_step *step) {
    disk_kind kind = DISK_MISSING;
    naula_status status = kind_of(applying, step->path, &kind);
    int points = 0;
    if (status == NAULA_OK && kind == DISK_LINK) {
        status = link_points_at(applying, step->path, step->target, &points);
    }
    if (status != NAULA_OK) {
        return status;
    }
    if (step->kind == NAULA_REMOVE_LINK) {
        return points ? refuse_kind(applying, "link-changed", OCCUPIED_FIX, step->path, "is back after its removal")
                      : link_record_forget(applying->root, &applying->record, step->path, applying->refusal);
    }
    if (!points) {
        return refuse_kind(applying, "link-changed", OCCUPIED_FIX, step->path,
                           "no longer points at the target naula linked it to");
    }
    return link_record_put(applying->root, &applying->record, step->path, step->target, applying->refusal);
}

static void tell(applier *applying, const naula_layout_step *step, naula_status status) {
    if (applying->report != NULL) {
        applying->report(applying->context, step, status);
    }
}

/**
 * Journals a refusal. One that left the disk exactly at the step's start also ends the journal as `stopped`, so a new
 * plan may be applied; one that came after part of the step's change leaves the journal unfinished, to be resumed.
 */
static naula_status stop_refused(applier *applying, size_t index, const naula_layout_step *step,
                                 naula_status status) {
    if (status == NAULA_REFUSED && strcmp(applying->refusal->code, "test-stopped") == 0) {
        return status;
    }
    if (status == NAULA_REFUSED) {
        naula_refusal ignored;
        ignored.size = sizeof ignored;
        if (journal_mark(&applying->journal, index, "refused", applying->refusal->code, NULL, &ignored) == NAULA_OK &&
            !applying->step_changed) {
            journal_stop(&applying->journal, &ignored);
        }
    }
    tell(applying, step, status);
    return status;
}

static naula_status finish_link_step(applier *applying, size_t index, const naula_layout_step *step) {
    applying->step_changed = 1;
    naula_status status = settle_record(applying, step);
    if (status != NAULA_OK) {
        return stop_refused(applying, index, step, status);
    }
    if (stops_here(applying->refusal)) {
        return NAULA_REFUSED;
    }
    status = journal_mark(&applying->journal, index, "recorded", NULL, NULL, applying->refusal);
    if (status != NAULA_OK || stops_here(applying->refusal)) {
        return status != NAULA_OK ? status : NAULA_REFUSED;
    }
    return NAULA_OK;
}

static void release_decision(decision *decided) {
    naula_memory_release(decided->previous);
    naula_memory_release(decided->bank_from);
    naula_memory_release(decided->bank_to);
}

/** A resumed step whose `intended` line was written may already be part done, so a refusal never stops it cleanly. */
static naula_status run_change(applier *applying, size_t index, const naula_layout_step *step, int resuming) {
    decision decided;
    memset(&decided, 0, sizeof decided);
    applying->step_changed = resuming;
    naula_status status = decide(applying, step, resuming, &decided);
    if (status == NAULA_OK) {
        status = journal_mark(&applying->journal, index, "intended", NULL, decided.previous, applying->refusal);
    }
    if (status == NAULA_OK && stops_here(applying->refusal)) {
        release_decision(&decided);
        return NAULA_REFUSED;
    }
    if (status == NAULA_OK) {
        status = perform(applying, step, &decided);
    }
    release_decision(&decided);
    if (status != NAULA_OK) {
        return stop_refused(applying, index, step, status);
    }
    if (stops_here(applying->refusal)) {
        return NAULA_REFUSED;
    }
    status = journal_mark(&applying->journal, index, "done", NULL, NULL, applying->refusal);
    if (status != NAULA_OK || stops_here(applying->refusal)) {
        return status != NAULA_OK ? status : NAULA_REFUSED;
    }
    return NAULA_OK;
}

static naula_status run_step(applier *applying, size_t index, const naula_layout_step *step,
                             unsigned char progress) {
    naula_status status = NAULA_OK;
    if (progress < JOURNAL_DONE) {
        status = run_change(applying, index, step, progress == JOURNAL_INTENDED);
    }
    if (status == NAULA_OK && is_link_step(step->kind)) {
        status = finish_link_step(applying, index, step);
    }
    if (status == NAULA_OK) {
        tell(applying, step, NAULA_OK);
    }
    return status;
}

static naula_status run_steps(applier *applying, const naula_layout_plan *plan, const unsigned char *progress) {
    for (size_t index = 0; index < plan->step_count; index++) {
        const naula_layout_step *step = &plan->steps[index];
        unsigned char seen = progress == NULL ? JOURNAL_NONE : progress[index];
        unsigned char complete = is_link_step(step->kind) ? JOURNAL_RECORDED : JOURNAL_DONE;
        if (seen >= complete) {
            continue;
        }
        naula_status status = run_step(applying, index, step, seen);
        if (status != NAULA_OK) {
            return status;
        }
    }
    naula_status status = journal_finish(&applying->journal, applying->refusal);
    return status == NAULA_OK && stops_here(applying->refusal) ? NAULA_REFUSED : status;
}

static void start_applier(applier *applying, const naula_apply_options *options, naula_refusal *refusal) {
    memset(applying, 0, sizeof *applying);
    applying->root = options->root;
    applying->memory_banks = options->memory_banks;
    applying->report = options->report;
    applying->context = options->context;
    applying->refusal = refusal;
}

static int is_simulated_death(naula_status status, const naula_refusal *refusal) {
    return status == NAULA_REFUSED && strcmp(refusal->code, "test-stopped") == 0;
}

/** A run the test hooks stop dies as a process would, leaving its lock behind. */
static void end_applier(applier *applying, naula_status status) {
    journal_close(&applying->journal);
    link_record_release(&applying->record);
    if (is_simulated_death(status, applying->refusal)) {
        disk_owned_file_abandon(applying->lock);
    } else {
        root_lock_release(applying->lock);
    }
    applying->lock = NULL;
}

static void report_dry_run(const naula_layout_plan *plan, const naula_apply_options *options) {
    for (size_t index = 0; options->report != NULL && index < plan->step_count; index++) {
        options->report(options->context, &plan->steps[index], NAULA_OK);
    }
}

static naula_status refuse_unfinished(const char *root, naula_refusal *refusal) {
    char *journal_file = NULL;
    naula_status status = naula_journal_unfinished(root, &journal_file, refusal);
    if (status == NAULA_NOT_FOUND) {
        return NAULA_OK;
    }
    if (status == NAULA_OK) {
        naula_refuse(refusal, "journal-unfinished",
                     "resume it with naula_journal_resume (naula sync does), then lay the root out again",
                     "the journal %s records a run that stopped partway, so the disk may be mid-step", journal_file);
        status = NAULA_REFUSED;
    }
    naula_memory_release(journal_file);
    return status;
}

naula_status naula_layout_apply(const naula_layout_plan *plan, const naula_apply_options *options,
                                naula_refusal *refusal) {
    naula_status status = check_input(plan, options, refusal);
    if (status == NAULA_OK) {
        status = apply_check_aliases(options->root, plan, refusal);
    }
    if (status != NAULA_OK) {
        return status;
    }
    if (options->dry_run) {
        report_dry_run(plan, options);
        return NAULA_OK;
    }
    applier applying;
    start_applier(&applying, options, refusal);
    status = apply_check_root(options->root, 1, refusal);
    if (status == NAULA_OK) {
        status = root_lock_take(options->root, &applying.lock, refusal);
    }
    if (status == NAULA_OK) {
        status = refuse_unfinished(options->root, refusal);
    }
    if (status == NAULA_OK) {
        status = link_record_read(options->root, &applying.record, refusal);
    }
    if (status == NAULA_OK) {
        status = journal_start(options->root, plan, &applying.journal, refusal);
    }
    if (status == NAULA_OK && stops_here(refusal)) {
        status = NAULA_REFUSED;
    } else if (status == NAULA_OK) {
        status = run_steps(&applying, plan, NULL);
    }
    end_applier(&applying, status);
    return status;
}

static int is_journal_name(const char *name) {
    unsigned long long sequence = 0;
    return journal_name_read(name, &sequence) == JOURNAL_NAME_SEQUENCED;
}

static naula_status refuse_journal_place(naula_refusal *refusal, const char *journal_file) {
    naula_refuse(refusal, "journal-elsewhere", "resume a journal naula wrote in the root's own .naula-journal folder",
                 "%s is not a journal directly in the root's .naula-journal folder", journal_file);
    return NAULA_REFUSED;
}

static naula_status check_journal_place(const char *root, const char *journal_file, naula_refusal *refusal) {
    const char *below = apply_below(root, journal_file);
    size_t folder_length = strlen(APPLY_JOURNAL_FOLDER);
    if (below == NULL || strncmp(below, APPLY_JOURNAL_FOLDER, folder_length) != 0 ||
        !is_separator(below[folder_length])) {
        return refuse_journal_place(refusal, journal_file);
    }
    const char *name = below + folder_length + 1;
    size_t length = strlen(name);
    if (strcspn(name, "/\\") != length || !is_journal_name(name)) {
        return refuse_journal_place(refusal, journal_file);
    }
    disk_kind kind = DISK_MISSING;
    unsigned long error = 0;
    naula_status read = disk_kind_of(journal_file, &kind, &error);
    if (read == NAULA_OUT_OF_MEMORY) {
        return read;
    }
    if (read != NAULA_OK || kind != DISK_FILE) {
        naula_refuse(refusal, "record-is-link", LINK_FIX, "%s is not a regular file", journal_file);
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

static naula_status list_journals(const char *root, journal_listing *listing, naula_refusal *refusal) {
    listing->journals = NULL;
    listing->count = 0;
    char *folder = layout_join(root, APPLY_JOURNAL_FOLDER);
    if (folder == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    naula_status status = journal_list(folder, listing, refusal);
    naula_memory_release(folder);
    return status;
}

/** A journal resumed while the root's journals cannot be put in order, finished or not, might not be the newest. */
static naula_status refuse_unordered_journals(const char *root, naula_refusal *refusal) {
    journal_listing listing;
    naula_status status = list_journals(root, &listing, refusal);
    journal_listing_release(&listing);
    return status;
}

static naula_status resume_contents(const char *journal_file, const naula_apply_options *options,
                                    journal_contents *contents, naula_refusal *refusal) {
    naula_status status = check_journal_place(options->root, journal_file, refusal);
    if (status == NAULA_OK) {
        status = apply_check_root(options->root, 0, refusal);
    }
    if (status == NAULA_OK) {
        status = refuse_unordered_journals(options->root, refusal);
    }
    if (status == NAULA_OK) {
        status = journal_load(journal_file, contents, refusal);
    }
    if (status == NAULA_OK && !apply_same_path(contents->root, options->root)) {
        naula_refuse(refusal, "journal-root-differs", "resume the journal with the root it names",
                     "%s was written for the root %s, not %s", journal_file, contents->root, options->root);
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK) {
        status = check_input(&contents->plan, options, refusal);
    }
    if (status == NAULA_OK) {
        status = apply_check_aliases(options->root, &contents->plan, refusal);
    }
    return status;
}

static const char *last_component(const char *path) {
    const char *name = path;
    for (const char *character = path; *character != '\0'; character++) {
        if (is_separator(*character)) {
            name = character + 1;
        }
    }
    return name;
}

static naula_status refuse_if_superseded(const char *root, const char *journal_file, naula_refusal *refusal) {
    journal_listing listing;
    naula_status status = list_journals(root, &listing, refusal);
    unsigned long long own = 0;
    journal_name_read(last_component(journal_file), &own);
    if (status == NAULA_OK && listing.count > 0 && listing.journals[0].sequence > own) {
        naula_refuse(refusal, "journal-superseded",
                     "check the disk by hand, then move the older journal out of .naula-journal and lay the root "
                     "out again",
                     "%s is older than the journal %s, so its plan may undo a newer layout", journal_file,
                     listing.journals[0].name);
        status = NAULA_REFUSED;
    }
    journal_listing_release(&listing);
    return status;
}

/**
 * A journal that stopped cleanly is never resumed: its plan was made for the disk as it stood then, and a new plan
 * is made instead. Nor is one that a newer journal follows, whose plan has moved the root on since.
 */
static naula_status refuse_unresumable(const char *root, const char *journal_file, const journal_contents *contents,
                                       naula_refusal *refusal) {
    if (contents->stopped) {
        naula_refuse(refusal, "journal-stopped", "lay the root out again with a new plan (naula sync does)",
                     "%s stopped cleanly at a refusal, so it is not resumed", journal_file);
        return NAULA_REFUSED;
    }
    return refuse_if_superseded(root, journal_file, refusal);
}

naula_status naula_journal_resume(const char *journal_file, const naula_apply_options *options,
                                  naula_refusal *refusal) {
    naula_layout_plan empty = {sizeof empty, NULL, 0};
    naula_status status = check_input(&empty, options, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    if (journal_file == NULL) {
        return refuse_journal_place(refusal, "(none)");
    }
    applier applying;
    start_applier(&applying, options, refusal);
    status = check_journal_place(options->root, journal_file, refusal);
    if (status == NAULA_OK && !options->dry_run) {
        status = apply_check_root(options->root, 0, refusal);
    }
    if (status == NAULA_OK && !options->dry_run) {
        status = root_lock_take(options->root, &applying.lock, refusal);
    }
    journal_contents contents;
    memset(&contents, 0, sizeof contents);
    if (status == NAULA_OK) {
        status = resume_contents(journal_file, options, &contents, refusal);
    }
    if (status == NAULA_OK && !contents.finished) {
        status = refuse_unresumable(options->root, journal_file, &contents, refusal);
    }
    if (status != NAULA_OK || contents.finished || options->dry_run) {
        if (status == NAULA_OK && options->dry_run) {
            report_dry_run(&contents.plan, options);
        }
        journal_contents_release(&contents);
        end_applier(&applying, status);
        return status;
    }
    status = link_record_read(options->root, &applying.record, refusal);
    if (status == NAULA_OK) {
        status = journal_reopen(journal_file, contents.ends_mid_line, &applying.journal, refusal);
    }
    if (status == NAULA_OK) {
        status = run_steps(&applying, &contents.plan, contents.progress);
    }
    end_applier(&applying, status);
    journal_contents_release(&contents);
    return status;
}

/** The newest journal in `listing` that neither finished nor stopped, read in the listing's order, newest first. */
static naula_status unfinished_among(const char *folder, const journal_listing *listing, char **journal_file,
                                     naula_refusal *refusal) {
    for (size_t index = 0; index < listing->count; index++) {
        char *path = layout_join(folder, listing->journals[index].name);
        journal_contents contents;
        memset(&contents, 0, sizeof contents);
        naula_status status = path == NULL ? NAULA_OUT_OF_MEMORY : journal_load(path, &contents, refusal);
        int finished = status == NAULA_OK && (contents.finished || contents.stopped);
        if (status == NAULA_OK) {
            journal_contents_release(&contents);
        }
        if (status == NAULA_OK && !finished) {
            *journal_file = path;
            return NAULA_OK;
        }
        naula_memory_release(path);
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_NOT_FOUND;
}

naula_status naula_journal_unfinished(const char *root, char **journal_file, naula_refusal *refusal) {
    *journal_file = NULL;
    naula_status status = apply_check_root(root, 0, refusal);
    char *folder = status == NAULA_OK ? layout_join(root, APPLY_JOURNAL_FOLDER) : NULL;
    if (status != NAULA_OK || folder == NULL) {
        return status != NAULA_OK ? status : NAULA_OUT_OF_MEMORY;
    }
    disk_kind kind = DISK_MISSING;
    unsigned long error = 0;
    status = disk_kind_of(folder, &kind, &error);
    if (status == NAULA_REFUSED) {
        status = apply_refuse_disk(refusal, "root-unreadable", DISK_FIX, "cannot read", folder, error);
    }
    journal_listing listing = {NULL, 0};
    if (status == NAULA_OK && kind == DISK_FOLDER) {
        status = journal_list(folder, &listing, refusal);
    }
    if (status == NAULA_OK) {
        status = unfinished_among(folder, &listing, journal_file, refusal);
    }
    journal_listing_release(&listing);
    naula_memory_release(folder);
    return status;
}
