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

/** The names of one listing, each ended by a NUL. */
typedef struct listing_snapshot {
    char *names;
    size_t length;
    size_t capacity;
} listing_snapshot;

/**
 * One folder being emptied: its name in the folder below it on the stack, and its identity, so that when its
 * descriptor was closed to stay within OPEN_FOLDER_BUDGET, the one reopened through `..` is proven the same folder.
 * The snapshot of its names is worked through from `next`; `listed` says one was ever taken, and `refills` counts
 * those found holding names after the first was worked through.
 */
typedef struct removal_frame {
    int folder;
    dev_t device;
    ino_t inode;
    char *name;
    listing_snapshot listing;
    size_t next;
    int listed;
    unsigned refills;
} removal_frame;

/**
 * The file system and the mount an entry is on. A bind mount shares its file system's device, so on Linux only the
 * mount id tells it apart; elsewhere the device alone does.
 */
typedef struct mount_identity {
    dev_t device;
    uint64_t mount;
    int has_mount;
} mount_identity;

#ifdef __linux__
#ifdef NAULA_TEST_HOOKS
static int test_without_statx;
static int test_without_file_handle;

void naula_disk_test_mount_ids(int by_statx, int by_file_handle) {
    test_without_statx = !by_statx;
    test_without_file_handle = !by_file_handle;
}
#else
#define test_without_statx 0
#define test_without_file_handle 0
#endif

/** The mount id statx gives since Linux 5.8; 0 with `errno` set where it gives none. */
static int mount_by_statx(int folder, const char *name, uint64_t *mount) {
    if (test_without_statx) {
        errno = ENOSYS;
        return 0;
    }
#ifdef STATX_MNT_ID
    struct statx extended;
    int flags = AT_SYMLINK_NOFOLLOW | (name[0] == '\0' ? AT_EMPTY_PATH : 0);
    if (statx(folder, name, flags, STATX_MNT_ID, &extended) != 0) {
        return 0;
    }
    if (!(extended.stx_mask & STATX_MNT_ID)) {
        errno = ENOTSUP;
        return 0;
    }
    *mount = extended.stx_mnt_id;
    return 1;
#else
    (void)folder;
    (void)name;
    (void)mount;
    errno = ENOSYS;
    return 0;
#endif
}

/**
 * The same mount id, from a file handle, which Linux gives since 2.6.39 on every file system that can name its files
 * by handle; 0 with `errno` set where it gives none.
 */
static int mount_by_file_handle(int folder, const char *name, uint64_t *mount) {
    _Alignas(struct file_handle) unsigned char room[sizeof(struct file_handle) + MAX_HANDLE_SZ];
    struct file_handle *handle = (struct file_handle *)room;
    handle->handle_bytes = MAX_HANDLE_SZ;
    int mount_id = 0;
    if (test_without_file_handle) {
        errno = ENOSYS;
        return 0;
    }
    if (name_to_handle_at(folder, name, handle, &mount_id, name[0] == '\0' ? AT_EMPTY_PATH : 0) != 0) {
        return 0;
    }
    *mount = (uint64_t)(unsigned)mount_id;
    return 1;
}
#endif

/**
 * The mount of the entry `name` in `folder`, never through a link, or of `folder` itself when `name` is empty. Answers
 * 0, or the error of the last way tried when none gave a mount id, with `has_mount` left 0.
 */
static int mount_identity_at(int folder, const char *name, dev_t device, mount_identity *identity) {
    identity->device = device;
    identity->mount = 0;
    identity->has_mount = 0;
#ifdef __linux__
    identity->has_mount =
        mount_by_statx(folder, name, &identity->mount) || mount_by_file_handle(folder, name, &identity->mount);
    return identity->has_mount ? 0 : errno;
#else
    (void)folder;
    (void)name;
    return 0;
#endif
}

typedef struct removal {
    int parent;
    removal_frame *frames;
    size_t depth;
    size_t capacity;
    mount_identity clone;
    disk_removal_refusal reason;
} removal;

static naula_status refuse_mount(removal *removing, disk_removal_refusal reason, unsigned long code,
                                 unsigned long *error) {
    removing->reason = reason;
    *error = code;
    return NAULA_REFUSED;
}

/**
 * NAULA_OK when `found` is on the mount `expected` is on. On Linux an entry whose mount id neither way can read
 * cannot be told from a bind mount on the same device, so it is refused as unverifiable.
 */
