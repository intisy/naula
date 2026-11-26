#ifndef NAULA_CACHE_INTERNAL_H
#define NAULA_CACHE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "naula/home.h"
#include "naula/status.h"

#define NAULA_CACHE_PROBLEM_SIZE 256

/** The most homes one record holds: the writer never writes more than the reader accepts. */
#define NAULA_CACHE_HOME_LIMIT 64

/**
 * What a record says of a home. CONFIRMED was in the confirmed set when the record was last written and alone is
 * served offline; GUARD was confirmed once and is kept so its number still guards its account; DEAD answered that it
 * is gone, is never asked again, and guards its account for good, so a re-registered account never stands in for it.
 */
typedef enum cache_state { CACHE_CONFIRMED = 1, CACHE_GUARD = 2, CACHE_DEAD = 3 } cache_state;

/** One home, by number and the account it was under or by domain: its state, last confirmation and validator. */
typedef struct cache_home {
    naula_place place;
    char *account;
    cache_state state;
    int64_t confirmed;
    char *etag;
} cache_home;

/** `primary` indexes a CONFIRMED home. */
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
 * Replaces the record whole: written to a temporary file beside it, then renamed over it. Past
 * NAULA_CACHE_HOME_LIMIT the oldest GUARD homes are left out; when CONFIRMED and DEAD homes alone pass it, nothing is
 * written. A record that could not be written is left as it was and `problem` says why. Answers NAULA_OUT_OF_MEMORY
 * or NAULA_OK.
 */
naula_status naula_cache_write(const char *directory, const char *hex, const identity_cache *cache, char *problem);

void naula_cache_release(identity_cache *cache);

/** Whether `name` is a temporary file a writer of records makes: `<40 or 64 hex>.json.<anything>.tmp`. */
int naula_cache_is_temporary_name(const char *name);

/** How old a temporary file must be before a later writer removes it as left behind. */
#define NAULA_CACHE_STALE_SECONDS 3600

/** The whole file at `path`: NAULA_NOT_FOUND when there is none, NAULA_REFUSED when it cannot be read. */
naula_status naula_cache_file_read(const char *path, uint8_t **bytes, size_t *length);

/**
 * Makes `directory` and its `identities` folder, removes temporary files an earlier writer left there more than an
 * hour ago, writes `bytes` to a new temporary file and renames that over `path`, removing the temporary file on any
 * failure. NAULA_REFUSED when the file system refused.
 */
naula_status naula_cache_file_replace(const char *directory, const char *path, const uint8_t *bytes, size_t length);

#endif
