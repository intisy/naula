#define _POSIX_C_SOURCE 200809L
#ifdef __linux__
#define _GNU_SOURCE
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

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
#include "disk_posix_internal.h"
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

/** Flushes the folder holding `path`, cutting the writable `path` short while it opens it, so it allocates nothing. */
static void sync_folder_of(char *path) {
    char *slash = strrchr(path, '/');
    char kept = '\0';
    char *cut = NULL;
    if (slash != NULL) {
        cut = slash == path ? slash + 1 : slash;
        kept = *cut;
        *cut = '\0';
    }
    int descriptor = open(slash == NULL ? "." : path, O_RDONLY | O_DIRECTORY);
    if (cut != NULL) {
        *cut = kept;
    }
    if (descriptor >= 0) {
        fsync(descriptor);
        close(descriptor);
    }
}

naula_status disk_sync_parent(const char *path) {
    char *copy = copy_of(path, strlen(path));
    if (copy == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    sync_folder_of(copy);
    naula_memory_release(copy);
    return NAULA_OK;
}

naula_status disk_folder_make(const char *path, unsigned long *error) {
    *error = 0;
    if (mkdir(path, 0777) != 0) {
        return refused(error);
    }
    return disk_sync_parent(path);
}

/** 1 when the rename was made or refused, 0 when this system or file system cannot rename without replacing. */
static int rename_without_replacing(const char *from, const char *to, int *renamed) {
#if defined(__linux__) && defined(RENAME_NOREPLACE)
    if (renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE) == 0) {
        *renamed = 1;
        return 1;
    }
    *renamed = 0;
    return errno != EINVAL && errno != ENOSYS;
#else
    (void)from;
    (void)to;
    *renamed = 0;
    return 0;
#endif
}

/**
 * @implNote rename(2) replaces an empty folder at `to`, so where renameat2's RENAME_NOREPLACE is missing (outside
 * Linux, an old kernel, or a file system that answers EINVAL) the absence is checked first and the window accepted.
 */
naula_status disk_move(const char *from, const char *to, int *held, unsigned long *error) {
    struct stat found;
    *held = 0;
    *error = 0;
    int renamed = 0;
    if (rename_without_replacing(from, to, &renamed)) {
        return renamed ? disk_sync_parent(to) : refused(error);
    }
    if (lstat(to, &found) == 0) {
        *error = EEXIST;
        return NAULA_REFUSED;
    }
    if (rename(from, to) != 0) {
        return refused(error);
    }
    return disk_sync_parent(to);
}

#define OWNER_ALL (S_IRUSR | S_IWUSR | S_IXUSR)
#define OPEN_FOLDER_BUDGET 64

/** A folder without the owner's write and search rights keeps its entries, so it gets them before it is emptied. */
static naula_status let_owner_empty(int folder, unsigned long *error) {
    struct stat found;
    if (fstat(folder, &found) != 0) {
        return refused(error);
    }
    if ((found.st_mode & OWNER_ALL) == OWNER_ALL) {
        return NAULA_OK;
    }
    return fchmod(folder, (found.st_mode & 07777) | OWNER_ALL) == 0 ? NAULA_OK : refused(error);
}

