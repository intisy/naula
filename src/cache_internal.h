#ifndef NAULA_CACHE_INTERNAL_H
#define NAULA_CACHE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "naula/home.h"
#include "naula/status.h"

#define NAULA_CACHE_PROBLEM_SIZE 256
#define NAULA_CACHE_ANCHOR_SIZE 13

/** The most homes one record holds: the writer never writes more than the reader accepts. */
#define NAULA_CACHE_HOME_LIMIT 64

/**
 * What a record says of a home. CONFIRMED was in the confirmed set when the record was last written and alone is
 * served offline; GUARD is kept only so its number still guards its account. A number that answered gone is a GUARD
 * and is asked again on the next run: a 404 is definite only for the run that saw it, since an expired token on a
 * private repo answers the same.
 */
typedef enum cache_state { CACHE_CONFIRMED = 1, CACHE_GUARD = 2 } cache_state;

/** One home, by number and the account it was under or by domain: its state, last confirmation and validator. */
typedef struct cache_home {
    naula_place place;
    char *account;
    cache_state state;
    int64_t confirmed;
    char *etag;
} cache_home;

/** `primary` indexes a CONFIRMED home when `has_primary` is 1, which it is whenever any home is CONFIRMED. */
typedef struct identity_cache {
    cache_home *homes;
    size_t count;
    size_t primary;
    int has_primary;
    int present;
} identity_cache;

/**
 * The anchor a record belongs to: the first 12 lowercase hex of the sha256 of the consumer's `at` places, each in its
 * text form (`github:1222336612`, `gitea@git.example.com:12`, `web:naula.dev` or the url), sorted bytewise and joined
 * by "\n" with no trailing newline. One record per anchor keeps a consumer's answer, offline or stale, its own: another
 * consumer's first contact with a copy never shapes it.
 */
naula_status naula_cache_anchor(const naula_place *places, size_t count, char anchor[NAULA_CACHE_ANCHOR_SIZE]);

/**
 * Reads `<directory>/identities/<hex>-<anchor>.json`. A missing file leaves `present` 0 and `problem` empty; a file
 * that cannot be read or is not a confirmation record for `hex` also leaves `present` 0, with `problem` saying why, so
 * it is never trusted. Answers NAULA_OUT_OF_MEMORY or NAULA_OK.
 */
naula_status naula_cache_read(const char *directory, const char *hex, const char *anchor, identity_cache *cache,
                              char *problem);

/**
 * Replaces the record whole: written to a temporary file beside it, then renamed over it. Past
 * NAULA_CACHE_HOME_LIMIT the oldest GUARD homes are left out; when CONFIRMED homes alone pass it, nothing is written.
 * A record that could not be written is left as it was and `problem` says why. Answers NAULA_OUT_OF_MEMORY or
 * NAULA_OK.
 */
naula_status naula_cache_write(const char *directory, const char *hex, const char *anchor, const identity_cache *cache,
                               char *problem);

void naula_cache_release(identity_cache *cache);

/** Whether `name` is a temporary file a writer of records makes: `<hex>-<12 hex>.json.<anything>.tmp`. */
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
