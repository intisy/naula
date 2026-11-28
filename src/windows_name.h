#ifndef NAULA_WINDOWS_NAME_H
#define NAULA_WINDOWS_NAME_H

#include <stddef.h>

/**
 * Whether Windows holds the UTF-8 path segment as itself: not with a trailing dot or space, which Win32 drops, and
 * not a DOS device name (CON, PRN, AUX, NUL, CONIN$, CONOUT$, COM and LPT with a digit 1 to 9 or a superscript one,
 * two or three), with or without an extension, in any case. NtCreateFile would make such a name literally, where
 * Win32 opens something else by it, so every platform refuses it and a plan stays portable.
 */
int windows_name_is_safe(const char *segment, size_t length);

#endif