static naula_status judge_mount(removal *removing, const mount_identity *expected, const mount_identity *found,
                                unsigned long *error) {
    if (found->device != expected->device) {
        return refuse_mount(removing, DISK_REMOVAL_MOUNT_INSIDE, EXDEV, error);
    }
#ifdef __linux__
    if (!expected->has_mount || !found->has_mount) {
        return refuse_mount(removing, DISK_REMOVAL_MOUNT_UNVERIFIABLE, ENOTSUP, error);
    }
    if (found->mount != expected->mount) {
        return refuse_mount(removing, DISK_REMOVAL_MOUNT_INSIDE, EXDEV, error);
    }
#endif
    return NAULA_OK;
}

/**
 * The clone must be on its parent folder's mount, so a clone that is itself a mount point is refused; its mount is
 * then recorded, and a folder below it on any other is refused before anything in it is touched.
 */
static naula_status check_mount(removal *removing, int folder, const struct stat *found, unsigned long *error) {
    mount_identity identity;
    mount_identity_at(folder, "", found->st_dev, &identity);
    if (removing->depth > 0) {
        return judge_mount(removing, &removing->clone, &identity, error);
    }
    struct stat parent_found;
    if (fstat(removing->parent, &parent_found) != 0) {
        return refused(error);
    }
    mount_identity parent;
    mount_identity_at(removing->parent, "", parent_found.st_dev, &parent);
    removing->clone = identity;
    return judge_mount(removing, &parent, &identity, error);
}

