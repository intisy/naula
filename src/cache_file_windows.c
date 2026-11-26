#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "cache_internal.h"
#include "naula/memory.h"

#define CACHE_FILE_LIMIT (1024 * 1024)
#define WRITE_CHUNK 65536u

static naula_status widen(const char *text, wchar_t **wide) {
    *wide = NULL;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (count <= 0) {
        return NAULA_REFUSED;
    }
    *wide = naula_memory_allocate((size_t)count * sizeof **wide);
    if (*wide == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, *wide, count) != count) {
        naula_memory_release(*wide);
        *wide = NULL;
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

static naula_status read_handle(HANDLE file, uint8_t **bytes, size_t *length) {
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > CACHE_FILE_LIMIT) {
        return NAULA_REFUSED;
    }
    size_t expected = (size_t)size.QuadPart;
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
        return NAULA_REFUSED;
    }
    *length = total;
    return NAULA_OK;
}

naula_status naula_cache_file_read(const char *path, uint8_t **bytes, size_t *length) {
    wchar_t *wide = NULL;
    *bytes = NULL;
    *length = 0;
    naula_status status = widen(path, &wide);
    if (status != NAULA_OK) {
        return status;
    }
    HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD error = GetLastError();
    naula_memory_release(wide);
    if (file == INVALID_HANDLE_VALUE) {
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? NAULA_NOT_FOUND : NAULA_REFUSED;
    }
    status = read_handle(file, bytes, length);
    CloseHandle(file);
    return status;
}

static int make_directory(const wchar_t *path) {
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int write_all(HANDLE file, const uint8_t *bytes, size_t length) {
    size_t total = 0;
    while (total < length) {
        DWORD chunk = (DWORD)(length - total < WRITE_CHUNK ? length - total : WRITE_CHUNK);
        DWORD written = 0;
        if (!WriteFile(file, bytes + total, chunk, &written, NULL) || written == 0) {
            return 0;
        }
        total += written;
    }
    return FlushFileBuffers(file) != 0;
}

typedef struct replace_paths {
    wchar_t *directory;
    wchar_t *identities;
    wchar_t *temporary;
    wchar_t *target;
} replace_paths;

static void release_paths(replace_paths *paths) {
    naula_memory_release(paths->directory);
    naula_memory_release(paths->identities);
    naula_memory_release(paths->temporary);
    naula_memory_release(paths->target);
}

static naula_status widen_paths(const char *directory, const char *path, replace_paths *paths) {
    static volatile LONG counter;
    char identities[MAX_PATH * 4];
    char temporary[MAX_PATH * 4];
    memset(paths, 0, sizeof *paths);
    if (snprintf(identities, sizeof identities, "%s/identities", directory) >= (int)sizeof identities ||
        snprintf(temporary, sizeof temporary, "%s.%lu.%ld.tmp", path, (unsigned long)GetCurrentProcessId(),
                 (long)InterlockedIncrement(&counter)) >= (int)sizeof temporary) {
        return NAULA_REFUSED;
    }
    naula_status status = widen(directory, &paths->directory);
    if (status == NAULA_OK) {
        status = widen(identities, &paths->identities);
    }
    if (status == NAULA_OK) {
        status = widen(temporary, &paths->temporary);
    }
    if (status == NAULA_OK) {
        status = widen(path, &paths->target);
    }
    return status;
}

naula_status naula_cache_file_replace(const char *directory, const char *path, const uint8_t *bytes, size_t length) {
    replace_paths paths;
    naula_status status = widen_paths(directory, path, &paths);
    if (status != NAULA_OK) {
        release_paths(&paths);
        return status;
    }
    status = NAULA_REFUSED;
    if (make_directory(paths.directory) && make_directory(paths.identities)) {
        HANDLE file = CreateFileW(paths.temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            int written = write_all(file, bytes, length);
            CloseHandle(file);
            if (written && MoveFileExW(paths.temporary, paths.target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                status = NAULA_OK;
            } else {
                DeleteFileW(paths.temporary);
            }
        }
    }
    release_paths(&paths);
    return status;
}
