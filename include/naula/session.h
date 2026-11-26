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
 * How a session resolves. `cache_directory` NULL keeps no cache; otherwise confirmations are kept in
 * `<cache_directory>/identities/<hex>.json`. `providers` NULL makes the session open and own a registry of the
 * built-in providers, and a registry passed in must outlive the session. `history` NULL reads history through each
 * provider, the installer's way. `token_for` answers a token for a provider word and host, or NULL for none: naula
 * reads it during the call it was asked for and never copies it, so the caller decides how long a secret lives.
 * `accept_primary` answers nonzero to accept a primary other than the one the cache holds, and NULL refuses every
 * such change. `offline` answers from the cache alone. Every callback receives `context`. First shape ends at
 * `context`.
 */
typedef struct naula_session_options {
    uint32_t size;
    const char *cache_directory;
    naula_providers *providers;
    const naula_history *history;
    const char *(*token_for)(void *context, const char *provider, const char *host);
    int (*accept_primary)(void *context, const char *id, const naula_place *was, const naula_place *becomes);
    int offline;
    void *context;
} naula_session_options;

typedef struct naula_session naula_session;

/** Copies what it needs of `options`. NAULA_REFUSED for options or a history smaller than their first shape. */
naula_status naula_session_open(const naula_session_options *options, naula_session **session);

void naula_session_close(naula_session *session);

#ifdef __cplusplus
}
#endif

#endif
