#ifndef NAULA_DISK_H
#define NAULA_DISK_H

/**
 * What the layout applier does to the disk, one call per change and none of them following a link. Every call
 * answers NAULA_OK, NAULA_NOT_FOUND where it says so, NAULA_OUT_OF_MEMORY, or NAULA_REFUSED with the system's own
 * error number in `error` for the refusal's detail. Paths are UTF-8 and either separator.
 */

#include <stddef.h>
#include <stdint.h>

#include "naula/status.h"

/** DISK_LINK is a junction or a symlink; DISK_OTHER is any other reparse point or special file. */
typedef enum disk_kind { DISK_MISSING = 0, DISK_FOLDER = 1, DISK_FILE = 2, DISK_LINK = 3, DISK_OTHER = 4 } disk_kind;

/** What `path` itself is, never what a link there points at. */
naula_status disk_kind_of(const char *path, disk_kind *kind, unsigned long *error);

/** Which file an entry is on its volume, so two names that open one file compare equal. */
typedef struct disk_identity {
    uint64_t volume;
    uint64_t high;
    uint64_t low;
} disk_identity;

/** The identity of `path` itself, never of what a link there points at: NAULA_NOT_FOUND when there is nothing. */
naula_status disk_identity_of(const char *path, disk_identity *identity, unsigned long *error);

/**
 * Where the link at `path` points, with `/` separators and Windows' `\??\` prefix taken off, exactly as
 * disk_link_make was given it. Release with naula_memory_release.
 */
naula_status disk_link_target(const char *path, char **target, unsigned long *error);

/** A junction on Windows and a symlink elsewhere, at `path`, which must not exist. */
naula_status disk_link_make(const char *path, const char *target, unsigned long *error);

/** Removes the link at `path` and never what it points at: RemoveDirectoryW or DeleteFileW, or unlink. */
naula_status disk_link_remove(const char *path, unsigned long *error);

/** Makes the one folder `path`; its parent must exist. */
naula_status disk_folder_make(const char *path, unsigned long *error);

/**
 * Renames `from` to `to` in one call, never replacing what is at `to` and never copying. `held` is set when Windows
 * refused because a process holds something inside `from`.
 */
naula_status disk_move(const char *from, const char *to, int *held, unsigned long *error);

/**
 * Removes the real folder `below` names under the folder `root`, and everything in it. `root` is opened once and
 * every component of `below` relative to the one before it, never following a link, so a link anywhere from `root`
 * down, `below` itself included, is refused rather than followed. A link or other reparse point found inside is
 * removed itself and never entered. The walk keeps its folders on its own stack, never the call stack, so depth is
 * bounded only by memory. A folder counts as empty only once a whole listing of it finds nothing; one that is found
 * refilled on DISK_REMOVAL_REFILL_PASSES listings after its first whole one stops the removal. On POSIX a folder or
 * file inside on another file system or mount than the clone's, a bind mount included, is never entered or removed:
 * the removal stops there, and a clone that is itself a mount point stops it before anything is removed. On Windows a mounted folder is a reparse point, removed itself. `reason` says why a refusal
 * came.
 */
typedef enum disk_removal_refusal {
    DISK_REMOVAL_REFUSED = 0,
    DISK_REMOVAL_HELD = 1,
    DISK_REMOVAL_REFILLED = 2,
    DISK_REMOVAL_MOUNT_INSIDE = 3
} disk_removal_refusal;

#define DISK_REMOVAL_REFILL_PASSES 8

naula_status disk_tree_remove(const char *root, const char *below, disk_removal_refusal *reason,
                              unsigned long *error);

/** The whole regular file at `path`, never through a link: NAULA_NOT_FOUND when there is none. */
naula_status disk_file_read(const char *path, uint8_t **bytes, size_t *length, unsigned long *error);

/**
 * Gives `path` exactly `bytes`: written to `<path>.naula-tmp`, flushed to the disk, then renamed over `path`, so a
 * crash leaves either the old file or the new one.
 */
naula_status disk_file_replace(const char *path, const uint8_t *bytes, size_t length, unsigned long *error);

/** A file this process created and will remove, its path kept ready so the removal allocates nothing. */
typedef struct disk_owned_file disk_owned_file;

/** Creates `path` holding `bytes` in one exclusive create, flushed; one already there refuses and sets `exists`. */
naula_status disk_owned_file_create(const char *path, const uint8_t *bytes, size_t length, int *exists,
                                    disk_owned_file **owned, unsigned long *error);
/** Removes the file and releases `owned`, allocating nothing. NULL does nothing. */
void disk_owned_file_remove(disk_owned_file *owned);
/** Releases `owned` and leaves the file where it is, as a process that dies would. */
void disk_owned_file_abandon(disk_owned_file *owned);

/** Removes the regular file `path`, never a folder or what a link points at. */
naula_status disk_file_remove(const char *path, unsigned long *error);

unsigned long disk_process_id(void);

/** Whether a process with this id runs now; a process the system will not let naula see counts as running. */
int disk_process_is_alive(unsigned long process_id);

/** The names in the folder `path`, without `.` and `..`. Release with disk_names_release. */
naula_status disk_names(const char *path, char ***names, size_t *count, unsigned long *error);
void disk_names_release(char **names, size_t count);

/** A file only ever appended to, each append flushed to the disk before it returns. */
typedef struct disk_appender disk_appender;

/** `create` makes a new file and refuses an existing one; otherwise the file must exist. */
naula_status disk_appender_open(const char *path, int create, disk_appender **appender, unsigned long *error);
naula_status disk_append(disk_appender *appender, const char *bytes, size_t length, unsigned long *error);
void disk_appender_close(disk_appender *appender);

/** The time now in UTC as `yyyymmddThhmmssZ`. */
void disk_utc_stamp(char stamp[17]);

/** The system's text for `error`, for a refusal's detail, cut to fit `text`. */
void disk_error_text(unsigned long error, char *text, size_t capacity);

#endif
