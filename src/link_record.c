#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "layout_apply_internal.h"
#include "layout_internal.h"
#include "naula/json.h"
#include "naula/memory.h"

#define SCAN_DEPTH_LIMIT 32

static const char *const RECORD_FIX = "remove the link or other reparse point and run naula sync again; naula keeps its "
                                      "own files only in real folders at the root";

static int is_separator(char character) {
    return character == '/' || character == '\\';
}

static char folded(char character) {
#ifdef _WIN32
    return character >= 'A' && character <= 'Z' ? (char)(character - 'A' + 'a') : character;
#else
    return character;
#endif
}

int apply_same_path(const char *left, const char *right) {
    for (; *left != '\0' && *right != '\0'; left++, right++) {
        int both_separators = is_separator(*left) && is_separator(*right);
        if (!both_separators && folded(*left) != folded(*right)) {
            return 0;
        }
    }
    return *left == *right;
}

int apply_same_target(const char *left, const char *right) {
    for (; *left != '\0' && *right != '\0'; left++, right++) {
        if (!(is_separator(*left) && is_separator(*right)) && *left != *right) {
            return 0;
        }
    }
    return *left == *right;
}

const char *apply_below(const char *root, const char *path) {
    size_t length = strlen(root);
    while (length > 0 && is_separator(root[length - 1])) {
        length--;
    }
    for (size_t index = 0; index < length; index++) {
        if (path[index] != root[index] && !(is_separator(path[index]) && is_separator(root[index]))) {
            return NULL;
        }
    }
    if (!is_separator(path[length]) || path[length + 1] == '\0' || is_separator(path[length + 1])) {
        return NULL;
    }
    return path + length + 1;
}

naula_status apply_refuse_disk(naula_refusal *refusal, const char *code, const char *fix, const char *what,
                               const char *path, unsigned long error) {
    char system_text[300];
    disk_error_text(error, system_text, sizeof system_text);
    naula_refuse(refusal, code, fix, "%s %s: %s", what, path, system_text);
    return NAULA_REFUSED;
}

