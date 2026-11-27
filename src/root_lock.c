#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout_apply_internal.h"
#include "layout_internal.h"
#include "naula/memory.h"

#define LOCK_NAME "lock"

static const char *const LOCKED_FIX = "wait for the other naula to finish; when no naula runs on this root, remove the "
                                      "lock file by hand";
static const char *const STALE_FIX = "check that no naula runs on this root, remove the lock file by hand, then resume "
                                     "the unfinished journal with naula_journal_resume";

static char *lock_path_of(const char *root) {
    char *folder = layout_join(root, APPLY_JOURNAL_FOLDER);
    char *path = folder == NULL ? NULL : layout_join(folder, LOCK_NAME);
    naula_memory_release(folder);
    return path;
}

static int read_holder(const char *path, unsigned long *holder) {
    uint8_t *bytes = NULL;
    size_t length = 0;
    unsigned long error = 0;
    if (disk_file_read(path, &bytes, &length, &error) != NAULA_OK) {
        return 0;
    }
    char *end = NULL;
    unsigned long value = strtoul((const char *)bytes, &end, 10);
    int read = end != (char *)bytes && *end == '\n' && end[1] == '\0' && value != 0;
    naula_memory_release(bytes);
    *holder = value;
    return read;
}

static naula_status refuse_held(const char *path, naula_refusal *refusal) {
    unsigned long holder = 0;
    if (!read_holder(path, &holder)) {
        naula_refuse(refusal, "root-locked", LOCKED_FIX, "the lock %s is held by a naula whose process id it does not "
                                                         "name",
                     path);
    } else if (disk_process_is_alive(holder)) {
        naula_refuse(refusal, "root-locked", LOCKED_FIX, "the naula with process id %lu holds the lock %s", holder,
                     path);
    } else {
        naula_refuse(refusal, "stale-lock", STALE_FIX,
                     "the lock %s names process id %lu, which no longer runs: a naula stopped without finishing",
                     path, holder);
    }
    return NAULA_REFUSED;
}

naula_status root_lock_take(const char *root, char **lock_path, naula_refusal *refusal) {
    *lock_path = lock_path_of(root);
    if (*lock_path == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    disk_kind kind = DISK_MISSING;
    unsigned long error = 0;
    naula_status status = disk_kind_of(*lock_path, &kind, &error);
    if (status == NAULA_OK && kind != DISK_MISSING && kind != DISK_FILE) {
        naula_refuse(refusal, "record-is-link", "remove it; naula keeps its lock as a regular file",
                     "the lock %s is not a regular file", *lock_path);
        status = NAULA_REFUSED;
    } else if (status == NAULA_REFUSED) {
        status = apply_refuse_disk(refusal, "root-unreadable", LOCKED_FIX, "cannot read", *lock_path, error);
    }
    if (status == NAULA_OK) {
        char text[32];
        snprintf(text, sizeof text, "%lu\n", disk_process_id());
        int exists = 0;
        status = disk_file_create(*lock_path, (const uint8_t *)text, strlen(text), &exists, &error);
        if (status == NAULA_REFUSED) {
            status = exists ? refuse_held(*lock_path, refusal)
                            : apply_refuse_disk(refusal, "root-unwritable", LOCKED_FIX, "cannot create", *lock_path,
                                                error);
        }
    }
    if (status != NAULA_OK) {
        naula_memory_release(*lock_path);
        *lock_path = NULL;
    }
    return status;
}

void root_lock_release(char *lock_path) {
    unsigned long holder = 0;
    unsigned long error = 0;
    if (lock_path != NULL && read_holder(lock_path, &holder) && holder == disk_process_id()) {
        disk_file_remove(lock_path, &error);
    }
    naula_memory_release(lock_path);
}
