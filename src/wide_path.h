#ifndef NAULA_WIDE_PATH_H
#define NAULA_WIDE_PATH_H

#ifdef _WIN32

#include <wchar.h>

#include "naula/status.h"

/** UTF-8 to UTF-16, refusing text that is not UTF-8. Release with naula_memory_release. */
naula_status wide_text(const char *text, wchar_t **wide);

/**
 * A path as Windows opens it past MAX_PATH: made absolute, then given the `\\?\` prefix, since a path under a deep
 * folder passes 260 characters, where a path without the prefix is refused.
 */
naula_status wide_path(const char *path, wchar_t **wide);

/** UTF-16 to UTF-8. Release with naula_memory_release. */
naula_status narrow_text(const wchar_t *wide, char **text);

#endif

#endif
