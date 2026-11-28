#ifndef NAULA_DISK_POSIX_INTERNAL_H
#define NAULA_DISK_POSIX_INTERNAL_H

#include "disk.h"
#include "naula/status.h"

/** The folder a walk from a root reached, held open, and the last component of its `below`, to act on by name. */
typedef struct disk_walk {
    int parent;
    const char *name;
    char *below;
} disk_walk;

/**
 * Opens `root`, then every component of `below` but the last relative to the folder before it, never through a link,
 * refusing a component that is empty, `.` or `..` and a folder on another mount than the root's, judged as `purpose`
 * says. Release `walk` with disk_walk_release whatever this answers.
 */
naula_status disk_walk_to_parent(const char *root, const char *below, disk_walk_purpose purpose, disk_walk *walk,
                                 unsigned long *error);
void disk_walk_release(disk_walk *walk);

#ifdef NAULA_TEST_HOOKS
void disk_test_reach(disk_test_point point, const char *name);
#else
static inline void disk_test_reach(disk_test_point point, const char *name) {
    (void)point;
    (void)name;
}
#endif

#ifdef NAULA_TEST_HOOKS
/**
 * Makes disk_folder_make answer `error` from the system for every folder named `name`, NULL ending it, so a test stands
 * in for a file system that refuses a call naula makes.
 *
 * @implNote Defined only when NAULA_TEST_HOOKS is set, which only the naula_test_hooks library does.
 */
void naula_disk_test_fail_folder_make(const char *name, int error);
#endif

#if defined(NAULA_TEST_HOOKS) && defined(__linux__)
/**
 * Lets every walk read mount ids by statx, by file handle, both or neither, so a test stands in for a Linux before 5.8
 * or a file system that names no file by handle; both is the default.
 *
 * @implNote Defined only when NAULA_TEST_HOOKS is set, which only the naula_test_hooks library does; the shipped
 * library carries no such symbol and always tries both.
 */
void naula_disk_test_mount_ids(int by_statx, int by_file_handle);
#endif

#endif