/** Takes ownership of `folder` and `name`, closing and releasing both when it cannot push them. */
static naula_status push_frame(removal *removing, int folder, char *name, unsigned long *error) {
    struct stat found;
    naula_status status = fstat(folder, &found) == 0 ? check_mount(removing, folder, &found, error) : refused(error);
    if (status == NAULA_OK) {
        status = let_owner_empty(folder, error);
    }
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
    memset(frame, 0, sizeof *frame);
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

static void pop_frame(removal *removing) {
    removal_frame *top = &removing->frames[--removing->depth];
    if (top->folder >= 0) {
        close(top->folder);
    }
    naula_memory_release(top->name);
    naula_memory_release(top->listing.names);
}

static naula_status snapshot_append(listing_snapshot *taken, const char *name) {
    size_t needed = strlen(name) + 1;
    if (taken->length + needed > taken->capacity) {
        size_t grown = taken->capacity == 0 ? 64 : taken->capacity * 2;
        while (grown < taken->length + needed) {
            grown *= 2;
        }
        char *larger = naula_memory_reallocate(taken->names, grown);
        if (larger == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        taken->names = larger;
        taken->capacity = grown;
    }
    memcpy(taken->names + taken->length, name, needed);
    taken->length += needed;
    return NAULA_OK;
}

/**
 * Every name in `folder`, read before any is acted on: a listing that removes as it reads can be fed new names behind
 * its cursor for as long as another program writes, so a pass would never end.
 */
static naula_status read_snapshot(int folder, listing_snapshot *taken, unsigned long *error) {
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
    while (status == NAULA_OK) {
        errno = 0;
        struct dirent *entry = readdir(listing);
        if (entry == NULL) {
            status = errno != 0 ? refused(error) : NAULA_OK;
            break;
        }
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            status = snapshot_append(taken, entry->d_name);
        }
    }
    closedir(listing);
    return status;
}

/** Unlinks the entry `name` of `folder`, which is no real folder, unless it is mounted there from elsewhere. */
static naula_status remove_entry(removal *removing, int folder, const char *name, const struct stat *found,
                                 unsigned long *error) {
    mount_identity identity;
    if (mount_identity_at(folder, name, found->st_dev, &identity) == ENOENT) {
        return NAULA_OK;
    }
    naula_status status = judge_mount(removing, &removing->clone, &identity, error);
    if (status != NAULA_OK) {
        return status;
    }
    return unlinkat(folder, name, 0) == 0 || errno == ENOENT ? NAULA_OK : refused(error);
}

/**
 * Removes the entry `name` of `folder` unless it is a real folder, which is handed back opened in `descend`; a file
 * on another file system or mount, a bind-mounted file included, is refused, never removed.
 */
static naula_status take_entry(removal *removing, int folder, const char *name, int *descend, char **descend_name,
                               unsigned long *error) {
    struct stat found;
    if (fstatat(folder, name, &found, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? NAULA_OK : refused(error);
    }
    if (!S_ISDIR(found.st_mode)) {
        return remove_entry(removing, folder, name, &found, error);
    }
    *descend_name = copy_of(name, strlen(name));
    if (*descend_name == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    *descend = open_folder_at(folder, name);
    if (*descend < 0) {
        naula_status status = refused(error);
        naula_memory_release(*descend_name);
        *descend_name = NULL;
        return status;
    }
    return NAULA_OK;
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

/** Removes the emptied top folder and pops it; one that something refilled first stays, with `refilled` set. */
static naula_status remove_top_folder(removal *removing, int *refilled, unsigned long *error) {
    *refilled = 0;
    int parent = -1;
    naula_status status = parent_of_top(removing, &parent, error);
    removal_frame *top = &removing->frames[removing->depth - 1];
    if (status == NAULA_OK && unlinkat(parent, top->name, AT_REMOVEDIR) != 0) {
        if (errno == ENOTEMPTY || errno == EEXIST) {
            *refilled = 1;
            return NAULA_OK;
        }
        status = refused(error);
    }
    pop_frame(removing);
    return status;
}

/** A folder found holding names after a snapshot of it was worked through was refilled by another program. */
static naula_status count_refill(removal *removing, removal_frame *top, unsigned long *error) {
    if (++top->refills < DISK_REMOVAL_REFILL_PASSES) {
        return NAULA_OK;
    }
    removing->reason = DISK_REMOVAL_REFILLED;
    *error = ENOTEMPTY;
    return NAULA_REFUSED;
}

/** Takes a new snapshot of the top folder once the last is worked through, and removes the folder when it is empty. */
static naula_status relist_top(removal *removing, unsigned long *error) {
    removal_frame *top = &removing->frames[removing->depth - 1];
    int worked_through = top->listed;
    top->listed = 1;
    top->listing.length = 0;
    top->next = 0;
    naula_status status = read_snapshot(top->folder, &top->listing, error);
    if (status != NAULA_OK) {
        return status;
    }
    if (top->listing.length > 0) {
        return worked_through ? count_refill(removing, top, error) : NAULA_OK;
    }
    int refilled = 0;
    status = remove_top_folder(removing, &refilled, error);
    return status == NAULA_OK && refilled ? count_refill(removing, top, error) : status;
}

/**
 * One step of the walk: the top folder's next name goes, or is pushed when it is a real folder, so the folder below
 * carries on with its own snapshot once that one is removed; a snapshot worked through is followed by a new one.
 */
static naula_status take_one_step(removal *removing, unsigned long *error) {
    removal_frame *top = &removing->frames[removing->depth - 1];
    if (top->next >= top->listing.length) {
        return relist_top(removing, error);
    }
    const char *name = top->listing.names + top->next;
    top->next += strlen(name) + 1;
    int descend = -1;
    char *child = NULL;
    naula_status status = take_entry(removing, top->folder, name, &descend, &child, error);
    return status == NAULA_OK && descend >= 0 ? push_frame(removing, descend, child, error) : status;
}

/** Empties and removes the folder `name` in `parent`, depth first, on its own stack rather than the call stack. */
static naula_status remove_tree_at(int parent, const char *name, disk_removal_refusal *reason, unsigned long *error) {
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
        status = take_one_step(&removing, error);
    }
    while (removing.depth > 0) {
        pop_frame(&removing);
    }
    naula_memory_release(removing.frames);
    *reason = removing.reason;
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

naula_status disk_tree_remove(const char *root, const char *below, disk_removal_refusal *reason,
                              unsigned long *error) {
    *reason = DISK_REMOVAL_REFUSED;
    *error = 0;
    char *copy = copy_of(below, strlen(below));
    if (copy == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    int parent = -1;
    const char *name = NULL;
    naula_status status = walk_to_parent(root, copy, &parent, &name, error);
    if (status == NAULA_OK) {
        status = remove_tree_at(parent, name, reason, error);
    }
    if (status == NAULA_OK) {
        fsync(parent);
    }
    if (parent >= 0) {
        close(parent);
    }
    naula_memory_release(copy);
    if (status == NAULA_REFUSED && *reason == DISK_REMOVAL_REFUSED && *error == EBUSY) {
        *reason = DISK_REMOVAL_HELD;
    }
    return status;
}

naula_status disk_identity_of(const char *path, disk_identity *identity, unsigned long *error) {
    struct stat found;
    *error = 0;
    memset(identity, 0, sizeof *identity);
    if (lstat(path, &found) != 0) {
        *error = (unsigned long)errno;
        return errno == ENOENT || errno == ENOTDIR ? NAULA_NOT_FOUND : NAULA_REFUSED;
    }
    identity->volume = (uint64_t)found.st_dev;
    identity->low = (uint64_t)found.st_ino;
    return NAULA_OK;
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
