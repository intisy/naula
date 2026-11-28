#include <windows.h>
#include <winternl.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "disk.h"
#include "disk_windows_internal.h"
#include "naula/memory.h"
#include "wide_path.h"
#include "windows_name.h"

#define FILE_LIMIT (16 * 1024 * 1024)
#define WRITE_CHUNK 65536u

struct disk_appender {
    HANDLE file;
};

static naula_status refused(unsigned long *error) {
    *error = GetLastError();
    return NAULA_REFUSED;
}

#ifdef NAULA_TEST_HOOKS
static disk_test_hook test_hook;
static void *test_hook_context;

void naula_disk_test_hook(disk_test_hook hook, void *context) {
    test_hook = hook;
    test_hook_context = context;
}

void disk_test_reach(disk_test_point point, const char *name) {
    if (test_hook != NULL) {
        test_hook(point, name, test_hook_context);
    }
}

static void test_reach_wide(disk_test_point point, const wchar_t *name) {
    char *narrow = NULL;
    if (test_hook != NULL && narrow_text(name, &narrow) == NAULA_OK) {
        disk_test_reach(point, narrow);
        naula_memory_release(narrow);
    }
}
#else
static void test_reach_wide(disk_test_point point, const wchar_t *name) {
    (void)point;
    (void)name;
}
#endif

static int is_held_error(DWORD code) {
    return code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
}

static wchar_t *child_path(const wchar_t *parent, const wchar_t *name) {
    size_t parent_length = wcslen(parent);
    size_t name_length = wcslen(name);
    wchar_t *joined = naula_memory_allocate((parent_length + 1 + name_length + 1) * sizeof *joined);
    if (joined != NULL) {
        wmemcpy(joined, parent, parent_length);
        joined[parent_length] = L'\\';
        wmemcpy(joined + parent_length + 1, name, name_length + 1);
    }
    return joined;
}

static int is_dot_name(const wchar_t *name) {
    return wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0;
}

/**
 * Every open a removal makes: DELETE to remove the entry by its own handle, the listing right for a folder, and no
 * FILE_SHARE_DELETE, so no other process can rename an entry naula holds open while it works inside it. The reparse
 * point flag opens a link itself, never what it points at.
 */
#define REMOVAL_ACCESS (DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE)
#define REMOVAL_SHARE (FILE_SHARE_READ | FILE_SHARE_WRITE)
#define WALK_ACCESS (FILE_TRAVERSE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE)
#define WALK_SHARE (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
#define LISTING_BUFFER_SIZE 65536u

/** ntdll is mapped into every process, so its entry points are looked up rather than linked. */
int relative_opener_load(relative_opener *opener) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    opener->create = ntdll == NULL ? NULL : (nt_create_file_function)(void (*)(void))GetProcAddress(ntdll, "NtCreateFile");
    opener->set_information = ntdll == NULL ? NULL
                                            : (nt_set_information_file_function)(void (*)(void))GetProcAddress(
                                                  ntdll, "NtSetInformationFile");
    opener->to_error =
        ntdll == NULL ? NULL : (nt_status_to_error_function)(void (*)(void))GetProcAddress(ntdll, "RtlNtStatusToDosError");
    return opener->create != NULL && opener->set_information != NULL && opener->to_error != NULL;
}

