#ifndef NAULA_SESSION_H
#define NAULA_SESSION_H

#include <stdint.h>

#include "naula/history.h"
#include "naula/home.h"
#include "naula/provider.h"
#include "naula/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * How a session resolves. `cache_directory` NULL keeps no cache; otherwise it is an absolute path, and confirmations
 * are kept in `<cache_directory>/identities/<hex>-<anchor>.json`, one record per consumer's `at`. `providers` NULL
 * makes the session open and own a registry of the built-in providers, and a registry passed in must outlive the
 * session. `history` NULL reads history through each provider, the
 * installer's way. `token_for` answers a token for a provider word and host, or NULL for none: naula reads it during
 * the call it was asked for and never copies it, so the caller decides how long a secret lives. `accept_primary`
 * answers nonzero to accept a primary other than the one the cache holds, and NULL refuses every such change.
 * `accept_number` answers nonzero to take `becomes` as the number of `account` when records of this id that a new
 * `at` grew from hold another number for it (`was`), since nothing local can tell a re-registration from a genuine
 * change. It is asked once in a run for each such `was`, so one run names every candidate; NULL refuses, which leaves
 * the home that lists the account refused as `number-disputed` until it is accepted.
 * `offline` answers from the cache alone. Every callback receives `context`. First shape ends at `context`.
 */
typedef struct naula_session_options {
    uint32_t size;
    const char *cache_directory;
    naula_providers *providers;
    const naula_history *history;
    const char *(*token_for)(void *context, const char *provider, const char *host);
    int (*accept_primary)(void *context, const char *id, const naula_place *was, const naula_place *becomes);
    int (*accept_number)(void *context, const char *id, const naula_home *account, uint64_t was, uint64_t becomes);
    int offline;
    void *context;
} naula_session_options;

typedef struct naula_session naula_session;

/**
 * Copies what it needs of `options`. Refuses (`invalid_options`) options or a history smaller than their first shape,
 * and a `cache_directory` that is not absolute, which is never resolved against the working directory.
 */
naula_status naula_session_open(const naula_session_options *options, naula_session **session,
                                naula_refusal *refusal);

void naula_session_close(naula_session *session);

#ifdef __cplusplus
}
#endif

#endif
