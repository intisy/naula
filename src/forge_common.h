#ifndef NAULA_FORGE_COMMON_H
#define NAULA_FORGE_COMMON_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "naula/provider.h"

#define FORGE_ROOT_SIZE 300
#define FORGE_ROUTE_SIZE 1536
#define FORGE_SMALL_BODY_CAPACITY (256u * 1024u)
#define FORGE_FILE_BODY_CAPACITY (2u * 1024u * 1024u)
#define FORGE_HISTORY_BODY_CAPACITY (8u * 1024u * 1024u)

typedef struct forge_flavor {
    const char *label;
    const char *host_fix;
    const char *const *request_headers;
} forge_flavor;

typedef struct forge_reply {
    uint8_t *body;
    size_t length;
    char *etag;
} forge_reply;

typedef struct forge_repository_list {
    naula_repository *items;
    size_t count;
    size_t capacity;
} forge_repository_list;

void naula_forge_reply_release(forge_reply *reply);

int naula_forge_is_host_text(const char *host);
int naula_forge_is_name_segment(const char *text);
int naula_forge_is_commit_text(const char *text);
int naula_forge_equals_ignoring_case(const char *left, const char *right);
size_t naula_forge_encode_text(const char *text, int keep_slash, char *out, size_t capacity);

naula_status naula_forge_refuse_input(const forge_flavor *flavor, naula_refusal *refusal, const char *what);
naula_status naula_forge_refuse_malformed(const forge_flavor *flavor, naula_refusal *refusal, const char *what);
naula_status naula_forge_refuse_unsupported(const forge_flavor *flavor, naula_refusal *refusal, const char *operation,
                                            const char *fix);

/** One GET of an absolute url, the token sent as `Authorization: token <token>` when given. */
naula_status naula_forge_fetch(const forge_flavor *flavor, const char *url, const char *token, size_t capacity,
                               forge_reply *reply, naula_refusal *refusal);

cJSON *naula_forge_parse_reply(const forge_flavor *flavor, const forge_reply *reply, naula_status *status,
                               naula_refusal *refusal);
int naula_forge_read_exact_number(const cJSON *object, const char *member, uint64_t *number);
const char *naula_forge_text_member(const cJSON *object, const char *member);

int naula_forge_covers_first_shape(const naula_repository *out);
naula_status naula_forge_fill_repository(const forge_flavor *flavor, const cJSON *json, naula_repository *out,
                                         naula_refusal *refusal);

/** Whether a repository answer belongs to account/name, which a host that follows renames with a redirect may not. */
int naula_forge_answers_to_the_name(const cJSON *json, const char *account, const char *name);

naula_status naula_forge_append_page(const forge_flavor *flavor, forge_repository_list *list, const forge_reply *reply,
                                     size_t *page_count, naula_refusal *refusal);

#endif
