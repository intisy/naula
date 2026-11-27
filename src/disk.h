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
 * Removes the real folder `path` and everything in it. A link or other reparse point found inside is removed itself,
 * by a non-recursive call, and never entered; `path` itself is refused when it is one.
 */
naula_status disk_tree_remove(const char *path, unsigned long *error);

/** The whole regular file at `path`, never through a link: NAULA_NOT_FOUND when there is none. */
naula_status disk_file_read(const char *path, uint8_t **bytes, size_t *length, unsigned long *error);

/**
 * Gives `path` exactly `bytes`: written to `<path>.naula-tmp`, flushed to the disk, then renamed over `path`, so a
 * crash leaves either the old file or the new one.
 */
naula_status disk_file_replace(const char *path, const uint8_t *bytes, size_t length, unsigned long *error);

/** Creates `path` holding `bytes` in one exclusive create, flushed; one already there refuses and sets `exists`. */
naula_status disk_file_create(const char *path, const uint8_t *bytes, size_t length, int *exists,
                              unsigned long *error);

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
