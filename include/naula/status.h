#ifndef NAULA_STATUS_H
#define NAULA_STATUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Every answer of naula, as a code and never `errno`. NOT_FOUND is the only answer that says a home is dead;
 * TIMED_OUT and UNREACHABLE say nothing about whether it still exists. Enums append only.
 */
typedef enum naula_status {
    NAULA_OK = 0,
    NAULA_REFUSED = 1,
    NAULA_MALFORMED = 2,
    NAULA_OUT_OF_MEMORY = 3,
    NAULA_UNREACHABLE = 4,
    NAULA_TIMED_OUT = 5,
    NAULA_NOT_FOUND = 6
} naula_status;

/**
 * Why naula declined: a stable `code`, a `detail` for people and a `fix` sentence. `fix` points at text the caller
 * keeps alive, normally a string literal. First shape ends at `fix`.
 */
typedef struct naula_refusal {
    uint32_t size;
    char code[64];
    char detail[1024];
    const char *fix;
} naula_refusal;

/** Fills `refusal`, truncating `code` and the formatted `detail` to their arrays with a terminator. NULL does nothing. */
void naula_refuse(naula_refusal *refusal, const char *code, const char *fix, const char *format, ...);

/** Zeroes `length` bytes in a way the compiler may not drop, for a secret before its block is released. */
void naula_overwrite(void *bytes, size_t length);

#ifdef __cplusplus
}
#endif

#endif
