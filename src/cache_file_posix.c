#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "cache_internal.h"
#include "naula/memory.h"

#define CACHE_FILE_LIMIT (1024 * 1024)

static naula_status read_descriptor(int descriptor, uint8_t **bytes, size_t *length) {
    struct stat status_of_file;
    if (fstat(descriptor, &status_of_file) != 0 || !S_ISREG(status_of_file.st_mode) || status_of_file.st_size < 0 ||
        status_of_file.st_size > CACHE_FILE_LIMIT) {
        return NAULA_REFUSED;
    }
    size_t expected = (size_t)status_of_file.st_size;
    *bytes = naula_memory_allocate(expected + 1);
    if (*bytes == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    size_t total = 0;
    while (total < expected) {
        ssize_t got = read(descriptor, *bytes + total, expected - total);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            break;
        }
        total += (size_t)got;
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
    *bytes = NULL;
    *length = 0;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return errno == ENOENT || errno == ENOTDIR ? NAULA_NOT_FOUND : NAULA_REFUSED;
    }
    naula_status status = read_descriptor(descriptor, bytes, length);
    close(descriptor);
    return status;
}

static int make_directory(const char *path) {
    return mkdir(path, 0700) == 0 || errno == EEXIST;
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
    return fsync(descriptor) == 0;
}

static char *joined(const char *left, const char *right) {
    size_t length = strlen(left) + strlen(right) + 1;
    char *text = naula_memory_allocate(length);
    if (text != NULL) {
        snprintf(text, length, "%s%s", left, right);
    }
    return text;
}

naula_status naula_cache_file_replace(const char *directory, const char *path, const uint8_t *bytes, size_t length) {
    static unsigned long counter;
    char suffix[64];
    snprintf(suffix, sizeof suffix, ".%ld.%lu.tmp", (long)getpid(), ++counter);
    char *identities = joined(directory, "/identities");
    char *temporary = joined(path, suffix);
    if (identities == NULL || temporary == NULL) {
        naula_memory_release(identities);
        naula_memory_release(temporary);
        return NAULA_OUT_OF_MEMORY;
    }
    naula_status status = NAULA_REFUSED;
    if (make_directory(directory) && make_directory(identities)) {
        int descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (descriptor >= 0) {
            int written = write_all(descriptor, bytes, length);
            int closed = close(descriptor) == 0;
            if (written && closed && rename(temporary, path) == 0) {
                status = NAULA_OK;
            } else {
                unlink(temporary);
            }
        }
    }
    naula_memory_release(identities);
    naula_memory_release(temporary);
    return status;
}
