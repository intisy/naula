#include <windows.h>

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

static naula_status remove_tree_wide(const wchar_t *folder, unsigned long *error);

/** A link or other reparse point is removed itself and never entered, whatever it points at. */
static naula_status remove_entry(const wchar_t *path, DWORD attributes, unsigned long *error) {
    int is_folder = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        return (is_folder ? RemoveDirectoryW(path) : DeleteFileW(path)) ? NAULA_OK : refused(error);
    }
    if (is_folder) {
        return remove_tree_wide(path, error);
    }
    if (attributes & FILE_ATTRIBUTE_READONLY) {
        SetFileAttributesW(path, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    }
    return DeleteFileW(path) ? NAULA_OK : refused(error);
}

static naula_status remove_children(const wchar_t *folder, unsigned long *error) {
    wchar_t *pattern = child_path(folder, L"*");
    if (pattern == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileExW(pattern, FindExInfoBasic, &found, FindExSearchNameMatch, NULL, 0);
    naula_memory_release(pattern);
    if (search == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? NAULA_OK : refused(error);
    }
    naula_status status = NAULA_OK;
    do {
        if (is_dot_name(found.cFileName)) {
            continue;
        }
        wchar_t *child = child_path(folder, found.cFileName);
        status = child == NULL ? NAULA_OUT_OF_MEMORY : remove_entry(child, found.dwFileAttributes, error);
        naula_memory_release(child);
    } while (status == NAULA_OK && FindNextFileW(search, &found));
    FindClose(search);
    return status;
}

static naula_status remove_tree_wide(const wchar_t *folder, unsigned long *error) {
    naula_status status = remove_children(folder, error);
    if (status == NAULA_OK && !RemoveDirectoryW(folder)) {
        status = refused(error);
    }
    return status;
}

naula_status disk_tree_remove(const char *path, unsigned long *error) {
    wchar_t *wide = NULL;
    *error = 0;
    naula_status status = wide_path(path, &wide);
    if (status != NAULA_OK) {
        return status;
    }
    DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        status = refused(error);
    } else if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        *error = ERROR_DIRECTORY;
        status = NAULA_REFUSED;
    } else {
        status = remove_tree_wide(wide, error);
    }
    naula_memory_release(wide);
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
