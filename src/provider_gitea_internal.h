#ifndef NAULA_PROVIDER_GITEA_INTERNAL_H
#define NAULA_PROVIDER_GITEA_INTERNAL_H

#include "forge_common.h"

typedef struct gitea_edition {
    const char *word;
    forge_flavor flavor;
} gitea_edition;

/** One call's host, repository number (0 for none) and token, under the word its edition answers to. */
typedef struct gitea_target {
    const gitea_edition *edition;
    const char *host;
    uint64_t number;
    const char *token;
} gitea_target;

/** What a 404 on a live repository is refused as. */
typedef struct missing_answer {
    const char *code;
    const char *what;
    const char *fix;
} missing_answer;

/** One request to https://<host>/api/v1/<route>; method NULL is GET and body NULL sends none. */
naula_status naula_gitea_request(const gitea_target *at, const char *method, const char *route, const cJSON *body,
                                 size_t capacity, forge_reply *reply, naula_refusal *refusal);

naula_repository naula_gitea_fresh_repository(void);

/** Looks the target's number up, refusing an answer for another number. */
naula_status naula_gitea_lookup(const gitea_target *at, naula_repository *out, naula_refusal *refusal);

/** `repos/<owner>/<name>` of repository, encoded; 0 when it does not fit. */
int naula_gitea_repository_prefix(const naula_repository *repository, char *prefix, size_t capacity);

/** Looks the number up again and refuses `repository_changed` when its names are no longer before's. */
naula_status naula_gitea_confirm_names(const gitea_target *at, const naula_repository *before, naula_refusal *refusal);

/** Settles a read by name: a 404 is refused as missing while the repository is live and NOT_FOUND once it is gone. */
naula_status naula_gitea_settle_read(const gitea_target *at, const naula_repository *before, naula_status status,
                                     const missing_answer *missing, naula_refusal *refusal);

/** Refuses a host older than Gitea 1.20, `unsupported_operation`. */
naula_status naula_gitea_require_version(const gitea_target *at, naula_refusal *refusal);

naula_status naula_gitea_create(void *context, const char *host, const char *account, const char *name,
                                const naula_metadata *metadata, const char *token, uint64_t *number,
                                naula_refusal *refusal);
naula_status naula_gitea_rename(void *context, const char *host, uint64_t number, const char *name, const char *token,
                                naula_refusal *refusal);
naula_status naula_gitea_transfer(void *context, const char *host, uint64_t number, const char *account,
                                  const char *token, naula_refusal *refusal);
naula_status naula_gitea_archive(void *context, const char *host, uint64_t number, const char *token,
                                 naula_refusal *refusal);
naula_status naula_gitea_read_metadata(void *context, const char *host, uint64_t number, const char *token,
                                       naula_metadata *out, naula_refusal *refusal);
naula_status naula_gitea_write_metadata(void *context, const char *host, uint64_t number,
                                        const naula_metadata *wanted, const char *token, uint32_t *applied,
                                        naula_refusal *refusal);

#endif