naula_status disk_open_at(const relative_opener *opener, HANDLE parent, const WCHAR *name, DWORD name_bytes,
                          ACCESS_MASK access, ULONG share, ULONG disposition, ULONG options, HANDLE *opened,
                          unsigned long *error) {
    UNICODE_STRING relative;
    relative.Buffer = (PWSTR)name;
    relative.Length = (USHORT)name_bytes;
    relative.MaximumLength = (USHORT)name_bytes;
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &relative, OBJ_CASE_INSENSITIVE, parent, NULL);
    IO_STATUS_BLOCK io;
    LONG result = opener->create(opened, access, &attributes, &io, NULL, 0, share, disposition, options, NULL, 0);
    if (result < 0) {
        *opened = INVALID_HANDLE_VALUE;
        *error = opener->to_error(result);
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

/** Opens the existing entry `name` of the folder `parent` holds open, a reparse point itself. */
static naula_status open_relative(const relative_opener *opener, HANDLE parent, const WCHAR *name, DWORD name_bytes,
                                  ACCESS_MASK access, ULONG share, HANDLE *opened, unsigned long *error) {
    return disk_open_at(opener, parent, name, name_bytes, access, share, FILE_OPEN, DISK_OPEN_OPTIONS, opened, error);
}

/** FILE_RENAME_INFORMATION, which only the driver kit declares. */
typedef struct rename_information {
    BOOLEAN replace_if_exists;
    HANDLE root_directory;
    ULONG name_length;
    WCHAR name[1];
} rename_information;

#define FILE_RENAME_INFORMATION_CLASS 10

naula_status disk_rename_by_handle(const relative_opener *opener, HANDLE moving, HANDLE folder, const WCHAR *name,
                                   DWORD name_bytes, int replace, unsigned long *error) {
    size_t size = offsetof(rename_information, name) + name_bytes;
    size = size < sizeof(rename_information) ? sizeof(rename_information) : size;
    rename_information *information = naula_memory_allocate(size);
    if (information == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    memset(information, 0, size);
    information->replace_if_exists = replace ? TRUE : FALSE;
    information->root_directory = folder;
    information->name_length = name_bytes;
    memcpy(information->name, name, name_bytes);
    IO_STATUS_BLOCK io;
    LONG result = opener->set_information(moving, &io, information, (ULONG)size, FILE_RENAME_INFORMATION_CLASS);
    naula_memory_release(information);
    if (result < 0) {
        *error = opener->to_error(result);
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

static int is_unsupported_error(DWORD code) {
    return code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION;
}

static naula_status set_plain_attributes(HANDLE handle, DWORD attributes, unsigned long *error) {
    FILE_BASIC_INFO basic;
    memset(&basic, 0, sizeof basic);
    basic.FileAttributes = attributes & ~(DWORD)(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY |
                                                 FILE_ATTRIBUTE_REPARSE_POINT);
    if (basic.FileAttributes == 0) {
        basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    }
    return SetFileInformationByHandle(handle, FileBasicInfo, &basic, sizeof basic) ? NAULA_OK : refused(error);
}

/**
 * Removes the entry `handle` holds, by that handle. Read-only is ignored rather than cleared, since a file's
 * attributes belong to every hard link of it, one outside the clone included.
 *
 * @implNote A file system without the extended disposition (FAT, Windows before 10 1709) is removed the old way, and
 * there a read-only file's attribute is cleared first, which also clears it on its other hard links.
 */
naula_status disk_delete_by_handle(HANDLE handle, DWORD attributes, unsigned long *error) {
    FILE_DISPOSITION_INFO_EX extended;
    extended.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                     FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
    if (SetFileInformationByHandle(handle, FileDispositionInfoEx, &extended, sizeof extended)) {
        return NAULA_OK;
    }
    DWORD code = GetLastError();
    if (!is_unsupported_error(code)) {
        *error = code;
        return NAULA_REFUSED;
    }
    if (attributes & FILE_ATTRIBUTE_READONLY) {
        naula_status status = set_plain_attributes(handle, attributes, error);
        if (status != NAULA_OK) {
            return status;
        }
    }
    FILE_DISPOSITION_INFO plain;
    plain.DeleteFile = TRUE;
    return SetFileInformationByHandle(handle, FileDispositionInfo, &plain, sizeof plain) ? NAULA_OK : refused(error);
}

static naula_status attributes_of(HANDLE handle, DWORD *attributes, unsigned long *error) {
    BY_HANDLE_FILE_INFORMATION information;
    if (!GetFileInformationByHandle(handle, &information)) {
        return refused(error);
    }
    *attributes = information.dwFileAttributes;
    return NAULA_OK;
}

static int is_real_folder(DWORD attributes) {
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

static int is_dot_entry(const WCHAR *name, DWORD name_bytes) {
    return (name_bytes == sizeof(WCHAR) && name[0] == L'.') ||
           (name_bytes == 2 * sizeof(WCHAR) && name[0] == L'.' && name[1] == L'.');
}

static int is_gone_error(unsigned long code) {
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

/**
 * The entries of one listing, each a kind mark, LISTED_FOLDER or LISTED_OTHER, then its name and a NUL. The mark only
 * chooses how the entry is first opened; what it is, is judged by its own handle.
 */
typedef struct listing_snapshot {
    WCHAR *entries;
    size_t length;
    size_t capacity;
    size_t count;
} listing_snapshot;

#define LISTED_FOLDER L'd'
#define LISTED_OTHER L'f'

/**
 * One folder being emptied: its handle, the snapshot of its entries being worked through from `next`, whether a
 * snapshot was ever taken, and how many were found holding entries after the first was worked through.
 */
typedef struct removal_folder {
    HANDLE handle;
    listing_snapshot listing;
    size_t next;
    int listed;
    unsigned refills;
} removal_folder;

/** The folders being emptied, the clone at the bottom, each held open by its handle; never the call stack. */
typedef struct removal {
    relative_opener opener;
    removal_folder *folders;
    size_t depth;
    size_t capacity;
    unsigned char *buffer;
    disk_removal_refusal reason;
} removal;

#ifdef NAULA_TEST_HOOKS
static size_t test_depth_limit;

void naula_disk_test_depth_limit(size_t levels) {
    test_depth_limit = levels;
}
#else
#define test_depth_limit 0
#endif

size_t disk_removal_depth_limit(void) {
    return test_depth_limit != 0 ? test_depth_limit : 32767 / 2;
}

/** Takes the folder below the top of the stack; one more levels below the clone than any checkout is refused. */
static naula_status push_folder(removal *removing, HANDLE folder, unsigned long *error) {
    if (removing->depth > disk_removal_depth_limit()) {
        removing->reason = DISK_REMOVAL_TOO_DEEP;
        *error = ERROR_FILENAME_EXCED_RANGE;
        return NAULA_REFUSED;
    }
    if (removing->depth == removing->capacity) {
        size_t grown = removing->capacity == 0 ? 64 : removing->capacity * 2;
        removal_folder *larger = naula_memory_reallocate(removing->folders, grown * sizeof *larger);
        if (larger == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        removing->folders = larger;
        removing->capacity = grown;
    }
    removal_folder *pushed = &removing->folders[removing->depth++];
    memset(pushed, 0, sizeof *pushed);
    pushed->handle = folder;
    return NAULA_OK;
}

static void pop_folder(removal *removing) {
    removal_folder *top = &removing->folders[--removing->depth];
    CloseHandle(top->handle);
    naula_memory_release(top->listing.entries);
}

/**
 * Every open a removal makes of something not listed as a folder: DELETE and the attributes only, never the reading
 * right a folder's listing needs, so a file another program holds open without sharing reads can still go.
 */
#define FILE_REMOVAL_ACCESS (DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE)

/**
 * Opens the listed entry and reads its attributes by its own handle, never by the listing, which another process may
 * have made stale: one listed as something else that turns out a real folder is opened again with the folder's rights.
 */
static naula_status open_entry(removal *removing, HANDLE folder, int listed_as_folder, const WCHAR *name,
                               DWORD name_bytes, HANDLE *child, DWORD *attributes, unsigned long *error) {
    for (int as_folder = listed_as_folder;; as_folder = 1) {
        naula_status status = open_relative(&removing->opener, folder, name, name_bytes,
                                            as_folder ? REMOVAL_ACCESS : FILE_REMOVAL_ACCESS, REMOVAL_SHARE, child,
                                            error);
        if (status == NAULA_OK) {
            status = attributes_of(*child, attributes, error);
        }
        if (status != NAULA_OK || as_folder || !is_real_folder(*attributes)) {
            return status;
        }
        CloseHandle(*child);
        *child = INVALID_HANDLE_VALUE;
    }
}

/**
 * Takes the listed entry: a real folder is handed back in `descend` to be emptied first; anything else, a link or
 * other reparse point included, is removed itself.
 */
static naula_status take_entry(removal *removing, HANDLE folder, int listed_as_folder, const WCHAR *name,
                               DWORD name_bytes, HANDLE *descend, unsigned long *error) {
    HANDLE child = INVALID_HANDLE_VALUE;
    DWORD attributes = 0;
    if (listed_as_folder) {
        test_reach_wide(DISK_TEST_BEFORE_ENTRY_OPEN, name);
    }
    naula_status status = open_entry(removing, folder, listed_as_folder, name, name_bytes, &child, &attributes, error);
    if (child == INVALID_HANDLE_VALUE) {
        return status != NAULA_OK && is_gone_error(*error) ? NAULA_OK : status;
    }
    if (status == NAULA_OK && is_real_folder(attributes)) {
        status = attributes & FILE_ATTRIBUTE_READONLY ? set_plain_attributes(child, attributes, error) : NAULA_OK;
        if (status == NAULA_OK) {
            *descend = child;
            return NAULA_OK;
        }
    } else if (status == NAULA_OK) {
        status = disk_delete_by_handle(child, attributes, error);
    }
    CloseHandle(child);
    return status;
}

#ifdef NAULA_TEST_HOOKS
static size_t test_names_listed;

size_t naula_disk_test_names_listed(void) {
    return test_names_listed;
}
#endif

static naula_status snapshot_append(listing_snapshot *taken, WCHAR mark, const WCHAR *name, size_t characters) {
#ifdef NAULA_TEST_HOOKS
    test_names_listed++;
#endif
    size_t needed = characters + 2;
    if (taken->length + needed > taken->capacity) {
        size_t grown = taken->capacity == 0 ? 64 : taken->capacity * 2;
        while (grown < taken->length + needed) {
            grown *= 2;
        }
        WCHAR *larger = naula_memory_reallocate(taken->entries, grown * sizeof *larger);
        if (larger == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        taken->entries = larger;
        taken->capacity = grown;
    }
    taken->entries[taken->length] = mark;
    wmemcpy(taken->entries + taken->length + 1, name, characters);
    taken->entries[taken->length + 1 + characters] = L'\0';
    taken->length += needed;
    taken->count++;
    return NAULA_OK;
}

/**
 * Every entry in `folder`, read before any is acted on: a listing that removes as it reads can be fed new names behind
 * its cursor for as long as another program writes, so a pass would never end.
 */
static naula_status read_snapshot(removal *removing, HANDLE folder, listing_snapshot *taken, unsigned long *error) {
    FILE_INFO_BY_HANDLE_CLASS query = FileIdBothDirectoryRestartInfo;
    for (;;) {
        if (!GetFileInformationByHandleEx(folder, query, removing->buffer, LISTING_BUFFER_SIZE)) {
            DWORD code = GetLastError();
            *error = code;
            return code == ERROR_NO_MORE_FILES ? NAULA_OK : NAULA_REFUSED;
        }
        query = FileIdBothDirectoryInfo;
        for (size_t offset = 0;;) {
            const FILE_ID_BOTH_DIR_INFO *entry = (const FILE_ID_BOTH_DIR_INFO *)(removing->buffer + offset);
            if (!is_dot_entry(entry->FileName, entry->FileNameLength)) {
                WCHAR mark = entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY ? LISTED_FOLDER : LISTED_OTHER;
                naula_status status =
                    snapshot_append(taken, mark, entry->FileName, entry->FileNameLength / sizeof(WCHAR));
                if (status != NAULA_OK) {
                    return status;
                }
            }
            if (entry->NextEntryOffset == 0) {
                break;
            }
            offset += entry->NextEntryOffset;
        }
    }
}

/** Removes the emptied top folder and pops it; one that something refilled first stays, with `refilled` set. */
static naula_status remove_top_folder(removal *removing, int *refilled, unsigned long *error) {
    *refilled = 0;
    HANDLE folder = removing->folders[removing->depth - 1].handle;
    DWORD attributes = 0;
    naula_status status = attributes_of(folder, &attributes, error);
    if (status == NAULA_OK) {
        status = disk_delete_by_handle(folder, attributes, error);
    }
    if (status == NAULA_REFUSED && *error == ERROR_DIR_NOT_EMPTY) {
        *refilled = 1;
        return NAULA_OK;
    }
    pop_folder(removing);
    return status;
}

/**
 * A folder found holding entries after a snapshot of it was worked through was refilled by another program. One
 * holding more than the snapshot before it is refilled faster than it is emptied, which no number of listings outruns.
 */
static naula_status count_refill(removal *removing, removal_folder *top, int outpaced, unsigned long *error) {
    if (++top->refills < DISK_REMOVAL_REFILL_PASSES && !outpaced) {
        return NAULA_OK;
    }
    removing->reason = DISK_REMOVAL_REFILLED;
    *error = ERROR_DIR_NOT_EMPTY;
    return NAULA_REFUSED;
}

/** Takes a new snapshot of the top folder once the last is worked through, and removes the folder when it is empty. */
static naula_status relist_top(removal *removing, unsigned long *error) {
    removal_folder *top = &removing->folders[removing->depth - 1];
    int worked_through = top->listed;
    size_t previous = top->listing.count;
    top->listed = 1;
    top->listing.length = 0;
    top->listing.count = 0;
    top->next = 0;
    naula_status status = read_snapshot(removing, top->handle, &top->listing, error);
    if (status != NAULA_OK) {
        return status;
    }
    if (top->listing.length > 0) {
        return worked_through ? count_refill(removing, top, top->listing.count > previous, error) : NAULA_OK;
    }
    int refilled = 0;
    status = remove_top_folder(removing, &refilled, error);
    return status == NAULA_OK && refilled ? count_refill(removing, top, 0, error) : status;
}

/**
 * One step of the walk: the top folder's next entry goes, or is pushed when it is a real folder, so the folder below
 * carries on with its own snapshot once that one is removed; a snapshot worked through is followed by a new one.
 */
static naula_status take_one_step(removal *removing, unsigned long *error) {
    removal_folder *top = &removing->folders[removing->depth - 1];
    if (top->next >= top->listing.length) {
        return relist_top(removing, error);
    }
    const WCHAR *entry = top->listing.entries + top->next;
    size_t characters = wcslen(entry + 1);
    top->next += characters + 2;
    HANDLE descend = INVALID_HANDLE_VALUE;
    naula_status status = take_entry(removing, top->handle, entry[0] == LISTED_FOLDER, entry + 1,
                                     (DWORD)(characters * sizeof(WCHAR)), &descend, error);
    if (status != NAULA_OK || descend == INVALID_HANDLE_VALUE) {
        return status;
    }
    status = push_folder(removing, descend, error);
    if (status != NAULA_OK) {
        CloseHandle(descend);
    }
    return status;
}

/** Empties and removes the folder `clone` holds, depth first; takes ownership of the handle. */
static naula_status remove_held_tree(removal *removing, HANDLE clone, unsigned long *error) {
    removing->buffer = naula_memory_allocate(LISTING_BUFFER_SIZE);
    naula_status status = removing->buffer == NULL ? NAULA_OUT_OF_MEMORY : push_folder(removing, clone, error);
    if (status != NAULA_OK) {
        CloseHandle(clone);
    }
    while (status == NAULA_OK && removing->depth > 0) {
        status = take_one_step(removing, error);
    }
    while (removing->depth > 0) {
        pop_folder(removing);
    }
    naula_memory_release(removing->folders);
    naula_memory_release(removing->buffer);
    if (status == NAULA_OK) {
        *error = 0;
    }
    return status;
}

static naula_status refuse_not_a_folder(HANDLE handle, unsigned long *error) {
    DWORD attributes = 0;
    naula_status status = attributes_of(handle, &attributes, error);
    if (status == NAULA_OK && !is_real_folder(attributes)) {
        *error = ERROR_DIRECTORY;
        status = NAULA_REFUSED;
    }
    return status;
}

static naula_status open_root(const char *root, HANDLE *folder, unsigned long *error) {
    wchar_t *wide = NULL;
    naula_status status = wide_path(root, &wide);
    if (status != NAULA_OK) {
        return status;
    }
    *folder = CreateFileW(wide, WALK_ACCESS, WALK_SHARE, NULL, OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    naula_memory_release(wide);
    if (*folder == INVALID_HANDLE_VALUE) {
        return refused(error);
    }
    status = refuse_not_a_folder(*folder, error);
    if (status != NAULA_OK) {
        CloseHandle(*folder);
        *folder = INVALID_HANDLE_VALUE;
    }
    return status;
}

static int is_wide_separator(wchar_t character) {
    return character == L'/' || character == L'\\';
}

static int is_walkable_name(const wchar_t *name, size_t length) {
    return length > 0 && !(length == 1 && name[0] == L'.') && !(length == 2 && name[0] == L'.' && name[1] == L'.');
}

/**
 * Whether every component of `below` is a name NtCreateFile makes as Win32 opens it.
 *
 * @implNote A relative open hands the name to NTFS raw, so `clone.` or `CON` would be made literally, a name no Win32
 * program then opens as itself.
 */
static int is_win32_named(const char *below) {
    for (const char *component = below;;) {
        size_t length = strcspn(component, "/\\");
        if (!windows_name_is_safe(component, length)) {
            return 0;
        }
        if (component[length] == '\0') {
            return 1;
        }
        component += length + 1;
    }
}

naula_status disk_walk_to_parent(const char *root, const char *below, disk_walk_purpose purpose, disk_walk *walk,
                                 unsigned long *error) {
    (void)purpose;
    walk->parent = INVALID_HANDLE_VALUE;
    walk->below = NULL;
    walk->name = NULL;
    walk->name_bytes = 0;
    disk_test_reach(DISK_TEST_BEFORE_WALK, below);
    if (!relative_opener_load(&walk->opener)) {
        *error = ERROR_PROC_NOT_FOUND;
        return NAULA_REFUSED;
    }
    if (!is_win32_named(below)) {
        *error = ERROR_INVALID_NAME;
        return NAULA_REFUSED;
    }
    naula_status status = wide_text(below, &walk->below);
    HANDLE folder = INVALID_HANDLE_VALUE;
    if (status == NAULA_OK) {
        status = open_root(root, &folder, error);
    }
    const wchar_t *component = walk->below;
    while (status == NAULA_OK) {
        size_t length = 0;
        while (component[length] != L'\0' && !is_wide_separator(component[length])) {
            length++;
        }
        if (!is_walkable_name(component, length)) {
            *error = ERROR_INVALID_NAME;
            status = NAULA_REFUSED;
            break;
        }
        if (component[length] == L'\0') {
            walk->parent = folder;
            walk->name = component;
            walk->name_bytes = (DWORD)(length * sizeof(wchar_t));
            return NAULA_OK;
        }
        HANDLE next = INVALID_HANDLE_VALUE;
        status = open_relative(&walk->opener, folder, component, (DWORD)(length * sizeof(wchar_t)), WALK_ACCESS,
                               WALK_SHARE, &next, error);
        CloseHandle(folder);
        folder = next;
        if (status == NAULA_OK) {
            status = refuse_not_a_folder(folder, error);
        }
        component += length + 1;
    }
    if (folder != INVALID_HANDLE_VALUE) {
        CloseHandle(folder);
    }
    return status;
}

void disk_walk_release(disk_walk *walk) {
    if (walk->parent != INVALID_HANDLE_VALUE) {
        CloseHandle(walk->parent);
    }
    naula_memory_release(walk->below);
    walk->parent = INVALID_HANDLE_VALUE;
    walk->below = NULL;
}

/** Opens the folder `below` names under `root` for removal, after a walk to it that follows no reparse point. */
static naula_status walk_to_clone(const char *root, const char *below, relative_opener *opener, HANDLE *clone,
                                  unsigned long *error) {
    disk_walk walk;
    naula_status status = disk_walk_to_parent(root, below, DISK_FOR_PLAN_STEP, &walk, error);
    *opener = walk.opener;
    if (status == NAULA_OK) {
        status = open_relative(&walk.opener, walk.parent, walk.name, walk.name_bytes, REMOVAL_ACCESS, REMOVAL_SHARE,
                               clone, error);
    }
    if (status == NAULA_OK) {
        status = refuse_not_a_folder(*clone, error);
        if (status != NAULA_OK) {
            CloseHandle(*clone);
            *clone = INVALID_HANDLE_VALUE;
        }
    }
    disk_walk_release(&walk);
    return status;
}

naula_status disk_folder_make(const char *root, const char *below, disk_walk_purpose purpose, unsigned long *error) {
    *error = 0;
    disk_walk walk;
    naula_status status = disk_walk_to_parent(root, below, purpose, &walk, error);
    HANDLE made = INVALID_HANDLE_VALUE;
    if (status == NAULA_OK) {
        status = disk_open_at(&walk.opener, walk.parent, walk.name, walk.name_bytes, FILE_LIST_DIRECTORY | SYNCHRONIZE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_CREATE,
                              FILE_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT, &made,
                              error);
    }
    if (made != INVALID_HANDLE_VALUE) {
        CloseHandle(made);
    }
    disk_walk_release(&walk);
    return status;
}

#define MOVE_ACCESS (DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE)
#define MOVE_SHARE (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)

/** Opens `from` by its walked folder and renames it by its handle into the folder `to` was walked to. */
static naula_status move_walked(const disk_walk *from, const disk_walk *to, int *held, unsigned long *error) {
    HANDLE moving = INVALID_HANDLE_VALUE;
    naula_status status = open_relative(&from->opener, from->parent, from->name, from->name_bytes, MOVE_ACCESS,
                                        MOVE_SHARE, &moving, error);
    if (status == NAULA_OK) {
        status = disk_rename_by_handle(&from->opener, moving, to->parent, to->name, to->name_bytes, 0, error);
        CloseHandle(moving);
    }
    *held = status == NAULA_REFUSED && is_held_error(*error);
    return status;
}

naula_status disk_move(const char *root, const char *from, const char *to, disk_walk_purpose purpose, int *held,
                       unsigned long *error) {
    *held = 0;
    *error = 0;
    disk_walk source;
    disk_walk target;
    naula_status status = disk_walk_to_parent(root, from, purpose, &source, error);
    target.parent = INVALID_HANDLE_VALUE;
    target.below = NULL;
    if (status == NAULA_OK) {
        status = disk_walk_to_parent(root, to, purpose, &target, error);
    }
    if (status == NAULA_OK) {
        status = move_walked(&source, &target, held, error);
    }
    disk_walk_release(&target);
    disk_walk_release(&source);
    return status;
}

static int is_removal_held_error(unsigned long code) {
    return code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
}

naula_status disk_tree_remove(const char *root, const char *below, disk_removal_refusal *reason,
                              unsigned long *error) {
    *reason = DISK_REMOVAL_REFUSED;
    *error = 0;
    removal removing;
    memset(&removing, 0, sizeof removing);
    HANDLE clone = INVALID_HANDLE_VALUE;
    naula_status status = walk_to_clone(root, below, &removing.opener, &clone, error);
    if (status == NAULA_OK) {
        status = remove_held_tree(&removing, clone, error);
    }
    *reason = removing.reason;
    if (status == NAULA_REFUSED && *reason == DISK_REMOVAL_REFUSED && is_removal_held_error(*error)) {
        *reason = DISK_REMOVAL_HELD;
    }
    return status;
}

static int is_absent_error(DWORD code) {
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND || code == ERROR_INVALID_NAME ||
           code == ERROR_BAD_PATHNAME;
}

/** The 128-bit id where the file system has one (ReFS needs it), else the volume serial and 64-bit file index. */
static naula_status identity_of_handle(HANDLE handle, disk_identity *identity, unsigned long *error) {
    FILE_ID_INFO wide_id;
    if (GetFileInformationByHandleEx(handle, FileIdInfo, &wide_id, sizeof wide_id)) {
        identity->volume = wide_id.VolumeSerialNumber;
        memcpy(&identity->low, wide_id.FileId.Identifier, sizeof identity->low);
        memcpy(&identity->high, wide_id.FileId.Identifier + sizeof identity->low, sizeof identity->high);
        return NAULA_OK;
    }
    BY_HANDLE_FILE_INFORMATION information;
    if (!GetFileInformationByHandle(handle, &information)) {
        return refused(error);
    }
    identity->volume = information.dwVolumeSerialNumber;
    identity->low = ((uint64_t)information.nFileIndexHigh << 32) | information.nFileIndexLow;
    return NAULA_OK;
}

naula_status disk_identity_of(const char *path, disk_identity *identity, unsigned long *error) {
    wchar_t *wide = NULL;
    *error = 0;
    memset(identity, 0, sizeof *identity);
    naula_status status = wide_path(path, &wide);
    if (status != NAULA_OK) {
        return status;
    }
    HANDLE handle = CreateFileW(wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    DWORD code = GetLastError();
    naula_memory_release(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        *error = code;
        return is_absent_error(code) ? NAULA_NOT_FOUND : NAULA_REFUSED;
    }
    status = identity_of_handle(handle, identity, error);
    CloseHandle(handle);
    return status;
}

/** `\\?\C:\x` as `C:\x` and `\\?\UNC\host\share` as `\\host\share`, the forms Win32 opens without the prefix. */
static naula_status narrow_final_path(wchar_t *final, char **real) {
    static const wchar_t long_prefix[] = L"\\\\?\\";
    static const wchar_t unc_prefix[] = L"\\\\?\\UNC\\";
    size_t unc_length = wcslen(unc_prefix);
    if (wcsncmp(final, unc_prefix, unc_length) == 0) {
        final[unc_length - 2] = L'\\';
        return narrow_text(final + unc_length - 2, real);
    }
    size_t long_length = wcslen(long_prefix);
    return narrow_text(wcsncmp(final, long_prefix, long_length) == 0 ? final + long_length : final, real);
}

static naula_status final_path_of(HANDLE folder, char **real, unsigned long *error) {
    DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    DWORD needed = GetFinalPathNameByHandleW(folder, NULL, 0, flags);
    if (needed == 0) {
        return refused(error);
    }
    wchar_t *final = naula_memory_allocate((size_t)needed * sizeof(wchar_t));
    if (final == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    DWORD written = GetFinalPathNameByHandleW(folder, final, needed, flags);
    naula_status status = written == 0 || written >= needed ? refused(error) : narrow_final_path(final, real);
    naula_memory_release(final);
    return status;
}

naula_status disk_real_folder(const char *path, char **real, unsigned long *error) {
    *real = NULL;
    *error = 0;
    wchar_t *wide = NULL;
    naula_status status = wide_path(path, &wide);
    if (status != NAULA_OK) {
        return status;
    }
    HANDLE folder = CreateFileW(wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    DWORD code = GetLastError();
    naula_memory_release(wide);
    if (folder == INVALID_HANDLE_VALUE) {
        *error = code;
        return is_absent_error(code) ? NAULA_NOT_FOUND : NAULA_REFUSED;
    }
    status = refuse_not_a_folder(folder, error);
    if (status == NAULA_OK) {
        status = final_path_of(folder, real, error);
    }
    CloseHandle(folder);
    return status;
}

static naula_status read_handle(HANDLE file, uint8_t **bytes, size_t *length, unsigned long *error) {
    BY_HANDLE_FILE_INFORMATION information;
    if (!GetFileInformationByHandle(file, &information)) {
        return refused(error);
    }
    if ((information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        information.nFileSizeHigh != 0 || information.nFileSizeLow > FILE_LIMIT) {
        *error = ERROR_FILE_INVALID;
        return NAULA_REFUSED;
    }
    size_t expected = information.nFileSizeLow;
    *bytes = naula_memory_allocate(expected + 1);
    if (*bytes == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    size_t total = 0;
    while (total < expected) {
        DWORD read = 0;
        if (!ReadFile(file, *bytes + total, (DWORD)(expected - total), &read, NULL) || read == 0) {
            break;
        }
        total += read;
    }
    if (total != expected) {
        naula_memory_release(*bytes);
        *bytes = NULL;
        *error = ERROR_READ_FAULT;
        return NAULA_REFUSED;
    }
    (*bytes)[total] = 0;
    *length = total;
    return NAULA_OK;
}

naula_status disk_file_read(const char *path, uint8_t **bytes, size_t *length, unsigned long *error) {
    wchar_t *wide = NULL;
    *bytes = NULL;
    *length = 0;
    *error = 0;
    disk_test_reach(DISK_TEST_BEFORE_FILE_OPEN, path);
    naula_status status = wide_path(path, &wide);
    if (status != NAULA_OK) {
        return status;
    }
    HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    DWORD code = GetLastError();
    naula_memory_release(wide);
    if (file == INVALID_HANDLE_VALUE) {
        *error = code;
        return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? NAULA_NOT_FOUND : NAULA_REFUSED;
    }
    status = read_handle(file, bytes, length, error);
    CloseHandle(file);
    return status;
}

static int write_all(HANDLE file, const uint8_t *bytes, size_t length) {
    size_t total = 0;
    while (total < length) {
        DWORD chunk = (DWORD)(length - total > WRITE_CHUNK ? WRITE_CHUNK : length - total);
        DWORD written = 0;
        if (!WriteFile(file, bytes + total, chunk, &written, NULL) || written == 0) {
            return 0;
        }
        total += written;
    }
    return 1;
}

#define STALE_ACCESS (DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE)

/** A stale temporary file in the walked folder is removed by its handle; anything else there is refused. */
static naula_status clear_stale_temporary(const disk_walk *walk, const wchar_t *temporary, unsigned long *error) {
    HANDLE stale = INVALID_HANDLE_VALUE;
    naula_status status = open_relative(&walk->opener, walk->parent, temporary,
                                        (DWORD)(wcslen(temporary) * sizeof(wchar_t)), STALE_ACCESS,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &stale, error);
    if (status != NAULA_OK) {
        if (is_gone_error(*error)) {
            *error = 0;
            return NAULA_OK;
        }
        return status;
    }
    DWORD attributes = 0;
    status = attributes_of(stale, &attributes, error);
    if (status == NAULA_OK && (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        *error = ERROR_FILE_EXISTS;
        status = NAULA_REFUSED;
    } else if (status == NAULA_OK) {
        status = disk_delete_by_handle(stale, attributes, error);
    }
    CloseHandle(stale);
    return status;
}

/** Writes and flushes the new temporary file, then renames it over the walked name by its handle. */
static naula_status write_and_rename(const disk_walk *walk, const wchar_t *temporary, const uint8_t *bytes,
                                     size_t length, unsigned long *error) {
    HANDLE file = INVALID_HANDLE_VALUE;
    naula_status status = disk_open_at(&walk->opener, walk->parent, temporary,
                                       (DWORD)(wcslen(temporary) * sizeof(wchar_t)), GENERIC_WRITE | DELETE | SYNCHRONIZE,
                                       0, FILE_CREATE, FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, &file,
                                       error);
    if (status != NAULA_OK) {
        return status;
    }
    status = write_all(file, bytes, length) && FlushFileBuffers(file) ? NAULA_OK : refused(error);
    if (status == NAULA_OK) {
        status = disk_rename_by_handle(&walk->opener, file, walk->parent, walk->name, walk->name_bytes, 1, error);
    }
    if (status != NAULA_OK) {
        unsigned long ignored = 0;
        disk_delete_by_handle(file, FILE_ATTRIBUTE_NORMAL, &ignored);
    }
    CloseHandle(file);
    return status;
}

naula_status disk_file_replace(const char *root, const char *below, const uint8_t *bytes, size_t length,
                               disk_walk_purpose purpose, unsigned long *error) {
    *error = 0;
    disk_walk walk;
    naula_status status = disk_walk_to_parent(root, below, purpose, &walk, error);
    size_t characters = walk.name_bytes / sizeof(wchar_t);
    wchar_t *temporary = status == NAULA_OK ? naula_memory_allocate((characters + 11) * sizeof *temporary) : NULL;
    if (status == NAULA_OK && temporary == NULL) {
        status = NAULA_OUT_OF_MEMORY;
    }
    if (status == NAULA_OK) {
        wmemcpy(temporary, walk.name, characters);
        wmemcpy(temporary + characters, L".naula-tmp", 11);
        status = clear_stale_temporary(&walk, temporary, error);
    }
    if (status == NAULA_OK) {
        status = write_and_rename(&walk, temporary, bytes, length, error);
    }
    naula_memory_release(temporary);
    disk_walk_release(&walk);
    return status;
}

void disk_names_release(char **names, size_t count) {
    for (size_t index = 0; index < count; index++) {
        naula_memory_release(names[index]);
    }
    naula_memory_release(names);
}

static naula_status append_name(char ***names, size_t *count, size_t *capacity, const wchar_t *wide) {
    if (*count == *capacity) {
        size_t grown = *capacity == 0 ? 16 : *capacity * 2;
        char **larger = naula_memory_reallocate(*names, grown * sizeof *larger);
        if (larger == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        *names = larger;
        *capacity = grown;
    }
    char *name = NULL;
    naula_status status = narrow_text(wide, &name);
    if (status == NAULA_OK) {
        (*names)[(*count)++] = name;
    }
    return status;
}

naula_status disk_names(const char *path, char ***names, size_t *count, unsigned long *error) {
    wchar_t *wide = NULL;
    *names = NULL;
    *count = 0;
    *error = 0;
    naula_status status = wide_path(path, &wide);
    wchar_t *pattern = status == NAULA_OK ? child_path(wide, L"*") : NULL;
    naula_memory_release(wide);
    if (status != NAULA_OK || pattern == NULL) {
        return status != NAULA_OK ? status : NAULA_OUT_OF_MEMORY;
    }
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileExW(pattern, FindExInfoBasic, &found, FindExSearchNameMatch, NULL, 0);
    naula_memory_release(pattern);
    if (search == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? NAULA_OK : refused(error);
    }
    size_t capacity = 0;
    do {
        if (!is_dot_name(found.cFileName)) {
            status = append_name(names, count, &capacity, found.cFileName);
        }
    } while (status == NAULA_OK && FindNextFileW(search, &found));
    FindClose(search);
    if (status != NAULA_OK) {
        disk_names_release(*names, *count);
        *names = NULL;
        *count = 0;
    }
    return status;
}

naula_status disk_appender_open(const char *root, const char *below, int create, disk_appender **appender,
                                unsigned long *error) {
    *appender = NULL;
    *error = 0;
    disk_walk walk;
    naula_status status = disk_walk_to_parent(root, below, DISK_FOR_NAULA_FILE, &walk, error);
    HANDLE file = INVALID_HANDLE_VALUE;
    if (status == NAULA_OK) {
        status = disk_open_at(&walk.opener, walk.parent, walk.name, walk.name_bytes, FILE_APPEND_DATA | SYNCHRONIZE,
                              FILE_SHARE_READ, create ? FILE_CREATE : FILE_OPEN,
                              FILE_NON_DIRECTORY_FILE | DISK_OPEN_OPTIONS, &file, error);
    }
    disk_walk_release(&walk);
    if (status != NAULA_OK) {
        return status;
    }
    *appender = naula_memory_allocate(sizeof **appender);
    if (*appender == NULL) {
        CloseHandle(file);
        return NAULA_OUT_OF_MEMORY;
    }
    (*appender)->file = file;
    return NAULA_OK;
}

naula_status disk_append(disk_appender *appender, const char *bytes, size_t length, unsigned long *error) {
    *error = 0;
    if (!write_all(appender->file, (const uint8_t *)bytes, length) || !FlushFileBuffers(appender->file)) {
        return refused(error);
    }
    return NAULA_OK;
}

void disk_appender_close(disk_appender *appender) {
    if (appender != NULL) {
        CloseHandle(appender->file);
        naula_memory_release(appender);
    }
}

void disk_utc_stamp(char stamp[17]) {
    SYSTEMTIME now;
    GetSystemTime(&now);
    snprintf(stamp, 17, "%04u%02u%02uT%02u%02u%02uZ", (unsigned)now.wYear % 10000u, (unsigned)now.wMonth,
             (unsigned)now.wDay, (unsigned)now.wHour, (unsigned)now.wMinute, (unsigned)now.wSecond);
}

int disk_error_is_mount(unsigned long error) {
    return error == ERROR_NOT_SAME_DEVICE;
}

int disk_error_is_mount_unverifiable(unsigned long error) {
    (void)error;
    return 0;
}

void disk_error_text(unsigned long error, char *text, size_t capacity) {
    if (capacity == 0) {
        return;
    }
    char message[256];
    DWORD length = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)error, 0,
                                  message, sizeof message, NULL);
    while (length > 0 && (message[length - 1] == '\n' || message[length - 1] == '\r' || message[length - 1] == '.')) {
        length--;
    }
    message[length] = '\0';
    snprintf(text, capacity, "%s (system error %lu)", length > 0 ? message : "unknown error", error);
}

struct disk_owned_file {
    relative_opener opener;
    HANDLE folder;
    wchar_t *name;
    DWORD name_bytes;
};

/** Creates the walked name exclusively, writes and flushes `bytes`, and removes it by its handle when that fails. */
static naula_status create_owned(const disk_walk *walk, const uint8_t *bytes, size_t length, int *exists,
                                 unsigned long *error) {
    HANDLE file = INVALID_HANDLE_VALUE;
    naula_status status = disk_open_at(&walk->opener, walk->parent, walk->name, walk->name_bytes,
                                       GENERIC_WRITE | DELETE | SYNCHRONIZE, 0, FILE_CREATE,
                                       FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, &file, error);
    if (status != NAULA_OK) {
        *exists = *error == ERROR_FILE_EXISTS || *error == ERROR_ALREADY_EXISTS;
        return status;
    }
    status = write_all(file, bytes, length) && FlushFileBuffers(file) ? NAULA_OK : refused(error);
    if (status != NAULA_OK) {
        unsigned long ignored = 0;
        disk_delete_by_handle(file, FILE_ATTRIBUTE_NORMAL, &ignored);
    }
    CloseHandle(file);
    return status;
}

naula_status disk_owned_file_create(const char *root, const char *below, const uint8_t *bytes, size_t length,
                                    int *exists, disk_owned_file **owned, unsigned long *error) {
    *owned = NULL;
    *exists = 0;
    *error = 0;
    disk_owned_file *made = naula_memory_allocate(sizeof *made);
    if (made == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    disk_walk walk;
    naula_status status = disk_walk_to_parent(root, below, DISK_FOR_NAULA_FILE, &walk, error);
    made->name = status == NAULA_OK ? naula_memory_allocate(walk.name_bytes + sizeof(wchar_t)) : NULL;
    if (status == NAULA_OK && made->name == NULL) {
        status = NAULA_OUT_OF_MEMORY;
    }
    if (status == NAULA_OK) {
        memcpy(made->name, walk.name, walk.name_bytes);
        made->name[walk.name_bytes / sizeof(wchar_t)] = L'\0';
        status = create_owned(&walk, bytes, length, exists, error);
    }
    if (status != NAULA_OK) {
        naula_memory_release(made->name);
        naula_memory_release(made);
        disk_walk_release(&walk);
        return status;
    }
    made->opener = walk.opener;
    made->folder = walk.parent;
    made->name_bytes = walk.name_bytes;
    walk.parent = INVALID_HANDLE_VALUE;
    disk_walk_release(&walk);
    *owned = made;
    return NAULA_OK;
}

void disk_owned_file_remove(disk_owned_file *owned) {
    if (owned != NULL) {
        HANDLE file = INVALID_HANDLE_VALUE;
        unsigned long error = 0;
        if (open_relative(&owned->opener, owned->folder, owned->name, owned->name_bytes, STALE_ACCESS,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &file, &error) == NAULA_OK) {
            disk_delete_by_handle(file, FILE_ATTRIBUTE_NORMAL, &error);
            CloseHandle(file);
        }
    }
    disk_owned_file_abandon(owned);
}

void disk_owned_file_abandon(disk_owned_file *owned) {
    if (owned != NULL) {
        CloseHandle(owned->folder);
        naula_memory_release(owned->name);
        naula_memory_release(owned);
    }
}

unsigned long disk_process_id(void) {
    return (unsigned long)GetCurrentProcessId();
}

int disk_process_is_alive(unsigned long process_id) {
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)process_id);
    if (process == NULL) {
        return GetLastError() != ERROR_INVALID_PARAMETER;
    }
    DWORD waited = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return waited == WAIT_TIMEOUT;
}
