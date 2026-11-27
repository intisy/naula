#ifndef NAULA_DISK_POSIX_INTERNAL_H
#define NAULA_DISK_POSIX_INTERNAL_H

#include "naula/status.h"

/**
 * Flushes the folder holding `path`, so a name made, renamed or removed there survives a power cut. Out of memory is
 * answered after the change it follows was made, which the journal's missing `done` line leaves to a resume to
 * confirm.
 */
naula_status disk_sync_parent(const char *path);

#if defined(NAULA_TEST_HOOKS) && defined(__linux__)
/**
 * Lets a removal read mount ids by statx, by file handle, both or neither, so a test stands in for a Linux before 5.8
 * or a file system that names no file by handle; both is the default.
 *
 * @implNote Defined only when NAULA_TEST_HOOKS is set, which only the naula_test_hooks library does; the shipped
 * library carries no such symbol and always tries both.
 */
void naula_disk_test_mount_ids(int by_statx, int by_file_handle);
#endif

#endif