static naula_status require_folder(const char *path, int make, const char *link_code, naula_refusal *refusal) {
    disk_kind kind = DISK_MISSING;
    unsigned long error = 0;
    naula_status read = disk_kind_of(path, &kind, &error);
    if (read != NAULA_OK) {
        return read == NAULA_REFUSED ? apply_refuse_disk(refusal, "root-unreadable", RECORD_FIX, "cannot read", path,
                                                         error)
                                     : read;
    }
    if (kind == DISK_MISSING && make) {
        naula_status status = disk_folder_make(path, &error);
        return status == NAULA_REFUSED ? apply_refuse_disk(refusal, "root-unwritable", RECORD_FIX, "cannot make", path,
                                                           error)
                                       : status;
    }
    if (kind == DISK_MISSING) {
        return NAULA_NOT_FOUND;
    }
    if (kind != DISK_FOLDER) {
        naula_refuse(refusal, kind == DISK_FILE ? "path-occupied" : link_code, RECORD_FIX,
                     "%s is a %s, where naula keeps a real folder", path, kind == DISK_FILE ? "file" : "link");
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

naula_status apply_check_root(const char *root, int make, naula_refusal *refusal) {
    naula_status status = require_folder(root, 0, "root-is-link", refusal);
    if (status == NAULA_NOT_FOUND) {
        naula_refuse(refusal, "root-missing", "make the root folder, or name the one the plan was made for",
                     "the root %s does not exist", root);
        return NAULA_REFUSED;
    }
    char *journal_folder = status == NAULA_OK ? layout_join(root, APPLY_JOURNAL_FOLDER) : NULL;
    if (status == NAULA_OK && journal_folder == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    if (status == NAULA_OK) {
        status = require_folder(journal_folder, make, "record-is-link", refusal);
        status = status == NAULA_NOT_FOUND ? NAULA_OK : status;
    }
    naula_memory_release(journal_folder);
    return status;
}

static char *record_path(const char *root) {
    char *folder = layout_join(root, APPLY_JOURNAL_FOLDER);
    char *path = folder == NULL ? NULL : layout_join(folder, APPLY_LINK_RECORD);
    naula_memory_release(folder);
    return path;
}

void link_record_release(link_record *record) {
    for (size_t index = 0; index < record->count; index++) {
        naula_memory_release(record->links[index].path);
        naula_memory_release(record->links[index].target);
    }
    naula_memory_release(record->links);
    record->links = NULL;
    record->count = 0;
}

static naula_status append_link(naula_layout_link **links, size_t *count, const char *path, const char *target,
                                int made_by_naula) {
    naula_layout_link *larger = naula_memory_reallocate(*links, (*count + 1) * sizeof *larger);
    if (larger == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    *links = larger;
    naula_layout_link *link = &larger[*count];
    memset(link, 0, sizeof *link);
    link->size = sizeof *link;
    link->made_by_naula = made_by_naula;
    link->path = naula_memory_duplicate_text(path);
    link->target = naula_memory_duplicate_text(target);
    if (link->path == NULL || link->target == NULL) {
        naula_memory_release(link->path);
        naula_memory_release(link->target);
        return NAULA_OUT_OF_MEMORY;
    }
    (*count)++;
    return NAULA_OK;
}

static naula_status refuse_record(naula_refusal *refusal, const char *path, const char *why) {
    naula_refuse(refusal, "link-record-invalid",
                 "restore the record from the journals, or remove it so naula removes no link until it is rebuilt",
                 "%s %s", path, why);
    return NAULA_REFUSED;
}

static naula_status read_entries(const cJSON *json, const char *path, link_record *record, naula_refusal *refusal) {
    const cJSON *links = cJSON_GetObjectItemCaseSensitive(json, "links");
    if (!cJSON_IsObject(json) || !cJSON_IsArray(links)) {
        return refuse_record(refusal, path, "is not an object holding a links array");
    }
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, links) {
        const cJSON *link_path = cJSON_GetObjectItemCaseSensitive(entry, "path");
        const cJSON *target = cJSON_GetObjectItemCaseSensitive(entry, "target");
        if (!cJSON_IsString(link_path) || !cJSON_IsString(target)) {
            return refuse_record(refusal, path, "holds an entry without a path and a target");
        }
        naula_status status = append_link(&record->links, &record->count, link_path->valuestring, target->valuestring,
                                          1);
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

static naula_status check_record_file(const char *path, naula_refusal *refusal) {
    disk_kind kind = DISK_MISSING;
    unsigned long error = 0;
    naula_status read = disk_kind_of(path, &kind, &error);
    if (read != NAULA_OK) {
        return read == NAULA_REFUSED ? apply_refuse_disk(refusal, "root-unreadable", RECORD_FIX, "cannot read", path,
                                                         error)
                                     : read;
    }
    if (kind == DISK_MISSING) {
        return NAULA_NOT_FOUND;
    }
    if (kind != DISK_FILE) {
        naula_refuse(refusal, "record-is-link", RECORD_FIX, "%s is not a regular file", path);
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

naula_status link_record_read(const char *root, link_record *record, naula_refusal *refusal) {
    record->links = NULL;
    record->count = 0;
    naula_status status = apply_check_root(root, 0, refusal);
    char *path = status == NAULA_OK ? record_path(root) : NULL;
    if (status != NAULA_OK || path == NULL) {
        return status != NAULA_OK ? status : NAULA_OUT_OF_MEMORY;
    }
    status = check_record_file(path, refusal);
    uint8_t *bytes = NULL;
    size_t length = 0;
    unsigned long error = 0;
    if (status == NAULA_OK) {
        status = disk_file_read(path, &bytes, &length, &error);
        if (status == NAULA_REFUSED || status == NAULA_NOT_FOUND) {
            status = apply_refuse_disk(refusal, "root-unreadable", RECORD_FIX, "cannot read", path, error);
        }
    }
    cJSON *json = NULL;
    const char *reason = NULL;
    if (status == NAULA_OK) {
        status = naula_json_parse(bytes, length, &json, &reason);
        if (status == NAULA_MALFORMED) {
            status = refuse_record(refusal, path, reason);
        }
    }
    if (status == NAULA_OK) {
        status = read_entries(json, path, record, refusal);
    }
    cJSON_Delete(json);
    naula_memory_release(bytes);
    naula_memory_release(path);
    if (status == NAULA_NOT_FOUND) {
        return NAULA_OK;
    }
    if (status != NAULA_OK) {
        link_record_release(record);
    }
    return status;
}

static naula_status write_record(const char *root, const link_record *record, naula_refusal *refusal) {
    naula_status status = apply_check_root(root, 1, refusal);
    char *path = status == NAULA_OK ? record_path(root) : NULL;
    if (status != NAULA_OK || path == NULL) {
        return status != NAULA_OK ? status : NAULA_OUT_OF_MEMORY;
    }
    status = check_record_file(path, refusal);
    status = status == NAULA_NOT_FOUND ? NAULA_OK : status;
    cJSON *json = status == NAULA_OK ? cJSON_CreateObject() : NULL;
    cJSON *links = json == NULL ? NULL : cJSON_AddArrayToObject(json, "links");
    if (status == NAULA_OK && links == NULL) {
        status = NAULA_OUT_OF_MEMORY;
    }
    for (size_t index = 0; status == NAULA_OK && index < record->count; index++) {
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL || !cJSON_AddItemToArray(links, entry) ||
            cJSON_AddStringToObject(entry, "path", record->links[index].path) == NULL ||
            cJSON_AddStringToObject(entry, "target", record->links[index].target) == NULL) {
            status = NAULA_OUT_OF_MEMORY;
        }
    }
    char *text = status == NAULA_OK ? cJSON_PrintUnformatted(json) : NULL;
    if (status == NAULA_OK && text == NULL) {
        status = NAULA_OUT_OF_MEMORY;
    }
    unsigned long error = 0;
    if (status == NAULA_OK) {
        status = disk_file_replace(path, (const uint8_t *)text, strlen(text), &error);
        if (status == NAULA_REFUSED) {
            status = apply_refuse_disk(refusal, "record-unwritable", RECORD_FIX, "cannot write", path, error);
        }
    }
    cJSON_free(text);
    cJSON_Delete(json);
    naula_memory_release(path);
    return status;
}

const naula_layout_link *link_record_find(const link_record *record, const char *path) {
    for (size_t index = 0; index < record->count; index++) {
        if (apply_same_path(record->links[index].path, path)) {
            return &record->links[index];
        }
    }
    return NULL;
}

naula_status link_record_put(const char *root, link_record *record, const char *path, const char *target,
                             naula_refusal *refusal) {
    for (size_t index = 0; index < record->count; index++) {
        if (apply_same_path(record->links[index].path, path)) {
            char *copy = naula_memory_duplicate_text(target);
            if (copy == NULL) {
                return NAULA_OUT_OF_MEMORY;
            }
            naula_memory_release(record->links[index].target);
            record->links[index].target = copy;
            return write_record(root, record, refusal);
        }
    }
    naula_status status = append_link(&record->links, &record->count, path, target, 1);
    return status == NAULA_OK ? write_record(root, record, refusal) : status;
}

naula_status link_record_forget(const char *root, link_record *record, const char *path, naula_refusal *refusal) {
    for (size_t index = 0; index < record->count; index++) {
        if (apply_same_path(record->links[index].path, path)) {
            naula_memory_release(record->links[index].path);
            naula_memory_release(record->links[index].target);
            memmove(&record->links[index], &record->links[index + 1],
                    (record->count - index - 1) * sizeof *record->links);
            record->count--;
            return write_record(root, record, refusal);
        }
    }
    return NAULA_OK;
}

void naula_layout_links_release(naula_layout_links *links) {
    if (links == NULL) {
        return;
    }
    for (size_t index = 0; index < links->count; index++) {
        naula_memory_release(links->links[index].path);
        naula_memory_release(links->links[index].target);
    }
    naula_memory_release(links->links);
    links->links = NULL;
    links->count = 0;
}

typedef struct scan {
    const link_record *record;
    naula_layout_links *found;
    naula_refusal *refusal;
} scan;

static naula_status add_found_link(scan *scanning, const char *path, disk_kind kind) {
    char *target = NULL;
    unsigned long error = 0;
    naula_status status = kind == DISK_LINK ? disk_link_target(path, &target, &error) : NAULA_OK;
    if (status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    const naula_layout_link *recorded = link_record_find(scanning->record, path);
    int made_by_naula = status == NAULA_OK && target != NULL && recorded != NULL &&
                        apply_same_target(recorded->target, target);
    status = append_link(&scanning->found->links, &scanning->found->count, path, target == NULL ? "" : target,
                         made_by_naula);
    naula_memory_release(target);
    return status;
}

/** A `.git` that cannot be read counts as a checkout, so the scan never enters one by doubt. */
static naula_status holds_a_checkout(const char *folder, int *holds) {
    char *dot_git = layout_join(folder, ".git");
    if (dot_git == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    disk_kind kind = DISK_MISSING;
    unsigned long error = 0;
    naula_status status = disk_kind_of(dot_git, &kind, &error);
    naula_memory_release(dot_git);
    *holds = status != NAULA_OK || kind != DISK_MISSING;
    return status == NAULA_OUT_OF_MEMORY ? status : NAULA_OK;
}

static naula_status scan_folder(scan *scanning, const char *folder, int depth);

static naula_status scan_entry(scan *scanning, const char *folder, const char *name, int depth) {
    char *child = layout_join(folder, name);
    if (child == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    disk_kind kind = DISK_MISSING;
    unsigned long error = 0;
    naula_status status = disk_kind_of(child, &kind, &error);
    if (status == NAULA_REFUSED) {
        status = apply_refuse_disk(scanning->refusal, "root-unreadable", RECORD_FIX, "cannot read", child, error);
    } else if (kind == DISK_LINK || kind == DISK_OTHER) {
        status = add_found_link(scanning, child, kind);
    } else if (kind == DISK_FOLDER && depth < SCAN_DEPTH_LIMIT && strcmp(name, APPLY_JOURNAL_FOLDER) != 0 &&
               strcmp(name, ".git") != 0) {
        int holds = 0;
        status = holds_a_checkout(child, &holds);
        if (status == NAULA_OK && !holds) {
            status = scan_folder(scanning, child, depth + 1);
        }
    }
    naula_memory_release(child);
    return status;
}

static naula_status scan_folder(scan *scanning, const char *folder, int depth) {
    char **names = NULL;
    size_t count = 0;
    unsigned long error = 0;
    naula_status status = disk_names(folder, &names, &count, &error);
    if (status == NAULA_REFUSED) {
        return apply_refuse_disk(scanning->refusal, "root-unreadable", RECORD_FIX, "cannot list", folder, error);
    }
    for (size_t index = 0; status == NAULA_OK && index < count; index++) {
        status = scan_entry(scanning, folder, names[index], depth);
    }
    disk_names_release(names, count);
    return status;
}

static naula_status still_made(const naula_layout_link *entry, int *made, naula_refusal *refusal) {
    disk_kind kind = DISK_MISSING;
    unsigned long error = 0;
    *made = 0;
    naula_status status = disk_kind_of(entry->path, &kind, &error);
    if (status == NAULA_REFUSED) {
        return apply_refuse_disk(refusal, "root-unreadable", RECORD_FIX, "cannot read", entry->path, error);
    }
    if (status != NAULA_OK || kind != DISK_LINK) {
        return status;
    }
    char *target = NULL;
    status = disk_link_target(entry->path, &target, &error);
    if (status == NAULA_OK) {
        *made = apply_same_target(target, entry->target);
    }
    naula_memory_release(target);
    return status == NAULA_REFUSED ? NAULA_OK : status;
}

static naula_status forget_entries(const char *root, link_record *record, journal *journalling,
                                   naula_refusal *refusal) {
    naula_status status = NAULA_OK;
    for (size_t index = 0; status == NAULA_OK && index < record->count;) {
        int made = 0;
        status = still_made(&record->links[index], &made, refusal);
        if (status != NAULA_OK || made) {
            index++;
            continue;
        }
        if (journalling->appender == NULL) {
            naula_layout_plan empty = {sizeof empty, NULL, 0};
            status = journal_start(root, &empty, journalling, refusal);
        }
        if (status == NAULA_OK) {
            status = journal_forgotten(journalling, record->links[index].path, record->links[index].target, refusal);
        }
        if (status == NAULA_OK) {
            status = link_record_forget(root, record, record->links[index].path, refusal);
        }
    }
    if (status == NAULA_OK && journalling->appender != NULL) {
        status = journal_finish(journalling, refusal);
    }
    return status;
}

/** Its `forgotten` journal would be newer than an unfinished run's, which could then never be resumed. */
static naula_status refuse_while_unfinished(const char *root, naula_refusal *refusal) {
    char *journal_file = NULL;
    naula_status status = naula_journal_unfinished(root, &journal_file, refusal);
    if (status == NAULA_NOT_FOUND) {
        return NAULA_OK;
    }
    if (status == NAULA_OK) {
        naula_refuse(refusal, "journal-unfinished",
                     "resume it with naula_journal_resume (naula sync does), then scan the root's links again",
                     "the journal %s records a run that stopped partway, so its links may be mid-step", journal_file);
        status = NAULA_REFUSED;
    }
    naula_memory_release(journal_file);
    return status;
}

/**
 * Forgets every recorded link the disk no longer holds exactly as recorded, journalled under the root's lock, so a
 * link someone makes there later is never read as naula's.
 */
static naula_status forget_vanished(const char *root, link_record *record, naula_refusal *refusal) {
    int any = 0;
    for (size_t index = 0; index < record->count && !any; index++) {
        int made = 0;
        naula_status status = still_made(&record->links[index], &made, refusal);
        if (status != NAULA_OK) {
            return status;
        }
        any = !made;
    }
    if (!any) {
        return NAULA_OK;
    }
    disk_owned_file *lock = NULL;
    naula_status status = root_lock_take(root, &lock, refusal);
    journal journalling = {NULL, NULL};
    if (status == NAULA_OK) {
        status = refuse_while_unfinished(root, refusal);
    }
    if (status == NAULA_OK) {
        status = forget_entries(root, record, &journalling, refusal);
    }
    journal_close(&journalling);
    root_lock_release(lock);
    return status;
}

naula_status naula_layout_links_scan(const char *root, naula_layout_links *links, naula_refusal *refusal) {
    links->size = sizeof *links;
    links->links = NULL;
    links->count = 0;
    link_record record = {NULL, 0};
    naula_status status = link_record_read(root, &record, refusal);
    if (status == NAULA_OK) {
        status = forget_vanished(root, &record, refusal);
    }
    if (status == NAULA_OK) {
        scan scanning = {&record, links, refusal};
        status = scan_folder(&scanning, root, 0);
    }
    link_record_release(&record);
    if (status != NAULA_OK) {
        naula_layout_links_release(links);
    }
    return status;
}
