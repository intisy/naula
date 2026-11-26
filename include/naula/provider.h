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

#include "naula/metadata.h"
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
#define NAULA_OP_CREATE (1u << 7)
#define NAULA_OP_RENAME (1u << 8)
#define NAULA_OP_TRANSFER (1u << 9)
#define NAULA_OP_ARCHIVE (1u << 10)
#define NAULA_OP_READ_METADATA (1u << 11)
#define NAULA_OP_WRITE_METADATA (1u << 12)

#define NAULA_UNKNOWN_PROVIDER_FIX "install a plugin that provides a resolver"
#define NAULA_ACCOUNT_RENAME_FIX "rename the account on the host's website, then change the account in homes"

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
 * naula_memory_release. First shape ends at `release_index`; the writes, `create` to `write_metadata`, were appended
 * and are read only when `size` covers `write_metadata` (naula_provider_offers_writes).
 *
 * Every write needs a token and refuses NULL, and a name or account that is not a plain name or starts with `-`,
 * before any request. `rename`, `transfer` and `archive` keep the number. A write the host refuses with a 404 is
 * NAULA_NOT_FOUND only once a lookup by number says the repository is gone; on a live one it is refused
 * `not_permitted`. A 409 on a name is `name_taken`, a 422 `refused_by_validation`. `create` always sends the
 * visibility, private unless `metadata` says public, so a repository is never public before its file says so, and the
 * caller writes the rest of `metadata` with `write_metadata`. `write_metadata` writes only the members whose bit is
 * in `wanted->present` and answers in `applied` the members the host shows written, also when it fails part way; a
 * member it cannot write, such as a setting the host lacks, is left out of `applied` and is not an error. The codes
 * are listed in docs/providers.md. `read_metadata` fills `out`, its `size` set by the caller,
 * released with naula_metadata_release.
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
    naula_status (*create)(void *context, const char *host, const char *account, const char *name,
                           const naula_metadata *metadata, const char *token, uint64_t *number,
                           naula_refusal *refusal);
    naula_status (*rename)(void *context, const char *host, uint64_t number, const char *name, const char *token,
                           naula_refusal *refusal);
    naula_status (*transfer)(void *context, const char *host, uint64_t number, const char *account,
                             const char *token, naula_refusal *refusal);
    naula_status (*archive)(void *context, const char *host, uint64_t number, const char *token,
                            naula_refusal *refusal);
    naula_status (*read_metadata)(void *context, const char *host, uint64_t number, const char *token,
                                  naula_metadata *out, naula_refusal *refusal);
    naula_status (*write_metadata)(void *context, const char *host, uint64_t number, const naula_metadata *wanted,
                                   const char *token, uint32_t *applied, naula_refusal *refusal);
} naula_provider;

/** Whether provider's size covers the writes, so its write members may be read. */
int naula_provider_offers_writes(const naula_provider *provider);

/**
 * Refuses renaming the account word's homes live under, `unsupported_operation` with NAULA_ACCOUNT_RENAME_FIX: naula
 * renames no account (spec 7.4, which leaves unmeasured whether every provider's API could), so the person renames
 * it on the website.
 */
naula_status naula_refuse_account_rename(const char *word, naula_refusal *refusal);

typedef struct naula_providers naula_providers;

/** A registry holding the built-in providers. */
naula_status naula_providers_open(naula_providers **providers);

/**
 * Refuses a provider whose size does not cover its first shape, one whose size covers the writes with a write left
 * NULL, and a second provider for a word. The registry keeps the provider's own size, so a provider of the first
 * shape offers no writes.
 */
naula_status naula_providers_register(naula_providers *providers, const naula_provider *provider,
                                      naula_refusal *refusal);

/** The provider for `word`, or NULL with a refusal whose fix is NAULA_UNKNOWN_PROVIDER_FIX. */
const naula_provider *naula_providers_find(const naula_providers *providers, const char *word,
                                           naula_refusal *refusal);

void naula_providers_close(naula_providers *providers);

/** GitHub and GitHub Enterprise: host NULL or github.com is api.github.com, another host is https://<host>/api/v3. */
const naula_provider *naula_provider_github(void);

/** Gitea and Forgejo, one API under two words: the home names the host, https://<host>/api/v1. */
const naula_provider *naula_provider_gitea(void);
const naula_provider *naula_provider_forgejo(void);

/**
 * A home that is a domain. It supports no NAULA_OP_ operation; what it reads is the identity document, through
 * naula_web_document.
 */
const naula_provider *naula_provider_web(void);

/**
 * Reads https://<domain>/.well-known/repo-identity/<hex>.json, the document `{ "id", "homes" }` a domain serves for
 * the repo whose root commit is hex (40 or 64 lowercase hex). The bytes are released with naula_memory_release.
 * NAULA_NOT_FOUND is answered for a 404 only. A domain that does not resolve is NAULA_UNREACHABLE, refused
 * `host_not_resolved`, and a transfer that did not finish in time is NAULA_TIMED_OUT: neither says the domain is gone,
 * since no transport tells an authoritative NXDOMAIN from an offline machine. A document whose id is another repo's is
 * refused.
 */
naula_status naula_web_document(const char *domain, const char *hex, uint8_t **bytes, size_t *length,
                                naula_refusal *refusal);

#ifdef __cplusplus
}
#endif

#endif
