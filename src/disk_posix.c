#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "disk.h"
#include "naula/memory.h"

#define FILE_LIMIT (16 * 1024 * 1024)

struct disk_appender {
    int descriptor;
};

static naula_status refused(unsigned long *error) {
    *error = (unsigned long)errno;
    return NAULA_REFUSED;
}

static char *copy_of(const char *text, size_t length) {
    char *copy = naula_memory_allocate(length + 1);
    if (copy != NULL) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

/** Flushes the folder holding `path`, so a name made or renamed there survives a power cut. */
static void sync_parent(const char *path) {
    const char *slash = strrchr(path, '/');
    char *parent = slash == NULL ? copy_of(".", 1) : copy_of(path, slash == path ? 1 : (size_t)(slash - path));
    if (parent == NULL) {
        return;
    }
    int descriptor = open(parent, O_RDONLY | O_DIRECTORY);
    naula_memory_release(parent);
    if (descriptor >= 0) {
        fsync(descriptor);
        close(descriptor);
    }
}

naula_status disk_folder_make(const char *path, unsigned long *error) {
    *error = 0;
    if (mkdir(path, 0777) != 0) {
        return refused(error);
    }
    sync_parent(path);
    return NAULA_OK;
}

/** @implNote rename(2) replaces an empty folder at `to`, so the absence is checked first; the window is accepted. */
naula_status disk_move(const char *from, const char *to, int *held, unsigned long *error) {
    struct stat found;
    *held = 0;
    *error = 0;
    if (lstat(to, &found) == 0) {
        *error = EEXIST;
        return NAULA_REFUSED;
    }
    if (rename(from, to) != 0) {
        return refused(error);
    }
    sync_parent(to);
    return NAULA_OK;
}

static naula_status remove_children(int folder, unsigned long *error);

static naula_status remove_child(int folder, const char *name, unsigned long *error) {
    struct stat found;
    if (fstatat(folder, name, &found, AT_SYMLINK_NOFOLLOW) != 0) {
        return refused(error);
    }
    if (!S_ISDIR(found.st_mode)) {
        return unlinkat(folder, name, 0) == 0 ? NAULA_OK : refused(error);
    }
    int child = openat(folder, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (child < 0) {
        return refused(error);
    }
    naula_status status = remove_children(child, error);
    if (status == NAULA_OK && unlinkat(folder, name, AT_REMOVEDIR) != 0) {
        status = refused(error);
    }
    return status;
}

/** Takes ownership of `folder` and closes it. */
static naula_status remove_children(int folder, unsigned long *error) {
    DIR *listing = fdopendir(folder);
    if (listing == NULL) {
        naula_status status = refused(error);
        close(folder);
        return status;
    }
    naula_status status = NAULA_OK;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(listing);
        if (entry == NULL) {
            if (errno != 0) {
                status = refused(error);
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        status = remove_child(dirfd(listing), entry->d_name, error);
        if (status != NAULA_OK) {
            break;
        }
        rewinddir(listing);
    }
    closedir(listing);
    return status;
}

naula_status disk_tree_remove(const char *path, unsigned long *error) {
    *error = 0;
    int folder = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (folder < 0) {
        return refused(error);
    }
    naula_status status = remove_children(folder, error);
    if (status == NAULA_OK && rmdir(path) != 0) {
        status = refused(error);
    }
    if (status == NAULA_OK) {
        sync_parent(path);
    }
    return status;
}

naula_status disk_file_read(const char *path, uint8_t **bytes, size_t *length, unsigned long *error) {
    *bytes = NULL;
    *length = 0;
    *error = 0;
    int descriptor = open(path, O_RDONLY | O_NOFOLLOW);
    if (descriptor < 0) {
        *error = (unsigned long)errno;
        return errno == ENOENT ? NAULA_NOT_FOUND : NAULA_REFUSED;
    }
    struct stat found;
    naula_status status = NAULA_OK;
    if (fstat(descriptor, &found) != 0 || !S_ISREG(found.st_mode) || found.st_size < 0 ||
        found.st_size > FILE_LIMIT) {
        *error = errno != 0 ? (unsigned long)errno : EINVAL;
        status = NAULA_REFUSED;
    }
    size_t expected = status == NAULA_OK ? (size_t)found.st_size : 0;
    *bytes = status == NAULA_OK ? naula_memory_allocate(expected + 1) : NULL;
    if (status == NAULA_OK && *bytes == NULL) {
        status = NAULA_OUT_OF_MEMORY;
    }
    size_t total = 0;
    while (status == NAULA_OK && total < expected) {
        ssize_t got = read(descriptor, *bytes + total, expected - total);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            *error = got < 0 ? (unsigned long)errno : EIO;
            status = NAULA_REFUSED;
            break;
        }
        total += (size_t)got;
    }
    close(descriptor);
    if (status != NAULA_OK) {
        naula_memory_release(*bytes);
        *bytes = NULL;
        return status;
    }
    (*bytes)[total] = 0;
    *length = total;
    return NAULA_OK;
}

static int write_all(int descriptor, const uint8_t *bytes, size_t length) {
    size_t total = 0;
    while (total < length) {
        ssize_t written = write(descriptor, bytes + total, length - total);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return 0;
        }
        total += (size_t)written;
    }
    return 1;
}

static naula_status clear_stale_temporary(const char *temporary, unsigned long *error) {
    struct stat found;
    if (lstat(temporary, &found) != 0) {
        return errno == ENOENT ? NAULA_OK : refused(error);
    }
    if (!S_ISREG(found.st_mode)) {
        *error = EEXIST;
        return NAULA_REFUSED;
    }
    return unlink(temporary) == 0 ? NAULA_OK : refused(error);
}

static naula_status write_temporary(const char *temporary, const uint8_t *bytes, size_t length,
                                    unsigned long *error) {
    int descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
    if (descriptor < 0) {
        return refused(error);
    }
    naula_status status = write_all(descriptor, bytes, length) && fsync(descriptor) == 0 ? NAULA_OK : refused(error);
    close(descriptor);
    if (status != NAULA_OK) {
        unlink(temporary);
    }
    return status;
}

naula_status disk_file_replace(const char *path, const uint8_t *bytes, size_t length, unsigned long *error) {
    *error = 0;
    size_t path_length = strlen(path);
    char *temporary = naula_memory_allocate(path_length + 11);
    if (temporary == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    memcpy(temporary, path, path_length);
    memcpy(temporary + path_length, ".naula-tmp", 11);
    naula_status status = clear_stale_temporary(temporary, error);
    if (status == NAULA_OK) {
        status = write_temporary(temporary, bytes, length, error);
    }
    if (status == NAULA_OK && rename(temporary, path) != 0) {
        status = refused(error);
        unlink(temporary);
    }
    if (status == NAULA_OK) {
        sync_parent(path);
    }
    naula_memory_release(temporary);
    return status;
}

void disk_names_release(char **names, size_t count) {
    for (size_t index = 0; index < count; index++) {
        naula_memory_release(names[index]);
    }
    naula_memory_release(names);
}

static naula_status append_name(char ***names, size_t *count, size_t *capacity, const char *name) {
    if (*count == *capacity) {
        size_t grown = *capacity == 0 ? 16 : *capacity * 2;
        char **larger = naula_memory_reallocate(*names, grown * sizeof *larger);
        if (larger == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        *names = larger;
        *capacity = grown;
    }
    char *copy = copy_of(name, strlen(name));
    if (copy == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    (*names)[(*count)++] = copy;
    return NAULA_OK;
}

naula_status disk_names(const char *path, char ***names, size_t *count, unsigned long *error) {
    *names = NULL;
    *count = 0;
    *error = 0;
    int folder = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    DIR *listing = folder < 0 ? NULL : fdopendir(folder);
    if (listing == NULL) {
        naula_status status = refused(error);
        if (folder >= 0) {
            close(folder);
        }
        return status;
    }
    size_t capacity = 0;
    naula_status status = NAULA_OK;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(listing);
        if (entry == NULL) {
            status = errno != 0 ? refused(error) : NAULA_OK;
            break;
        }
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            status = append_name(names, count, &capacity, entry->d_name);
            if (status != NAULA_OK) {
                break;
            }
        }
    }
    closedir(listing);
    if (status != NAULA_OK) {
        disk_names_release(*names, *count);
        *names = NULL;
        *count = 0;
    }
    return status;
}

naula_status disk_appender_open(const char *path, int create, disk_appender **appender, unsigned long *error) {
    *appender = NULL;
    *error = 0;
    int flags = O_WRONLY | O_APPEND | O_NOFOLLOW | (create ? O_CREAT | O_EXCL : 0);
    int descriptor = open(path, flags, 0666);
    if (descriptor < 0) {
        return refused(error);
    }
    if (create) {
        sync_parent(path);
    }
    *appender = naula_memory_allocate(sizeof **appender);
    if (*appender == NULL) {
        close(descriptor);
        return NAULA_OUT_OF_MEMORY;
    }
    (*appender)->descriptor = descriptor;
    return NAULA_OK;
}

naula_status disk_append(disk_appender *appender, const char *bytes, size_t length, unsigned long *error) {
    *error = 0;
    if (!write_all(appender->descriptor, (const uint8_t *)bytes, length) || fsync(appender->descriptor) != 0) {
        return refused(error);
    }
    return NAULA_OK;
}

void disk_appender_close(disk_appender *appender) {
    if (appender != NULL) {
        close(appender->descriptor);
        naula_memory_release(appender);
    }
}

void disk_utc_stamp(char stamp[17]) {
    time_t now = time(NULL);
    struct tm parts;
    memset(&parts, 0, sizeof parts);
    gmtime_r(&now, &parts);
    snprintf(stamp, 17, "%04u%02u%02uT%02u%02u%02uZ", (unsigned)(parts.tm_year + 1900) % 10000u,
             (unsigned)(parts.tm_mon + 1) % 100u, (unsigned)parts.tm_mday % 100u, (unsigned)parts.tm_hour % 100u,
             (unsigned)parts.tm_min % 100u, (unsigned)parts.tm_sec % 100u);
}

void disk_error_text(unsigned long error, char *text, size_t capacity) {
    if (capacity > 0) {
        snprintf(text, capacity, "%s (errno %lu)", strerror((int)error), error);
    }
}

struct disk_owned_file {
    char *path;
};

naula_status disk_owned_file_create(const char *path, const uint8_t *bytes, size_t length, int *exists,
                                    disk_owned_file **owned, unsigned long *error) {
    *owned = NULL;
    *exists = 0;
    *error = 0;
    disk_owned_file *made = naula_memory_allocate(sizeof *made);
    char *copy = made == NULL ? NULL : copy_of(path, strlen(path));
    if (copy == NULL) {
        naula_memory_release(made);
        return NAULA_OUT_OF_MEMORY;
    }
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
    naula_status status = NAULA_OK;
    if (descriptor < 0) {
        *exists = errno == EEXIST;
        status = refused(error);
    } else {
        status = write_all(descriptor, bytes, length) && fsync(descriptor) == 0 ? NAULA_OK : refused(error);
        close(descriptor);
        if (status != NAULA_OK) {
            unlink(path);
        } else {
            sync_parent(path);
        }
    }
    if (status != NAULA_OK) {
        naula_memory_release(copy);
        naula_memory_release(made);
        return status;
    }
    made->path = copy;
    *owned = made;
    return NAULA_OK;
}

void disk_owned_file_remove(disk_owned_file *owned) {
    if (owned != NULL) {
        unlink(owned->path);
    }
    disk_owned_file_abandon(owned);
}

void disk_owned_file_abandon(disk_owned_file *owned) {
    if (owned != NULL) {
        naula_memory_release(owned->path);
        naula_memory_release(owned);
    }
}

naula_status disk_file_remove(const char *path, unsigned long *error) {
    struct stat found;
    *error = 0;
    if (lstat(path, &found) != 0) {
        return refused(error);
    }
    if (!S_ISREG(found.st_mode)) {
        *error = EINVAL;
        return NAULA_REFUSED;
    }
    return unlink(path) == 0 ? NAULA_OK : refused(error);
}

unsigned long disk_process_id(void) {
    return (unsigned long)getpid();
}

int disk_process_is_alive(unsigned long process_id) {
    if (process_id == 0 || process_id > (unsigned long)INT_MAX) {
        return 0;
    }
    return kill((pid_t)process_id, 0) == 0 || errno == EPERM;
}
