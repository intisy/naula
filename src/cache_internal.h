#ifndef NAULA_CACHE_INTERNAL_H
#define NAULA_CACHE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "naula/home.h"
#include "naula/status.h"

#define NAULA_CACHE_PROBLEM_SIZE 256

/** One home confirmed before: by number (and the account it was under) or by domain, when, and its validator. */
typedef struct cache_home {
    naula_place place;
    char *account;
    int64_t confirmed;
    char *etag;
} cache_home;

typedef struct identity_cache {
    cache_home *homes;
    size_t count;
    size_t primary;
    int present;
} identity_cache;

/**
 * Reads `<directory>/identities/<hex>.json`. A missing file leaves `present` 0 and `problem` empty; a file that cannot
 * be read or is not a confirmation record for `hex` also leaves `present` 0, with `problem` saying why, so it is never
 * trusted. Answers NAULA_OUT_OF_MEMORY or NAULA_OK.
 */
naula_status naula_cache_read(const char *directory, const char *hex, identity_cache *cache, char *problem);

/**
 * Replaces the record whole: written to a temporary file beside it, then renamed over it. A record that could not be
 * written is left as it was and `problem` says why. Answers NAULA_OUT_OF_MEMORY or NAULA_OK.
 */
naula_status naula_cache_write(const char *directory, const char *hex, const identity_cache *cache, char *problem);

void naula_cache_release(identity_cache *cache);

/** The whole file at `path`: NAULA_NOT_FOUND when there is none, NAULA_REFUSED when it cannot be read. */
naula_status naula_cache_file_read(const char *path, uint8_t **bytes, size_t *length);

/**
 * Makes `directory` and its `identities` folder, writes `bytes` to a new temporary file in it and renames that over
 * `path`, removing the temporary file on any failure. NAULA_REFUSED when the file system refused.
 */
naula_status naula_cache_file_replace(const char *directory, const char *path, const uint8_t *bytes, size_t length);

#endif
