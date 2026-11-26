#ifndef NAULA_RESOLVE_INTERNAL_H
#define NAULA_RESOLVE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "cache_internal.h"
#include "naula/history.h"
#include "naula/provider.h"
#include "naula/repo_file.h"
#include "naula/resolve.h"
#include "naula/session.h"

#define NAULA_RESOLVE_DETAIL_SIZE 256
#define NAULA_NO_CANDIDATE ((size_t)-1)

struct naula_session {
    char *cache_directory;
    naula_providers *providers;
    int owns_providers;
    naula_history history;
    const char *(*token_for)(void *context, const char *provider, const char *host);
    int (*accept_primary)(void *context, const char *id, const naula_place *was, const naula_place *becomes);
    int offline;
    void *context;
};

const char *naula_session_token(const naula_session *session, const char *provider, const char *host);

/** A history that reads each provider home through its provider's `contains` and `parent_count`. */
void naula_history_over_providers(naula_session *session, naula_history *history);

typedef enum candidate_outcome {
    CANDIDATE_PENDING = 0,
    CANDIDATE_CONFIRMED,
    CANDIDATE_REFUSED,
    CANDIDATE_DEAD,
    CANDIDATE_UNKNOWN
} candidate_outcome;

/**
 * One home the resolver met. `wanted` marks one reached from `at`, the cache or a confirmed home's `homes`, which is
 * confirmed or refused in full; any other is only read, to see whom it lists, or probed as a fork parent. A home
 * listed by an account whose number was never learned has number 0 and keeps that `account`. `listed` maps each
 * entry of its homes to the candidate it named, NAULA_NO_CANDIDATE for itself and for one never reached.
 */
typedef struct candidate {
    naula_place place;
    char *account;
    int read;
    int readable;
    naula_repository repository;
    char *branch;
    naula_repo_file file;
    naula_home *document_homes;
    size_t document_home_count;
    size_t *listed;
    candidate_outcome outcome;
    char code[64];
    char detail[NAULA_RESOLVE_DETAIL_SIZE];
    int wanted;
    int probe;
    int evaluating;
    int expanded;
} candidate;

typedef struct resolver {
    naula_session *session;
    char hex[65];
    char id_text[70];
    candidate **candidates;
    size_t count;
    size_t capacity;
    size_t at_count;
    identity_cache cache;
} resolver;

/** The homes a readable candidate lists: its repo.json's, or a web home's document's. */
const naula_home *naula_candidate_homes(const candidate *subject, size_t *count);

/** Whether `home` names `subject`: the same provider, host and account, or the same domain. */
int naula_home_names(const naula_home *home, const candidate *subject);

/** `place` as people write it, with the account for a home known only by account. */
void naula_candidate_text(const candidate *subject, char *out, size_t size);

/** The index of the candidate at `place` (and `account` for number 0), adding it when new. REFUSED past the limit. */
naula_status naula_resolver_add(resolver *subject, const naula_place *place, const char *account, size_t *index);

/** Confirms or refuses the candidate at `index` in full. Answers NAULA_OUT_OF_MEMORY or NAULA_OK. */
naula_status naula_resolver_confirm(resolver *subject, size_t index, unsigned depth);

void naula_resolver_release_candidates(resolver *subject);

naula_status naula_resolver_copy_place(const naula_place *from, naula_place *to);

int naula_resolver_same_place(const naula_place *left, const char *left_account, const naula_place *right,
                              const char *right_account);

int naula_resolver_same_host(const char *word, const char *left, const char *right);

int naula_resolver_equals_ignoring_case(const char *left, const char *right);

#endif
