#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "cli.h"
#include "disk.h"
#include "naula/memory.h"

#if defined(_WIN32)
static char *narrow(const wchar_t *wide) {
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *text = needed <= 0 ? NULL : naula_memory_allocate((size_t)needed);
    if (text != NULL && WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, needed, NULL, NULL) != needed) {
        naula_memory_release(text);
        return NULL;
    }
    return text;
}

static wchar_t *widen(const char *text) {
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t *wide = needed <= 0 ? NULL : naula_memory_allocate((size_t)needed * sizeof *wide);
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, needed) != needed) {
        naula_memory_release(wide);
        return NULL;
    }
    return wide;
}

char *cli_environment(const char *name) {
    wchar_t *wide_name = widen(name);
    DWORD needed = wide_name == NULL ? 0 : GetEnvironmentVariableW(wide_name, NULL, 0);
    wchar_t *value = needed == 0 ? NULL : naula_memory_allocate(needed * sizeof *value);
    char *text = NULL;
    if (value != NULL && GetEnvironmentVariableW(wide_name, value, needed) == needed - 1) {
        text = narrow(value);
    }
    naula_memory_release(value);
    naula_memory_release(wide_name);
    if (text != NULL && text[0] == '\0') {
        naula_memory_release(text);
        return NULL;
    }
    return text;
}

static char *working_folder(void) {
    DWORD needed = GetCurrentDirectoryW(0, NULL);
    wchar_t *wide = needed == 0 ? NULL : naula_memory_allocate(needed * sizeof *wide);
    char *text = wide != NULL && GetCurrentDirectoryW(needed, wide) == needed - 1 ? narrow(wide) : NULL;
    naula_memory_release(wide);
    return text;
}

static int is_absolute(const char *path) {
    return ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':' &&
           (path[2] == '/' || path[2] == '\\');
}

