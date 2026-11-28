#ifndef NAULA_DISK_H
#define NAULA_DISK_H

/**
 * What the layout applier does to the disk, one call per change and none of them following a link. Every call
 * answers NAULA_OK, NAULA_NOT_FOUND where it says so, NAULA_OUT_OF_MEMORY, or NAULA_REFUSED with the system's own
 * error number in `error` for the refusal's detail. Paths are UTF-8 and either separator.
 *
 * A call that changes the disk takes a folder `root` and a path `below` it, opens `root` once and every component of
 * `below` but the last relative to the folder before it, never through a link, and acts on the last component by its
 * name in the folder it holds open, never by a full path. So a link swapped in anywhere below `root` after the caller
 * checked the path is refused rather than followed. A component that is empty, `.` or `..` is refused.
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

/** A junction on Windows and a symlink elsewhere, at `below` under `root`, which must not exist. */
naula_status disk_link_make(const char *root, const char *below, const char *target, unsigned long *error);

/** Removes the link at `below` under `root` and never what it points at; anything but a link there is refused. */
naula_status disk_link_remove(const char *root, const char *below, unsigned long *error);

/** Makes the one folder `below` under `root`; its parent must exist. */
naula_status disk_folder_make(const char *root, const char *below, unsigned long *error);

/**
 * Renames `from` to `to`, both below `root`, in one call, never replacing what is at `to` and never copying. `held` is
 * set when Windows refused because a process holds something inside `from`.
 */
naula_status disk_move(const char *root, const char *from, const char *to, int *held, unsigned long *error);

/**
 * Removes the real folder `below` names under the folder `root`, and everything in it. The walk from `root` follows
 * no link, so a link anywhere from `root` down, `below` itself included, is refused. A link or other reparse point
 * found inside is removed itself and never entered. The walk keeps its folders on its own stack, never the call stack,
 * and carries each folder's listing on past every subfolder it holds, so its time is linear in the entries it finds.
 *
 * Two bounds stop a removal another program keeps feeding; each answers NAULA_REFUSED having removed what it removed,
 * and `reason` says which. Across a folder: one found holding entries on DISK_REMOVAL_REFILL_PASSES listings after its
 * first whole one, or whose listing holds more entries than the one before it, is DISK_REMOVAL_REFILLED. In depth: a
 * folder more than disk_removal_depth_limit() levels below the clone is DISK_REMOVAL_TOO_DEEP, which is what stops a
 * writer making ever deeper new folders, since each of those is filled only once.
 *
 * On POSIX the mount `root` is on is recorded, and every folder from `root` down to the clone, the clone, and every
 * folder and file in it is judged against it: one on another file system or mount, a bind mount included, is
 * DISK_REMOVAL_MOUNT_INSIDE and never entered or removed, so a mount at or above the clone stops the removal before
 * anything is removed, a deliberate one included. On Linux a bind mount is told apart only by its mount id, from statx
 * or else a file handle; where neither gives one, the removal stops as DISK_REMOVAL_MOUNT_UNVERIFIABLE. On Windows a
 * mounted folder is a reparse point, refused on the way to the clone like a link and removed itself inside it.
 */
typedef enum disk_removal_refusal {
    DISK_REMOVAL_REFUSED = 0,
    DISK_REMOVAL_HELD = 1,
    DISK_REMOVAL_REFILLED = 2,
    DISK_REMOVAL_MOUNT_INSIDE = 3,
    DISK_REMOVAL_MOUNT_UNVERIFIABLE = 4,
    DISK_REMOVAL_TOO_DEEP = 5
} disk_removal_refusal;

#define DISK_REMOVAL_REFILL_PASSES 8

/**
 * How many levels below the clone a removal enters: the longest path the system opens, at two bytes a level, so no
 * checkout git makes is deeper. 2048 on Linux, 512 on macOS, 16383 on Windows.
 */
size_t disk_removal_depth_limit(void);

naula_status disk_tree_remove(const char *root, const char *below, disk_removal_refusal *reason,
                              unsigned long *error);

#ifdef NAULA_TEST_HOOKS
/**
 * How many names every removal's listings have read in this process, so a test bounds the walk's work by count, which
 * no machine's speed can blur.
 *
 * @implNote Defined only when NAULA_TEST_HOOKS is set, which only the naula_test_hooks library does.
 */
