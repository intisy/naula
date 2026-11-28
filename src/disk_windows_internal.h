#ifndef NAULA_DISK_WINDOWS_INTERNAL_H
#define NAULA_DISK_WINDOWS_INTERNAL_H

#include <windows.h>
#include <winternl.h>

#include "disk.h"
#include "naula/status.h"

#ifndef FILE_CREATE
#define FILE_CREATE 0x00000002
#endif
#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE 0x00000001
#endif
#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_NON_DIRECTORY_FILE 0x00000040
#endif

/** How every relative open is made: a reparse point itself, never what it points at, with synchronous I/O. */
#define DISK_OPEN_OPTIONS (FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT)

typedef LONG(NTAPI *nt_create_file_function)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                             PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef LONG(NTAPI *nt_set_information_file_function)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
typedef ULONG(NTAPI *nt_status_to_error_function)(LONG);

/** The ntdll entry points that open by a folder's handle and rename into one, which kernel32 does not offer. */
typedef struct relative_opener {
    nt_create_file_function create;
    nt_set_information_file_function set_information;
    nt_status_to_error_function to_error;
} relative_opener;

int relative_opener_load(relative_opener *opener);

/**
 * Opens, or makes, as NtCreateFile's `disposition` says, the entry `name` of the folder `parent` holds open, by that
 * handle, so no path above it is walked again.
 */
naula_status disk_open_at(const relative_opener *opener, HANDLE parent, const WCHAR *name, DWORD name_bytes,
                          ACCESS_MASK access, ULONG share, ULONG disposition, ULONG options, HANDLE *opened,
                          unsigned long *error);

/** The folder a walk from a root reached, held open, and the last component of its `below`, to act on by name. */
typedef struct disk_walk {
    relative_opener opener;
    HANDLE parent;
    wchar_t *below;
    const wchar_t *name;
    DWORD name_bytes;
} disk_walk;

/**
 * Opens `root`, then every component of `below` but the last relative to the handle before it, never through a
 * reparse point, refusing a component that is empty, `.` or `..`. `purpose` changes nothing here, since a mounted
 * folder is a reparse point. Release `walk` with disk_walk_release whatever this answers.
 */
naula_status disk_walk_to_parent(const char *root, const char *below, disk_walk_purpose purpose, disk_walk *walk,
                                 unsigned long *error);
void disk_walk_release(disk_walk *walk);

/**
 * Renames what `moving` holds to `name` in the folder `folder` holds, replacing what is there only when `replace`.
 *
 * @implNote Nothing is flushed after the rename, unlike POSIX's fsync of both folders: MOVEFILE_WRITE_THROUGH matters
 * only to a move Windows copies across volumes, which a rename by handle never is; NTFS logs its metadata changes in
 * order, so once the journal's `done` line after it is flushed the rename is on the disk too; and a step whose `done`
 * line never reached the disk is decided again from the disk on resume, so a rename the disk lost is made again.
 */
naula_status disk_rename_by_handle(const relative_opener *opener, HANDLE moving, HANDLE folder, const WCHAR *name,
                                   DWORD name_bytes, int replace, unsigned long *error);

/** Removes the entry `handle` holds, by that handle, however its attributes are set. */
naula_status disk_delete_by_handle(HANDLE handle, DWORD attributes, unsigned long *error);

#ifdef NAULA_TEST_HOOKS
void disk_test_reach(disk_test_point point, const char *name);
#else
static __inline void disk_test_reach(disk_test_point point, const char *name) {
    (void)point;
    (void)name;
}
#endif

#endif
