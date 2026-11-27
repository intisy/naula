#include <windows.h>
#include <winternl.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "disk.h"
#include "naula/memory.h"
#include "wide_path.h"

#define FILE_LIMIT (16 * 1024 * 1024)
#define WRITE_CHUNK 65536u

struct disk_appender {
    HANDLE file;
};

static naula_status refused(unsigned long *error) {
    *error = GetLastError();
    return NAULA_REFUSED;
}

naula_status disk_folder_make(const char *path, unsigned long *error) {
    wchar_t *wide = NULL;
    *error = 0;
    naula_status status = wide_path(path, &wide);
    if (status == NAULA_OK && !CreateDirectoryW(wide, NULL)) {
        status = refused(error);
    }
    naula_memory_release(wide);
    return status;
}

static int is_held_error(DWORD code) {
    return code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
}

naula_status disk_move(const char *from, const char *to, int *held, unsigned long *error) {
    wchar_t *wide_from = NULL;
    wchar_t *wide_to = NULL;
    *held = 0;
    *error = 0;
    naula_status status = wide_path(from, &wide_from);
    if (status == NAULA_OK) {
        status = wide_path(to, &wide_to);
    }
    if (status == NAULA_OK && !MoveFileExW(wide_from, wide_to, MOVEFILE_WRITE_THROUGH)) {
        status = refused(error);
        *held = is_held_error(*error);
    }
    naula_memory_release(wide_to);
    naula_memory_release(wide_from);
    return status;
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
#define LISTING_BUFFER_SIZE 65536u
#define LISTING_PASSES 8

typedef LONG(NTAPI *nt_create_file_function)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                             PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef ULONG(NTAPI *nt_status_to_error_function)(LONG);

typedef struct relative_opener {
    nt_create_file_function create;
    nt_status_to_error_function to_error;
} relative_opener;

/** ntdll is mapped into every process, so its two entry points are looked up rather than linked. */
static int relative_opener_load(relative_opener *opener) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    opener->create = ntdll == NULL ? NULL : (nt_create_file_function)(void (*)(void))GetProcAddress(ntdll, "NtCreateFile");
    opener->to_error =
        ntdll == NULL ? NULL : (nt_status_to_error_function)(void (*)(void))GetProcAddress(ntdll, "RtlNtStatusToDosError");
    return opener->create != NULL && opener->to_error != NULL;
}