/** Opens `name` in `folder` only when it is a real folder there, never through a link. */
static int open_folder_at(int folder, const char *name) {
    return openat(folder, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
}

/**
 * One folder being emptied: its name in the folder below it on the stack, and its identity, so that when its
 * descriptor was closed to stay within OPEN_FOLDER_BUDGET, the one reopened through `..` is proven the same folder.
 */
typedef struct removal_frame {
    int folder;
    dev_t device;
    ino_t inode;
    char *name;
} removal_frame;

typedef struct removal {
    int parent;
    removal_frame *frames;
    size_t depth;
    size_t capacity;
} removal;

/** Takes ownership of `folder` and `name`, closing and releasing both when it cannot push them. */
static naula_status push_frame(removal *removing, int folder, char *name, unsigned long *error) {
    struct stat found;
    naula_status status = fstat(folder, &found) == 0 ? let_owner_empty(folder, error) : refused(error);
    if (status == NAULA_OK && removing->depth == removing->capacity) {
        size_t grown = removing->capacity == 0 ? 64 : removing->capacity * 2;
        removal_frame *larger = naula_memory_reallocate(removing->frames, grown * sizeof *larger);
        status = larger == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
        if (larger != NULL) {
            removing->frames = larger;
            removing->capacity = grown;
        }
    }
    if (status != NAULA_OK) {
        close(folder);
        naula_memory_release(name);
        return status;
    }
    removal_frame *frame = &removing->frames[removing->depth++];
    frame->folder = folder;
    frame->device = found.st_dev;
    frame->inode = found.st_ino;
    frame->name = name;
    if (removing->depth > OPEN_FOLDER_BUDGET) {
        removal_frame *resting = &removing->frames[removing->depth - 1 - OPEN_FOLDER_BUDGET];
        close(resting->folder);
        resting->folder = -1;
    }
    return NAULA_OK;
}

/**
 * Reads `folder` from its start, removing every entry that is not a real folder, until it meets one to descend into,
 * handed back opened in `descend` with its name. `removed` says whether anything went, so a folder counts as empty
 * only once a whole reading found nothing.
 */
static naula_status take_listing(int folder, int *descend, char **name, int *removed, unsigned long *error) {
    *descend = -1;
    *removed = 0;
    int copy = dup(folder);
    DIR *listing = copy < 0 ? NULL : fdopendir(copy);
    if (listing == NULL) {
        naula_status status = refused(error);
        if (copy >= 0) {
            close(copy);
        }
        return status;
    }
    rewinddir(listing);
    naula_status status = NAULA_OK;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(listing);
        if (entry == NULL) {
            status = errno != 0 ? refused(error) : NAULA_OK;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        *removed = 1;
        struct stat found;
        if (fstatat(folder, entry->d_name, &found, AT_SYMLINK_NOFOLLOW) != 0) {
            status = errno == ENOENT ? NAULA_OK : refused(error);
        } else if (!S_ISDIR(found.st_mode)) {
            status = unlinkat(folder, entry->d_name, 0) == 0 || errno == ENOENT ? NAULA_OK : refused(error);
        } else {
            *name = copy_of(entry->d_name, strlen(entry->d_name));
            *descend = *name == NULL ? -1 : open_folder_at(folder, entry->d_name);
            status = *name == NULL ? NAULA_OUT_OF_MEMORY : (*descend < 0 ? refused(error) : NAULA_OK);
            if (status != NAULA_OK) {
                naula_memory_release(*name);
                *name = NULL;
            }
            break;
        }
        if (status != NAULA_OK) {
            break;
        }
    }
    closedir(listing);
    return status;
}

/** The descriptor of the folder below the top of the stack, reopened through `..` when it was closed. */
static naula_status parent_of_top(removal *removing, int *parent, unsigned long *error) {
    if (removing->depth == 1) {
        *parent = removing->parent;
        return NAULA_OK;
    }
    removal_frame *below = &removing->frames[removing->depth - 2];
    if (below->folder < 0) {
        int reopened = openat(removing->frames[removing->depth - 1].folder, "..", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        struct stat found;
        if (reopened < 0 || fstat(reopened, &found) != 0) {
            naula_status status = refused(error);
            if (reopened >= 0) {
                close(reopened);
            }
            return status;
        }
        if (found.st_dev != below->device || found.st_ino != below->inode) {
            close(reopened);
            *error = ESTALE;
            return NAULA_REFUSED;
        }
        below->folder = reopened;
    }
    *parent = below->folder;
    return NAULA_OK;
}

static naula_status remove_top_folder(removal *removing, unsigned long *error) {
    int parent = -1;
    naula_status status = parent_of_top(removing, &parent, error);
    removal_frame *top = &removing->frames[removing->depth - 1];
    close(top->folder);
    top->folder = -1;
    if (status == NAULA_OK && unlinkat(parent, top->name, AT_REMOVEDIR) != 0) {
        status = refused(error);
    }
    naula_memory_release(top->name);
    removing->depth--;
    return status;
}

/** Empties and removes the folder `name` in `parent`, depth first, on its own stack rather than the call stack. */
static naula_status remove_tree_at(int parent, const char *name, unsigned long *error) {
    removal removing;
    memset(&removing, 0, sizeof removing);
    removing.parent = parent;
    char *own_name = copy_of(name, strlen(name));
    int clone = own_name == NULL ? -1 : open_folder_at(parent, name);
    naula_status status = own_name == NULL ? NAULA_OUT_OF_MEMORY : (clone < 0 ? refused(error) : NAULA_OK);
    if (status == NAULA_OK) {
        status = push_frame(&removing, clone, own_name, error);
    } else {
        naula_memory_release(own_name);
    }
    while (status == NAULA_OK && removing.depth > 0) {
        int descend = -1;
        char *child = NULL;
        int removed = 0;
        status = take_listing(removing.frames[removing.depth - 1].folder, &descend, &child, &removed, error);
        if (status == NAULA_OK && descend >= 0) {
            status = push_frame(&removing, descend, child, error);
        } else if (status == NAULA_OK && !removed) {
            status = remove_top_folder(&removing, error);
        }
    }
    while (removing.depth > 0) {
        removal_frame *frame = &removing.frames[--removing.depth];
        if (frame->folder >= 0) {
            close(frame->folder);
        }
        naula_memory_release(frame->name);
    }
    naula_memory_release(removing.frames);
    return status;
}

/**
 * Opens every component of `below` but the last relative to the folder before it, from `root` down, never through a
 * link, and hands back the last folder and where the last component starts. Cuts `below` while it works.
 */
static naula_status walk_to_parent(const char *root, char *below, int *parent, const char **name,
                                   unsigned long *error) {
    int folder = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (folder < 0) {
        return refused(error);
    }
    char *component = below;
    for (char *slash = strchr(component, '/'); slash != NULL; slash = strchr(component, '/')) {
        *slash = '\0';
        int next = open_folder_at(folder, component);
        *slash = '/';
        close(folder);
        if (next < 0) {
            return refused(error);
        }
        folder = next;
        component = slash + 1;
    }
    *parent = folder;
    *name = component;
    return NAULA_OK;
}

naula_status disk_tree_remove(const char *root, const char *below, int *held, unsigned long *error) {
    *held = 0;
    *error = 0;
    char *copy = copy_of(below, strlen(below));
    if (copy == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    int parent = -1;
    const char *name = NULL;
    naula_status status = walk_to_parent(root, copy, &parent, &name, error);
    if (status == NAULA_OK) {
        status = remove_tree_at(parent, name, error);
    }
    if (status == NAULA_OK) {
        fsync(parent);
    }
    if (parent >= 0) {
        close(parent);
    }
    naula_memory_release(copy);
    *held = status == NAULA_REFUSED && *error == EBUSY;
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
        sync_folder_of(temporary);
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
    disk_appender *made = naula_memory_allocate(sizeof *made);
    if (made == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    int flags = O_WRONLY | O_APPEND | O_NOFOLLOW | (create ? O_CREAT | O_EXCL : 0);
    int descriptor = open(path, flags, 0666);
    naula_status status = descriptor < 0 ? refused(error) : NAULA_OK;
    if (status == NAULA_OK && create) {
        status = disk_sync_parent(path);
    }
    if (status != NAULA_OK) {
        if (descriptor >= 0) {
            close(descriptor);
        }
        naula_memory_release(made);
        return status;
    }
    made->descriptor = descriptor;
    *appender = made;
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
            sync_folder_of(copy);
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
