#include <string.h>

#include "windows_name.h"

static const char *const DEVICE_NAMES[] = {"con", "prn", "aux", "nul", "conin$", "conout$", NULL};
static const char *const SUPERSCRIPT_DIGITS[] = {"\xc2\xb9", "\xc2\xb2", "\xc2\xb3", NULL};

static int is_named(const char *segment, size_t length, const char *name) {
    if (length != strlen(name)) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        char character = segment[index] >= 'A' && segment[index] <= 'Z' ? (char)(segment[index] - 'A' + 'a')
                                                                          : segment[index];
        if (character != name[index]) {
            return 0;
        }
    }
    return 1;
}

static int is_port_number(const char *digits, size_t length) {
    if (length == 1) {
        return digits[0] >= '1' && digits[0] <= '9';
    }
    for (size_t index = 0; SUPERSCRIPT_DIGITS[index] != NULL; index++) {
        if (length == 2 && memcmp(digits, SUPERSCRIPT_DIGITS[index], 2) == 0) {
            return 1;
        }
    }
    return 0;
}

/** Win32 reads the device from the name before its first dot, its trailing spaces dropped. */
static int is_device_name(const char *segment, size_t length) {
    const char *dot = memchr(segment, '.', length);
    size_t base = dot == NULL ? length : (size_t)(dot - segment);
    while (base > 0 && segment[base - 1] == ' ') {
        base--;
    }
    for (size_t index = 0; DEVICE_NAMES[index] != NULL; index++) {
        if (is_named(segment, base, DEVICE_NAMES[index])) {
            return 1;
        }
    }
    int is_port = base > 3 && (is_named(segment, 3, "com") || is_named(segment, 3, "lpt"));
    return is_port && is_port_number(segment + 3, base - 3);
}

int windows_name_is_safe(const char *segment, size_t length) {
    if (length > 0 && (segment[length - 1] == '.' || segment[length - 1] == ' ')) {
        return 0;
    }
    return !is_device_name(segment, length);
}
