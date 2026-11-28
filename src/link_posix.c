#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
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

naula_status disk_link_make(const char *root, const char *below, const char *target, disk_walk_purpose purpose,
                            unsigned long *error) {
    *error = 0;
    disk_walk walk;
    naula_status status = disk_walk_to_parent(root, below, purpose, &walk, error);
    if (status == NAULA_OK && symlinkat(target, walk.parent, walk.name) != 0) {
        *error = (unsigned long)errno;
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK) {
        fsync(walk.parent);
    }
    disk_walk_release(&walk);
    return status;
}

naula_status disk_file_remove(const char *root, const char *below, disk_walk_purpose purpose, unsigned long *error) {
    *error = 0;
    disk_walk walk;
    naula_status status = disk_walk_to_parent(root, below, purpose, &walk, error);
    struct stat found;
    if (status == NAULA_OK && fstatat(walk.parent, walk.name, &found, AT_SYMLINK_NOFOLLOW) != 0) {
        *error = (unsigned long)errno;
        status = errno == ENOENT ? NAULA_NOT_FOUND : NAULA_REFUSED;
    } else if (status == NAULA_OK && !S_ISREG(found.st_mode)) {
        *error = EINVAL;
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK && unlinkat(walk.parent, walk.name, 0) != 0) {
        *error = (unsigned long)errno;
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK) {
        fsync(walk.parent);
    }
    disk_walk_release(&walk);
    return status;
}

static naula_status holds_exactly(int file, const uint8_t *bytes, size_t length, int *holding, unsigned long *error) {
    uint8_t buffer[4096];
    size_t seen = 0;
    for (;;) {
        ssize_t read_now = read(file, buffer, sizeof buffer);
        if (read_now < 0 && errno == EINTR) {
            continue;
        }
        if (read_now < 0) {
            *error = (unsigned long)errno;
            return NAULA_REFUSED;
        }
        if (read_now == 0) {
            break;
        }
        if ((size_t)read_now > length - seen || memcmp(buffer, bytes + seen, (size_t)read_now) != 0) {
            *holding = 0;
            return NAULA_OK;
        }
        seen += (size_t)read_now;
    }
    *holding = seen == length;
    return NAULA_OK;
}

static int same_file(const struct stat *left, const struct stat *right) {
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino;
}

naula_status disk_file_remove_if_holding(const char *root, const char *below, const uint8_t *bytes, size_t length,
                                         disk_walk_purpose purpose, int *holding, unsigned long *error) {
    *error = 0;
    *holding = 0;
    disk_walk walk;
    naula_status status = disk_walk_to_parent(root, below, purpose, &walk, error);
    int file = -1;
    struct stat opened;
    if (status == NAULA_OK) {
        file = openat(walk.parent, walk.name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
        if (file < 0) {
            *error = (unsigned long)errno;
            status = errno == ENOENT ? NAULA_NOT_FOUND : NAULA_REFUSED;
        }
    }
    if (status == NAULA_OK && fstat(file, &opened) != 0) {
        *error = (unsigned long)errno;
        status = NAULA_REFUSED;
    } else if (status == NAULA_OK && !S_ISREG(opened.st_mode)) {
        *error = EINVAL;
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK) {
        status = holds_exactly(file, bytes, length, holding, error);
    }
    struct stat named;
    if (status == NAULA_OK && *holding && fstatat(walk.parent, walk.name, &named, AT_SYMLINK_NOFOLLOW) != 0) {
        *error = (unsigned long)errno;
        status = NAULA_REFUSED;
    } else if (status == NAULA_OK && *holding && !same_file(&named, &opened)) {
        *holding = 0;
    }
    if (status == NAULA_OK && *holding && unlinkat(walk.parent, walk.name, 0) != 0) {
        *error = (unsigned long)errno;
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK && *holding) {
        fsync(walk.parent);
    }
    if (file >= 0) {
        close(file);
    }
    disk_walk_release(&walk);
    return status;
}

naula_status disk_link_remove(const char *root, const char *below, disk_walk_purpose purpose, unsigned long *error) {
    *error = 0;
    disk_walk walk;
    naula_status status = disk_walk_to_parent(root, below, purpose, &walk, error);
    struct stat found;
    if (status == NAULA_OK && fstatat(walk.parent, walk.name, &found, AT_SYMLINK_NOFOLLOW) != 0) {
        *error = (unsigned long)errno;
        status = NAULA_REFUSED;
    } else if (status == NAULA_OK && !S_ISLNK(found.st_mode)) {
        *error = EINVAL;
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK && unlinkat(walk.parent, walk.name, 0) != 0) {
        *error = (unsigned long)errno;
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK) {
        fsync(walk.parent);
    }
    disk_walk_release(&walk);
    return status;
}
