#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "disk.h"
#include "disk_posix_internal.h"
#include "naula/memory.h"

#define LINK_TARGET_LIMIT 65536

naula_status disk_kind_of(const char *path, disk_kind *kind, unsigned long *error) {
    struct stat found;
    *kind = DISK_MISSING;
    *error = 0;
    if (lstat(path, &found) != 0) {
        *error = (unsigned long)errno;
        return errno == ENOENT ? NAULA_OK : NAULA_REFUSED;
    }
    if (S_ISLNK(found.st_mode)) {
        *kind = DISK_LINK;
    } else if (S_ISDIR(found.st_mode)) {
        *kind = DISK_FOLDER;
    } else if (S_ISREG(found.st_mode)) {
        *kind = DISK_FILE;
    } else {
        *kind = DISK_OTHER;
    }
    return NAULA_OK;
}

naula_status disk_link_target(const char *path, char **target, unsigned long *error) {
    *target = NULL;
    *error = 0;
    for (size_t capacity = 256; capacity <= LINK_TARGET_LIMIT; capacity *= 2) {
        char *buffer = naula_memory_allocate(capacity);
        if (buffer == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        ssize_t length = readlink(path, buffer, capacity);
        if (length < 0) {
            *error = (unsigned long)errno;
            naula_memory_release(buffer);
            return NAULA_REFUSED;
        }
        if ((size_t)length < capacity) {
            buffer[length] = '\0';
            *target = buffer;
            return NAULA_OK;
        }
        naula_memory_release(buffer);
    }
    *error = ENAMETOOLONG;
    return NAULA_REFUSED;
}

naula_status disk_link_make(const char *path, const char *target, unsigned long *error) {
    *error = 0;
    if (symlink(target, path) != 0) {
        *error = (unsigned long)errno;
        return NAULA_REFUSED;
    }
    return disk_sync_parent(path);
}

naula_status disk_link_remove(const char *path, unsigned long *error) {
    struct stat found;
    *error = 0;
    if (lstat(path, &found) != 0) {
        *error = (unsigned long)errno;
        return NAULA_REFUSED;
    }
    if (!S_ISLNK(found.st_mode)) {
        *error = EINVAL;
        return NAULA_REFUSED;
    }
    if (unlink(path) != 0) {
        *error = (unsigned long)errno;
        return NAULA_REFUSED;
    }
    return disk_sync_parent(path);
}
