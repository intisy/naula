#ifndef NAULA_PROVIDER_H
#define NAULA_PROVIDER_H

/**
 * The provider interface: a C struct of function pointers a built-in module or a plugin fills, and the registry that
 * resolves a provider word to one. Every call takes the `host` the home names (NULL for the provider's own) and an
 * optional `token`, and answers NAULA_NOT_FOUND only for a repo that is gone, NAULA_TIMED_OUT for a transfer that did
 * not finish in time, which says nothing about whether the repo exists.
 */

#include <stddef.h>
#include <stdint.h>

#include "naula/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NAULA_OP_LOOKUP (1u << 0)
#define NAULA_OP_FIND (1u << 1)
#define NAULA_OP_READ_FILE (1u << 2)
#define NAULA_OP_CONTAINS (1u << 3)
#define NAULA_OP_PARENT_COUNT (1u << 4)
#define NAULA_OP_LIST_REPOS (1u << 5)
#define NAULA_OP_RELEASE_INDEX (1u << 6)

#define NAULA_UNKNOWN_PROVIDER_FIX "install a plugin that provides a resolver"

/**
 * What a provider knows of one repo. `fork_parent` is 0 for a repo that is no fork, `etag` NULL when the answer
 * carried none (kept for the cache). Release with naula_repository_release. First shape ends at `etag`.
 */
typedef struct naula_repository {
    uint32_t size;
    uint64_t number;
    char *account;
    char *name;
    char *default_branch;
    int archived;
    char *clone_url;
    uint64_t fork_parent;
    char *etag;
} naula_repository;

void naula_repository_release(naula_repository *repository);
void naula_repository_list_release(naula_repository *repositories, size_t count);

/**
 * `describe` answers the NAULA_OP_ bits this provider supports; a call to an operation it lacks is refused. `word` and
 * `context` outlive the registration. Bytes answered by read_file and release_index are released with
 * naula_memory_release. First shape ends at `release_index`.
 */
typedef struct naula_provider {
    uint32_t size;
    const char *word;
    void *context;
    naula_status (*describe)(void *context, uint32_t *supported, int *anonymous_lookup);
    naula_status (*lookup)(void *context, const char *host, uint64_t number, const char *token, naula_repository *out,
                           naula_refusal *refusal);
    naula_status (*find)(void *context, const char *host, const char *account, const char *name, const char *token,
                         uint64_t *number, naula_refusal *refusal);
    naula_status (*read_file)(void *context, const char *host, uint64_t number, const char *ref, const char *path,
                              const char *token, uint8_t **bytes, size_t *length, naula_refusal *refusal);
    naula_status (*contains)(void *context, const char *host, uint64_t number, const char *ref, const char *commit,
                             const char *token, int *contained, naula_refusal *refusal);
    naula_status (*parent_count)(void *context, const char *host, uint64_t number, const char *commit,
                                 const char *token, size_t *count, naula_refusal *refusal);
    naula_status (*list_repos)(void *context, const char *host, const char *account, const char *token,
                               naula_repository **out, size_t *count, naula_refusal *refusal);
    naula_status (*release_index)(void *context, const char *host, uint64_t number, const char *tag,
                                  const char *token, uint8_t **bytes, size_t *length, naula_refusal *refusal);
} naula_provider;

typedef struct naula_providers naula_providers;

/** A registry holding the built-in providers. */
naula_status naula_providers_open(naula_providers **providers);

/** Refuses a provider whose size does not cover its first shape, and a second provider for a word. */
naula_status naula_providers_register(naula_providers *providers, const naula_provider *provider,
                                      naula_refusal *refusal);

/** The provider for `word`, or NULL with a refusal whose fix is NAULA_UNKNOWN_PROVIDER_FIX. */
const naula_provider *naula_providers_find(const naula_providers *providers, const char *word,
                                           naula_refusal *refusal);

void naula_providers_close(naula_providers *providers);

/** GitHub and GitHub Enterprise: host NULL or github.com is api.github.com, another host is https://<host>/api/v3. */
const naula_provider *naula_provider_github(void);

#ifdef __cplusplus
}
#endif

#endif