naula_status cli_folder_bytes(const char *folder, uint64_t *bytes) {
    *bytes = 0;
    char *pattern = cli_join(folder, "*");
    wchar_t *wide = pattern == NULL ? NULL : widen(pattern);
    naula_memory_release(pattern);
    if (wide == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(wide, &found);
    naula_memory_release(wide);
    if (search == INVALID_HANDLE_VALUE) {
        return NAULA_OK;
    }
    do {
        if (!(found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            *bytes += ((uint64_t)found.nFileSizeHigh << 32) | found.nFileSizeLow;
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return NAULA_OK;
}
#else
naula_status cli_folder_bytes(const char *folder, uint64_t *bytes) {
    *bytes = 0;
    DIR *listing = opendir(folder);
    if (listing == NULL) {
        return NAULA_OK;
    }
    int descriptor = dirfd(listing);
    for (struct dirent *entry = readdir(listing); entry != NULL; entry = readdir(listing)) {
        struct stat facts;
        if (fstatat(descriptor, entry->d_name, &facts, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(facts.st_mode)) {
            *bytes += (uint64_t)facts.st_size;
        }
    }
    closedir(listing);
    return NAULA_OK;
}

char *cli_environment(const char *name) {
    const char *value = getenv(name);
    return value == NULL || value[0] == '\0' ? NULL : naula_memory_duplicate_text(value);
}

static char *working_folder(void) {
    size_t capacity = 4096;
    char *text = naula_memory_allocate(capacity);
    if (text != NULL && getcwd(text, capacity) == NULL) {
        naula_memory_release(text);
        return NULL;
    }
    return text;
}

static int is_absolute(const char *path) {
    return path[0] == '/';
}
#endif

/** Drops empty and `.` segments and resolves `..` against the segments before it, never above the first. */
static void normalise(char *path, size_t keep) {
    for (char *cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '\\') {
            *cursor = '/';
        }
    }
    char *write = path + keep;
    const char *read = path + keep;
    while (*read != '\0') {
        while (*read == '/') {
            read++;
        }
        const char *end = strchr(read, '/');
        size_t length = end == NULL ? strlen(read) : (size_t)(end - read);
        if (length == 0 || (length == 1 && read[0] == '.')) {
            read += length;
            continue;
        }
        if (length == 2 && read[0] == '.' && read[1] == '.') {
            while (write > path + keep && *(write - 1) != '/') {
                write--;
            }
            if (write > path + keep) {
                write--;
            }
            read += length;
            continue;
        }
        if (write > path + keep) {
            *write++ = '/';
        }
        memmove(write, read, length);
        write += length;
        read += length;
    }
    *write = '\0';
}

char *cli_absolute_path(const char *path) {
    char *joined = NULL;
    if (is_absolute(path)) {
        joined = naula_memory_duplicate_text(path);
    } else {
        char *folder = working_folder();
        joined = folder == NULL ? NULL : cli_join(folder, path);
        naula_memory_release(folder);
    }
    if (joined == NULL) {
        return NULL;
    }
#if defined(_WIN32)
    joined[0] = (char)(joined[0] >= 'a' && joined[0] <= 'z' ? joined[0] - 'a' + 'A' : joined[0]);
    normalise(joined, 3);
    if (joined[3] == '\0') {
        joined[2] = '/';
    }
#else
    normalise(joined, 1);
#endif
    return joined;
}

int cli_is_folder(const char *path) {
    disk_kind kind = DISK_MISSING;
    unsigned long error = 0;
    return disk_kind_of(path, &kind, &error) == NAULA_OK && kind == DISK_FOLDER;
}

int cli_exists(const char *path) {
    disk_kind kind = DISK_MISSING;
    unsigned long error = 0;
    return disk_kind_of(path, &kind, &error) == NAULA_OK && kind != DISK_MISSING;
}

naula_status cli_make_folders(const char *path, naula_refusal *refusal) {
    char *copy = naula_memory_duplicate_text(path);
    if (copy == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    naula_status status = NAULA_OK;
    for (char *cursor = copy + 1; status == NAULA_OK; cursor++) {
        int at_end = *cursor == '\0';
        if (!at_end && *cursor != '/' && *cursor != '\\') {
            continue;
        }
        char saved = *cursor;
        *cursor = '\0';
        unsigned long error = 0;
        disk_kind kind = DISK_MISSING;
        int is_drive = strlen(copy) == 2 && copy[1] == ':';
        if (!is_drive && disk_kind_of(copy, &kind, &error) == NAULA_OK && kind == DISK_MISSING &&
            disk_folder_make(copy, &error) != NAULA_OK) {
            char text[256];
            disk_error_text(error, text, sizeof text);
            naula_refuse(refusal, "folder-not-made", "check the folder can be written", "cannot make %s: %s", copy,
                         text);
            status = NAULA_REFUSED;
        }
        *cursor = saved;
        if (at_end) {
            break;
        }
    }
    naula_memory_release(copy);
    return status;
}

naula_status cli_read_file(const char *path, uint8_t **bytes, size_t *length, naula_refusal *refusal) {
    uint8_t *read = NULL;
    size_t read_length = 0;
    unsigned long error = 0;
    *bytes = NULL;
    *length = 0;
    naula_status status = disk_file_read(path, &read, &read_length, &error);
    if (status == NAULA_NOT_FOUND || status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    if (status != NAULA_OK) {
        char text[256];
        disk_error_text(error, text, sizeof text);
        naula_refuse(refusal, "file-unreadable", "check the file can be read", "cannot read %s: %s", path, text);
        return status;
    }
    uint8_t *terminated = naula_memory_reallocate(read, read_length + 1);
    if (terminated == NULL) {
        naula_memory_release(read);
        return NAULA_OUT_OF_MEMORY;
    }
    terminated[read_length] = '\0';
    *bytes = terminated;
    *length = read_length;
    return NAULA_OK;
}
