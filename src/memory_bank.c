#include <string.h>

#include "layout_apply_internal.h"
#include "layout_internal.h"
#include "naula/memory.h"

void naula_memory_bank_key(const char *real_path, char *key, size_t capacity) {
    if (capacity == 0) {
        return;
    }
    size_t length = strlen(real_path);
    if (length >= capacity) {
        key[0] = '\0';
        return;
    }
    for (size_t index = 0; index < length; index++) {
        char character = real_path[index];
        key[index] = character == ':' || character == '/' || character == '\\' ? '-' : character;
    }
    key[length] = '\0';
}

char *memory_bank_path(const char *banks, const char *real_path) {
    size_t capacity = strlen(real_path) + 1;
    char *key = naula_memory_allocate(capacity);
    if (key == NULL) {
        return NULL;
    }
    naula_memory_bank_key(real_path, key, capacity);
    char *path = key[0] == '\0' ? NULL : layout_join(banks, key);
    naula_memory_release(key);
    return path;
}
