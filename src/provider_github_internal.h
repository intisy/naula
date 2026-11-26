#ifndef NAULA_PROVIDER_GITHUB_INTERNAL_H
#define NAULA_PROVIDER_GITHUB_INTERNAL_H

#include "forge_common.h"

const forge_flavor *naula_github_flavor(void);

/** One request to the API root the host names; method NULL is GET and body NULL sends none. */
naula_status naula_github_request(const char *host, const char *method, const char *route, const char *token,
                                  const cJSON *body, size_t capacity, forge_reply *reply, naula_refusal *refusal);

/** Looks the repository up by number: NAULA_OK while it is live, NAULA_NOT_FOUND once it is gone. */
naula_status naula_github_still_the_same_number(const char *host, uint64_t number, const char *token,
                                               naula_refusal *refusal);

naula_status naula_github_create(void *context, const char *host, const char *account, const char *name,
                                 const naula_metadata *metadata, const char *token, uint64_t *number,
                                 naula_refusal *refusal);
naula_status naula_github_rename(void *context, const char *host, uint64_t number, const char *name, const char *token,
                                 naula_refusal *refusal);
naula_status naula_github_transfer(void *context, const char *host, uint64_t number, const char *account,
                                   const char *token, naula_refusal *refusal);
naula_status naula_github_archive(void *context, const char *host, uint64_t number, const char *token,
                                  naula_refusal *refusal);
naula_status naula_github_read_metadata(void *context, const char *host, uint64_t number, const char *token,
                                        naula_metadata *out, naula_refusal *refusal);
naula_status naula_github_write_metadata(void *context, const char *host, uint64_t number,
                                         const naula_metadata *wanted, const char *token, uint32_t *applied,
                                         naula_refusal *refusal);

#endif