/** Opens the entry `name` of the folder `parent` holds open, by that handle, so no path above it is walked again. */
static naula_status open_relative(const relative_opener *opener, HANDLE parent, const WCHAR *name, DWORD name_bytes,
                                  HANDLE *opened, unsigned long *error) {
    UNICODE_STRING relative;
    relative.Buffer = (PWSTR)name;
    relative.Length = (USHORT)name_bytes;
    relative.MaximumLength = (USHORT)name_bytes;
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &relative, OBJ_CASE_INSENSITIVE, parent, NULL);
    IO_STATUS_BLOCK io;
    LONG result = opener->create(opened, REMOVAL_ACCESS, &attributes, &io, NULL, 0, REMOVAL_SHARE, FILE_OPEN,
                                 FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT,
                                 NULL, 0);
    if (result < 0) {
        *opened = INVALID_HANDLE_VALUE;
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
static naula_status delete_by_handle(HANDLE handle, DWORD attributes, unsigned long *error) {
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

static int is_dot_entry(const WCHAR *name, DWORD name_bytes) {
    return (name_bytes == sizeof(WCHAR) && name[0] == L'.') ||
           (name_bytes == 2 * sizeof(WCHAR) && name[0] == L'.' && name[1] == L'.');
}

static naula_status remove_folder_contents(const relative_opener *opener, HANDLE folder, unsigned long *error);

/**
 * Removes what `handle` holds, judged by the handle itself and never by a listing, which another process may have
 * made stale: a link or other reparse point is removed itself and never entered.
 */
static naula_status remove_opened(const relative_opener *opener, HANDLE handle, unsigned long *error) {
    DWORD attributes = 0;
    naula_status status = attributes_of(handle, &attributes, error);
    int enters = status == NAULA_OK && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
                 !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
    if (enters && (attributes & FILE_ATTRIBUTE_READONLY)) {
        status = set_plain_attributes(handle, attributes, error);
    }
    if (status == NAULA_OK && enters) {
        status = remove_folder_contents(opener, handle, error);
    }
    if (status == NAULA_OK) {
        status = delete_by_handle(handle, attributes, error);
    }
    return status;
}

static int is_gone_error(unsigned long code) {
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

static naula_status remove_child(const relative_opener *opener, HANDLE folder, const FILE_ID_BOTH_DIR_INFO *entry,
                                 unsigned long *error) {
    HANDLE child = INVALID_HANDLE_VALUE;
    naula_status status = open_relative(opener, folder, entry->FileName, entry->FileNameLength, &child, error);
    if (status != NAULA_OK) {
        return is_gone_error(*error) ? NAULA_OK : status;
    }
    status = remove_opened(opener, child, error);
    CloseHandle(child);
    return status;
}

/** One listing of `folder` from its start, removing every entry it returns; `found` counts them. */
static naula_status remove_listed(const relative_opener *opener, HANDLE folder, unsigned char *buffer,
                                  size_t *found, unsigned long *error) {
    *found = 0;
    FILE_INFO_BY_HANDLE_CLASS query = FileIdBothDirectoryRestartInfo;
    for (;;) {
        if (!GetFileInformationByHandleEx(folder, query, buffer, LISTING_BUFFER_SIZE)) {
            DWORD code = GetLastError();
            *error = code;
            return code == ERROR_NO_MORE_FILES ? NAULA_OK : NAULA_REFUSED;
        }
        query = FileIdBothDirectoryInfo;
        for (size_t offset = 0;;) {
            const FILE_ID_BOTH_DIR_INFO *entry = (const FILE_ID_BOTH_DIR_INFO *)(buffer + offset);
            if (!is_dot_entry(entry->FileName, entry->FileNameLength)) {
                (*found)++;
                naula_status status = remove_child(opener, folder, entry, error);
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

/** Lists again until a listing finds nothing, since a folder may still change while naula empties it. */
static naula_status remove_folder_contents(const relative_opener *opener, HANDLE folder, unsigned long *error) {
    unsigned char *buffer = naula_memory_allocate(LISTING_BUFFER_SIZE);
    if (buffer == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    naula_status status = NAULA_OK;
    size_t found = 1;
    for (int pass = 0; status == NAULA_OK && found > 0 && pass < LISTING_PASSES; pass++) {
        status = remove_listed(opener, folder, buffer, &found, error);
    }
    naula_memory_release(buffer);
    if (status == NAULA_OK) {
        *error = 0;
    }
    return status;
}

naula_status disk_tree_remove(const char *path, unsigned long *error) {
    wchar_t *wide = NULL;
    *error = 0;
    relative_opener opener;
    if (!relative_opener_load(&opener)) {
        *error = ERROR_PROC_NOT_FOUND;
        return NAULA_REFUSED;
    }
    naula_status status = wide_path(path, &wide);
    if (status != NAULA_OK) {
        return status;
    }
    HANDLE folder = CreateFileW(wide, REMOVAL_ACCESS, REMOVAL_SHARE, NULL, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    naula_memory_release(wide);
    if (folder == INVALID_HANDLE_VALUE) {
        return refused(error);
    }
    DWORD attributes = 0;
    status = attributes_of(folder, &attributes, error);
    if (status == NAULA_OK &&
        ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(attributes & FILE_ATTRIBUTE_DIRECTORY))) {
        *error = ERROR_DIRECTORY;
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK) {
        status = remove_opened(&opener, folder, error);
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

static naula_status clear_stale_temporary(const wchar_t *temporary, unsigned long *error) {
    DWORD attributes = GetFileAttributesW(temporary);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return NAULA_OK;
    }
    if (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) {
        *error = ERROR_FILE_EXISTS;
        return NAULA_REFUSED;
    }
    return DeleteFileW(temporary) ? NAULA_OK : refused(error);
}

static naula_status write_temporary(const wchar_t *temporary, const uint8_t *bytes, size_t length,
                                    unsigned long *error) {
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return refused(error);
    }
    naula_status status = write_all(file, bytes, length) && FlushFileBuffers(file) ? NAULA_OK : refused(error);
    CloseHandle(file);
    if (status != NAULA_OK) {
        DeleteFileW(temporary);
    }
    return status;
}

naula_status disk_file_replace(const char *path, const uint8_t *bytes, size_t length, unsigned long *error) {
    wchar_t *wide = NULL;
    wchar_t *temporary = NULL;
    *error = 0;
    naula_status status = wide_path(path, &wide);
    if (status == NAULA_OK) {
        size_t wide_length = wcslen(wide);
        temporary = naula_memory_allocate((wide_length + 11) * sizeof *temporary);
        status = temporary == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
        if (temporary != NULL) {
            wmemcpy(temporary, wide, wide_length);
            wmemcpy(temporary + wide_length, L".naula-tmp", 11);
        }
    }
    if (status == NAULA_OK) {
        status = clear_stale_temporary(temporary, error);
    }
    if (status == NAULA_OK) {
        status = write_temporary(temporary, bytes, length, error);
    }
    if (status == NAULA_OK && !MoveFileExW(temporary, wide, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        status = refused(error);
        DeleteFileW(temporary);
    }
    naula_memory_release(temporary);
    naula_memory_release(wide);
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

naula_status disk_appender_open(const char *path, int create, disk_appender **appender, unsigned long *error) {
    wchar_t *wide = NULL;
    *appender = NULL;
    *error = 0;
    naula_status status = wide_path(path, &wide);
    if (status != NAULA_OK) {
        return status;
    }
    HANDLE file = CreateFileW(wide, FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ, NULL,
                              create ? CREATE_NEW : OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    naula_memory_release(wide);
    if (file == INVALID_HANDLE_VALUE) {
        return refused(error);
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
    wchar_t *wide;
};

naula_status disk_owned_file_create(const char *path, const uint8_t *bytes, size_t length, int *exists,
                                    disk_owned_file **owned, unsigned long *error) {
    wchar_t *wide = NULL;
    *owned = NULL;
    *exists = 0;
    *error = 0;
    naula_status status = wide_path(path, &wide);
    disk_owned_file *made = status == NAULA_OK ? naula_memory_allocate(sizeof *made) : NULL;
    if (status == NAULA_OK && made == NULL) {
        status = NAULA_OUT_OF_MEMORY;
    }
    HANDLE file = status == NAULA_OK ? CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL)
                                     : INVALID_HANDLE_VALUE;
    if (status == NAULA_OK && file == INVALID_HANDLE_VALUE) {
        status = refused(error);
        *exists = *error == ERROR_FILE_EXISTS || *error == ERROR_ALREADY_EXISTS;
    } else if (status == NAULA_OK) {
        status = write_all(file, bytes, length) && FlushFileBuffers(file) ? NAULA_OK : refused(error);
        CloseHandle(file);
        if (status != NAULA_OK) {
            DeleteFileW(wide);
        }
    }
    if (status != NAULA_OK) {
        naula_memory_release(made);
        naula_memory_release(wide);
        return status;
    }
    made->wide = wide;
    *owned = made;
    return NAULA_OK;
}

void disk_owned_file_remove(disk_owned_file *owned) {
    if (owned != NULL) {
        DeleteFileW(owned->wide);
    }
    disk_owned_file_abandon(owned);
}

void disk_owned_file_abandon(disk_owned_file *owned) {
    if (owned != NULL) {
        naula_memory_release(owned->wide);
        naula_memory_release(owned);
    }
}

naula_status disk_file_remove(const char *path, unsigned long *error) {
    wchar_t *wide = NULL;
    *error = 0;
    naula_status status = wide_path(path, &wide);
    if (status != NAULA_OK) {
        return status;
    }
    DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        status = refused(error);
    } else if (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) {
        *error = ERROR_FILE_INVALID;
        status = NAULA_REFUSED;
    } else if (!DeleteFileW(wide)) {
        status = refused(error);
    }
    naula_memory_release(wide);
    return status;
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
