#include <windows.h>
#include <winioctl.h>

#include <string.h>
#include <wchar.h>

#include "disk.h"
#include "disk_windows_internal.h"
#include "naula/memory.h"
#include "wide_path.h"

#define REPARSE_BUFFER_SIZE (16 * 1024)

/**
 * The fixed part of a mount point's REPARSE_DATA_BUFFER, which only the driver kit declares. A symlink's carries a
 * 32-bit flags word after it, before the names.
 */
typedef struct reparse_header {
    ULONG tag;
    USHORT data_length;
    USHORT reserved;
    USHORT substitute_offset;
    USHORT substitute_length;
    USHORT print_offset;
    USHORT print_length;
} reparse_header;

#define SYMLINK_FLAGS_SIZE sizeof(ULONG)

static naula_status refused(unsigned long *error) {
    *error = GetLastError();
    return NAULA_REFUSED;
}

static int is_missing_error(DWORD code) {
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

static int is_link_tag(DWORD tag) {
    return tag == IO_REPARSE_TAG_MOUNT_POINT || tag == IO_REPARSE_TAG_SYMLINK;
}

static naula_status reparse_tag(const wchar_t *wide, DWORD *tag, unsigned long *error) {
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(wide, &found);
    if (search == INVALID_HANDLE_VALUE) {
        return refused(error);
    }
    FindClose(search);
    *tag = found.dwReserved0;
    return NAULA_OK;
}

naula_status disk_kind_of(const char *path, disk_kind *kind, unsigned long *error) {
    wchar_t *wide = NULL;
    *kind = DISK_MISSING;
    *error = 0;
    naula_status status = wide_path(path, &wide);
    if (status != NAULA_OK) {
        return status;
    }
    DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD code = GetLastError();
        naula_memory_release(wide);
        *error = code;
        return is_missing_error(code) ? NAULA_OK : NAULA_REFUSED;
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        DWORD tag = 0;
        status = reparse_tag(wide, &tag, error);
        *kind = is_link_tag(tag) ? DISK_LINK : DISK_OTHER;
    } else {
        *kind = attributes & FILE_ATTRIBUTE_DIRECTORY ? DISK_FOLDER : DISK_FILE;
    }
    naula_memory_release(wide);
    return status;
}

static naula_status read_reparse_buffer(const wchar_t *wide, unsigned char *buffer, unsigned long *error) {
    HANDLE handle = CreateFileW(wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return refused(error);
    }
    DWORD returned = 0;
    BOOL read = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, NULL, 0, buffer, REPARSE_BUFFER_SIZE, &returned, NULL);
    naula_status status = read ? NAULA_OK : refused(error);
    CloseHandle(handle);
    if (status == NAULA_OK && returned < sizeof(reparse_header)) {
        *error = ERROR_INVALID_REPARSE_DATA;
        status = NAULA_REFUSED;
    }
    return status;
}