size_t naula_disk_test_names_listed(void);
#endif

/** Where a test hook is called; `name` is the path or entry the call is about to reach. */
typedef enum disk_test_point {
    /** A call that walks from its root, before it opens the root: `name` is its `below`. */
    DISK_TEST_BEFORE_WALK = 0,
    /** A removal, after it judged an entry a real folder and before it opens it: `name` is the entry's name. */
    DISK_TEST_BEFORE_ENTRY_OPEN = 1,
    /** disk_file_read, before it opens: `name` is its path. */
    DISK_TEST_BEFORE_FILE_OPEN = 2
} disk_test_point;

#ifdef NAULA_TEST_HOOKS
typedef void (*disk_test_hook)(disk_test_point point, const char *name, void *context);

/**
 * Calls `hook` at every disk_test_point until it is set back to NULL, so a test changes the disk at exactly the moment
 * a race would.
 *
 * @implNote Defined only when NAULA_TEST_HOOKS is set, which only the naula_test_hooks library does.
 */
void naula_disk_test_hook(disk_test_hook hook, void *context);

/**
 * Makes disk_removal_depth_limit() answer `levels`, 0 restoring the system's own, so a test reaches the bound where
 * reaching the real one costs more than a test may: NTFS spends time on every open that grows with the folder's depth,
 * so a Windows tree 16383 levels deep takes far longer to make and remove than the depth alone suggests.
 *
 * @implNote Defined only when NAULA_TEST_HOOKS is set, which only the naula_test_hooks library does.
 */
void naula_disk_test_depth_limit(size_t levels);
#endif

/**
 * The whole regular file at `path`, never through a link: NAULA_NOT_FOUND when there is none. Anything else there, a
 * FIFO swapped in after a check included, is refused without waiting on it.
 */
naula_status disk_file_read(const char *path, uint8_t **bytes, size_t *length, unsigned long *error);

/**
 * Gives `below` under `root` exactly `bytes`: written to `<below>.naula-tmp`, flushed to the disk, then renamed over
 * `below`, so a crash leaves either the old file or the new one.
 */
naula_status disk_file_replace(const char *root, const char *below, const uint8_t *bytes, size_t length,
                               unsigned long *error);

/**
 * A file this process created and will remove, its folder held open and its name kept, so the removal reaches the
 * same folder and allocates nothing.
 */
typedef struct disk_owned_file disk_owned_file;

/**
 * Creates `below` under `root` holding `bytes` in one exclusive create, flushed; one already there refuses and sets
 * `exists`.
 */
naula_status disk_owned_file_create(const char *root, const char *below, const uint8_t *bytes, size_t length,
                                    int *exists, disk_owned_file **owned, unsigned long *error);
/** Removes the file and releases `owned`, allocating nothing. NULL does nothing. */
void disk_owned_file_remove(disk_owned_file *owned);
/** Releases `owned` and leaves the file where it is, as a process that dies would. */
void disk_owned_file_abandon(disk_owned_file *owned);

unsigned long disk_process_id(void);

/** Whether a process with this id runs now; a process the system will not let naula see counts as running. */
int disk_process_is_alive(unsigned long process_id);

/** The names in the folder `path`, without `.` and `..`. Release with disk_names_release. */
naula_status disk_names(const char *path, char ***names, size_t *count, unsigned long *error);
void disk_names_release(char **names, size_t count);

/** A file only ever appended to, each append flushed to the disk before it returns. */
typedef struct disk_appender disk_appender;

/** Opens `below` under `root`: `create` makes a new file and refuses an existing one; otherwise it must exist. */
naula_status disk_appender_open(const char *root, const char *below, int create, disk_appender **appender,
                                unsigned long *error);
naula_status disk_append(disk_appender *appender, const char *bytes, size_t length, unsigned long *error);
void disk_appender_close(disk_appender *appender);

/** The time now in UTC as `yyyymmddThhmmssZ`. */
void disk_utc_stamp(char stamp[17]);

/** The system's text for `error`, for a refusal's detail, cut to fit `text`. */
void disk_error_text(unsigned long error, char *text, size_t capacity);

#endif
