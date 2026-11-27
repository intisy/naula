#include <windows.h>

#include <wchar.h>

#include "naula/memory.h"
#include "wide_path.h"

naula_status wide_text(const char *text, wchar_t **wide) {
    *wide = NULL;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (count <= 0) {
        return NAULA_REFUSED;
    }
    *wide = naula_memory_allocate((size_t)count * sizeof **wide);
    if (*wide == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, *wide, count) != count) {
        naula_memory_release(*wide);
        *wide = NULL;
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

naula_status narrow_text(const wchar_t *wide, char **text) {
    *text = NULL;
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    if (count <= 0) {
        return NAULA_REFUSED;
    }
    *text = naula_memory_allocate((size_t)count);
    if (*text == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, *text, count, NULL, NULL) != count) {
        naula_memory_release(*text);
        *text = NULL;
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

static naula_status prefixed(const wchar_t *full, wchar_t **wide) {
    int already = full[0] == L'\\' && full[1] == L'\\' && (full[2] == L'?' || full[2] == L'.');
    int unc = !already && full[0] == L'\\' && full[1] == L'\\';
    const wchar_t *prefix = already ? L"" : (unc ? L"\\\\?\\UNC\\" : L"\\\\?\\");
    const wchar_t *rest = unc ? full + 2 : full;
    size_t prefix_length = wcslen(prefix);
    size_t rest_length = wcslen(rest);
    *wide = naula_memory_allocate((prefix_length + rest_length + 1) * sizeof **wide);
    if (*wide == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    wmemcpy(*wide, prefix, prefix_length);
    wmemcpy(*wide + prefix_length, rest, rest_length + 1);
    return NAULA_OK;
}

naula_status wide_path(const char *path, wchar_t **wide) {
    wchar_t *plain = NULL;
    *wide = NULL;
    naula_status status = wide_text(path, &plain);
    if (status != NAULA_OK) {
        return status;
    }
    DWORD needed = GetFullPathNameW(plain, 0, NULL, NULL);
    wchar_t *full = needed == 0 ? NULL : naula_memory_allocate((size_t)needed * sizeof *full);
    if (needed == 0 || full == NULL) {
        naula_memory_release(plain);
        return needed == 0 ? NAULA_REFUSED : NAULA_OUT_OF_MEMORY;
    }
    DWORD written = GetFullPathNameW(plain, needed, full, NULL);
    naula_memory_release(plain);
    status = written == 0 || written >= needed ? NAULA_REFUSED : prefixed(full, wide);
    naula_memory_release(full);
    return status;
}