static naula_status substitute_name(const unsigned char *buffer, wchar_t **name, unsigned long *error) {
    reparse_header header;
    memcpy(&header, buffer, sizeof header);
    size_t names_at = sizeof header + (header.tag == IO_REPARSE_TAG_SYMLINK ? SYMLINK_FLAGS_SIZE : 0);
    size_t end = (size_t)header.substitute_offset + header.substitute_length;
    if (!is_link_tag(header.tag) || names_at + end > REPARSE_BUFFER_SIZE || header.substitute_length % 2 != 0) {
        *error = ERROR_INVALID_REPARSE_DATA;
        return NAULA_REFUSED;
    }
    size_t characters = header.substitute_length / sizeof(wchar_t);
    *name = naula_memory_allocate((characters + 1) * sizeof **name);
    if (*name == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    memcpy(*name, buffer + names_at + header.substitute_offset, header.substitute_length);
    (*name)[characters] = L'\0';
    return NAULA_OK;
}

static void forward_slashes(char *text) {
    for (; *text != '\0'; text++) {
        if (*text == '\\') {
            *text = '/';
        }
    }
}

naula_status disk_link_target(const char *path, char **target, unsigned long *error) {
    wchar_t *wide = NULL;
    *target = NULL;
    *error = 0;
    naula_status status = wide_path(path, &wide);
    unsigned char *buffer = status == NAULA_OK ? naula_memory_allocate(REPARSE_BUFFER_SIZE) : NULL;
    if (status == NAULA_OK && buffer == NULL) {
        status = NAULA_OUT_OF_MEMORY;
    }
    wchar_t *name = NULL;
    if (status == NAULA_OK) {
        status = read_reparse_buffer(wide, buffer, error);
    }
    if (status == NAULA_OK) {
        status = substitute_name(buffer, &name, error);
    }
    if (status == NAULA_OK) {
        const wchar_t *shown = wcsncmp(name, L"\\??\\", 4) == 0 ? name + 4 : name;
        status = narrow_text(shown, target);
        if (status == NAULA_OK) {
            forward_slashes(*target);
        }
    }
    naula_memory_release(name);
    naula_memory_release(buffer);
    naula_memory_release(wide);
    return status;
}

static naula_status full_target(const char *target, wchar_t **full) {
    wchar_t *plain = NULL;
    *full = NULL;
    naula_status status = wide_text(target, &plain);
    if (status != NAULA_OK) {
        return status;
    }
    DWORD needed = GetFullPathNameW(plain, 0, NULL, NULL);
    *full = needed == 0 ? NULL : naula_memory_allocate((size_t)needed * sizeof **full);
    if (needed == 0 || *full == NULL) {
        naula_memory_release(plain);
        return needed == 0 ? NAULA_REFUSED : NAULA_OUT_OF_MEMORY;
    }
    DWORD written = GetFullPathNameW(plain, needed, *full, NULL);
    naula_memory_release(plain);
    if (written == 0 || written >= needed) {
        naula_memory_release(*full);
        *full = NULL;
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

/** A mount point buffer naming `full` as both its substitute name, `\??\` prefixed, and its print name. */
static naula_status mount_point_buffer(const wchar_t *full, unsigned char **buffer, DWORD *size) {
    size_t print_bytes = wcslen(full) * sizeof(wchar_t);
    size_t substitute_bytes = print_bytes + 4 * sizeof(wchar_t);
    size_t names_bytes = substitute_bytes + sizeof(wchar_t) + print_bytes + sizeof(wchar_t);
    size_t total = sizeof(reparse_header) + names_bytes;
    if (total > REPARSE_BUFFER_SIZE) {
        return NAULA_REFUSED;
    }
    *buffer = naula_memory_allocate(total);
    if (*buffer == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    memset(*buffer, 0, total);
    reparse_header header;
    header.tag = IO_REPARSE_TAG_MOUNT_POINT;
    header.data_length = (USHORT)(total - 8);
    header.reserved = 0;
    header.substitute_offset = 0;
    header.substitute_length = (USHORT)substitute_bytes;
    header.print_offset = (USHORT)(substitute_bytes + sizeof(wchar_t));
    header.print_length = (USHORT)print_bytes;
    memcpy(*buffer, &header, sizeof header);
    unsigned char *names = *buffer + sizeof header;
    memcpy(names, L"\\??\\", 4 * sizeof(wchar_t));
    memcpy(names + 4 * sizeof(wchar_t), full, print_bytes);
    memcpy(names + header.print_offset, full, print_bytes);
    *size = (DWORD)total;
    return NAULA_OK;
}

#define LINK_MAKE_ACCESS (FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES | DELETE | SYNCHRONIZE)
#define LINK_REMOVE_ACCESS (DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE)

/** Makes the empty folder `name` in the walked folder and turns it into the mount point `buffer` describes. */
static naula_status make_mount_point(const disk_walk *walk, const unsigned char *buffer, DWORD size,
                                     unsigned long *error) {
    HANDLE made = INVALID_HANDLE_VALUE;
    naula_status status = disk_open_at(&walk->opener, walk->parent, walk->name, walk->name_bytes, LINK_MAKE_ACCESS, 0,
                                       FILE_CREATE, FILE_DIRECTORY_FILE | DISK_OPEN_OPTIONS, &made, error);
    if (status != NAULA_OK) {
        return status;
    }
    DWORD returned = 0;
    if (!DeviceIoControl(made, FSCTL_SET_REPARSE_POINT, (LPVOID)buffer, size, NULL, 0, &returned, NULL)) {
        status = refused(error);
        unsigned long ignored = 0;
        disk_delete_by_handle(made, FILE_ATTRIBUTE_DIRECTORY, &ignored);
    }
    CloseHandle(made);
    return status;
}

naula_status disk_link_make(const char *root, const char *below, const char *target, disk_walk_purpose purpose,
                            unsigned long *error) {
    wchar_t *full = NULL;
    unsigned char *buffer = NULL;
    DWORD size = 0;
    *error = 0;
    naula_status status = full_target(target, &full);
    if (status == NAULA_OK) {
        status = mount_point_buffer(full, &buffer, &size);
    }
    disk_walk walk;
    walk.parent = INVALID_HANDLE_VALUE;
    walk.below = NULL;
    if (status == NAULA_OK) {
        status = disk_walk_to_parent(root, below, purpose, &walk, error);
    }
    if (status == NAULA_OK) {
        status = make_mount_point(&walk, buffer, size, error);
    }
    disk_walk_release(&walk);
    naula_memory_release(buffer);
    naula_memory_release(full);
    return status;
}

/** Only a junction or a symlink, judged by the handle that removes it, never by a path read before. */
static naula_status refuse_unless_link(HANDLE handle, DWORD *attributes, unsigned long *error) {
    FILE_ATTRIBUTE_TAG_INFO tag;
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof tag)) {
        return refused(error);
    }
    *attributes = tag.FileAttributes;
    if (!(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !is_link_tag(tag.ReparseTag)) {
        *error = ERROR_NOT_A_REPARSE_POINT;
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

/**
 * Opens the regular file the walk names by the walked folder's handle, judged by that handle: a link, a folder or
 * anything else there is refused, and nothing there is NAULA_NOT_FOUND. `file` is left for the caller to close.
 */
static naula_status open_regular_file(const disk_walk *walk, ACCESS_MASK access, ULONG share, HANDLE *file,
                                      DWORD *attributes, unsigned long *error) {
    naula_status status = disk_open_at(&walk->opener, walk->parent, walk->name, walk->name_bytes, access, share,
                                       FILE_OPEN, FILE_NON_DIRECTORY_FILE | DISK_OPEN_OPTIONS, file, error);
    if (status == NAULA_REFUSED && is_missing_error(*error)) {
        return NAULA_NOT_FOUND;
    }
    if (status != NAULA_OK) {
        return status;
    }
    FILE_ATTRIBUTE_TAG_INFO tag;
    memset(&tag, 0, sizeof tag);
    if (!GetFileInformationByHandleEx(*file, FileAttributeTagInfo, &tag, sizeof tag)) {
        return refused(error);
    }
    if ((tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        *error = ERROR_INVALID_FUNCTION;
        return NAULA_REFUSED;
    }
    *attributes = tag.FileAttributes;
    return NAULA_OK;
}

naula_status disk_file_remove(const char *root, const char *below, disk_walk_purpose purpose, unsigned long *error) {
    *error = 0;
    disk_walk walk;
    naula_status status = disk_walk_to_parent(root, below, purpose, &walk, error);
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD attributes = 0;
    if (status == NAULA_OK) {
        status = open_regular_file(&walk, LINK_REMOVE_ACCESS, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   &file, &attributes, error);
    }
    if (status == NAULA_OK) {
        status = disk_delete_by_handle(file, attributes, error);
    }
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
    disk_walk_release(&walk);
    return status;
}

static naula_status holds_exactly(HANDLE file, const uint8_t *bytes, size_t length, int *holding,
                                  unsigned long *error) {
    uint8_t buffer[4096];
    size_t seen = 0;
    for (;;) {
        DWORD read_now = 0;
        if (!ReadFile(file, buffer, sizeof buffer, &read_now, NULL)) {
            return refused(error);
        }
        if (read_now == 0) {
            break;
        }
        if ((size_t)read_now > length - seen || memcmp(buffer, bytes + seen, read_now) != 0) {
            *holding = 0;
            return NAULA_OK;
        }
        seen += read_now;
    }
    *holding = seen == length;
    return NAULA_OK;
}

/** @implNote Opened sharing reads only, so no writer changes the file between the read and the removal by handle. */
naula_status disk_file_remove_if_holding(const char *root, const char *below, const uint8_t *bytes, size_t length,
                                         disk_walk_purpose purpose, int *holding, unsigned long *error) {
    *error = 0;
    *holding = 0;
    disk_walk walk;
    naula_status status = disk_walk_to_parent(root, below, purpose, &walk, error);
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD attributes = 0;
    if (status == NAULA_OK) {
        status = open_regular_file(&walk, FILE_READ_DATA | LINK_REMOVE_ACCESS, FILE_SHARE_READ, &file, &attributes,
                                   error);
    }
    if (status == NAULA_OK) {
        status = holds_exactly(file, bytes, length, holding, error);
    }
    if (status == NAULA_OK && *holding) {
        status = disk_delete_by_handle(file, attributes, error);
    }
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
    disk_walk_release(&walk);
    return status;
}

naula_status disk_link_remove(const char *root, const char *below, disk_walk_purpose purpose, unsigned long *error) {
    *error = 0;
    disk_walk walk;
    naula_status status = disk_walk_to_parent(root, below, purpose, &walk, error);
    HANDLE link = INVALID_HANDLE_VALUE;
    if (status == NAULA_OK) {
        status = disk_open_at(&walk.opener, walk.parent, walk.name, walk.name_bytes, LINK_REMOVE_ACCESS,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN, DISK_OPEN_OPTIONS, &link,
                              error);
    }
    DWORD attributes = 0;
    if (status == NAULA_OK) {
        status = refuse_unless_link(link, &attributes, error);
    }
    if (status == NAULA_OK) {
        status = disk_delete_by_handle(link, attributes, error);
    }
    if (link != INVALID_HANDLE_VALUE) {
        CloseHandle(link);
    }
    disk_walk_release(&walk);
    return status;
}
