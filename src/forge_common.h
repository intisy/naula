#ifndef NAULA_FORGE_COMMON_H
#define NAULA_FORGE_COMMON_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "naula/metadata.h"
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

/** A host's answer; `status` is the HTTP status of a refused answer, 0 when none arrived. */
typedef struct forge_reply {
    uint8_t *body;
    size_t length;
    char *etag;
    long status;
} forge_reply;

/** How a host names the metadata members a write sends: its homepage member, and its topics member, NULL for none. */
typedef struct forge_metadata_names {
    const char *homepage;
    const char *topics;
} forge_metadata_names;

/** The bodies a metadata write sends: `edit` carries `edit_members`, `topics` the topics; either may be NULL. */
typedef struct forge_metadata_bodies {
    cJSON *edit;
    cJSON *topics;
    uint32_t edit_members;
} forge_metadata_bodies;

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

/** naula_forge_fetch with a method and a JSON body (NULL for none), which a redirect never carries elsewhere. */
naula_status naula_forge_send(const forge_flavor *flavor, const char *method, const char *url, const char *token,
                              const cJSON *body, size_t capacity, forge_reply *reply, naula_refusal *refusal);

int naula_forge_is_writable_name(const char *text);
naula_status naula_forge_require_token(const forge_flavor *flavor, const char *token, naula_refusal *refusal);
int naula_forge_is_name_conflict(long status);
naula_status naula_forge_refuse_name_taken(const forge_flavor *flavor, const char *account, const char *name,
                                           naula_refusal *refusal);
naula_status naula_forge_refuse_not_permitted(const forge_flavor *flavor, naula_refusal *refusal);

/**
 * Parses a write's answer, the repository it changed or created, and refuses one whose number is not *number; a
 * *number of 0 takes the answered one. NULL with *status set when it fails.
 */
cJSON *naula_forge_parse_written(const forge_flavor *flavor, const forge_reply *reply, uint64_t *number,
                                 naula_status *status, naula_refusal *refusal);

/** The JSON object {name: text}, NULL when memory runs out. */
cJSON *naula_forge_text_body(const char *name, const char *text);

/** The JSON object {"archived": true}, NULL when memory runs out. */
cJSON *naula_forge_archive_body(void);

/** Refuses a rename answer that does not carry the new name, `rename_not_applied`. */
naula_status naula_forge_check_renamed(const forge_flavor *flavor, const cJSON *answer, const char *name,
                                       naula_refusal *refusal);

/** Refuses an archive answer that does not say archived, `archive_not_applied`. */
naula_status naula_forge_check_archived(const forge_flavor *flavor, const cJSON *answer, naula_refusal *refusal);

/** One request of a route on the host a provider's `where` names; method NULL is GET and body NULL sends none. */
typedef naula_status (*forge_send_function)(const void *where, const char *method, const char *route,
                                            const cJSON *body, forge_reply *reply, naula_refusal *refusal);

/**
 * The route that creates a repository under account: `orgs/<account>/repos` when the host answers the account as an
 * org, `user/repos` when it is the token's own user, and refused `account_not_writable` otherwise, so a repository is
 * never created under the token's user in place of the account named.
 */
/** One request of a route on the host a provider's `where` names; method NULL is GET and body NULL sends none. */
typedef naula_status (*forge_send_function)(const void *where, const char *method, const char *route,
                                            const cJSON *body, forge_reply *reply, naula_refusal *refusal);

/**
 * The route that creates a repository under account: `orgs/<account>/repos` when the host answers the account as an
 * org, `user/repos` when it is the token's own user, and refused `account_not_writable` otherwise, so a repository is
 * never created under the token's user in place of the account named.
 */
/** Whether metadata may be given to create: NULL, or a struct whose size covers its first shape. */
int naula_forge_is_usable_creation(const naula_metadata *metadata);

/** The JSON object {"name", "private"?} that creates a repository, NULL when memory runs out. */
cJSON *naula_forge_create_body(const char *name, const naula_metadata *metadata);

/**
 * Reads a creation's answer into *number, refusing `created_elsewhere` when the answer is not account/name, which a
 * host that creates under the token's user in place of the account would answer.
 */
naula_status naula_forge_read_created(const forge_flavor *flavor, const forge_reply *reply, const char *account,
                                      const char *name, uint64_t *number, naula_refusal *refusal);

naula_status naula_forge_create_route(const forge_flavor *flavor, forge_send_function send, const void *where,
                                      const char *account, char *route, size_t capacity, naula_refusal *refusal);

/**
 * The provider-neutral `create`: refuses an unusable account, name, metadata or token before any request, finds the
 * route, posts the name and visibility, and reads the answered number.
 */
naula_status naula_forge_create(const forge_flavor *flavor, forge_send_function send, const void *where,
                                const char *account, const char *name, const naula_metadata *metadata,
                                const char *token, uint64_t *number, naula_refusal *refusal);

/** The visibility metadata names in its settings, as `private`: -1 when it names none. */
int naula_forge_wanted_privacy(const naula_metadata *metadata);

int naula_forge_metadata_covers_first_shape(const naula_metadata *metadata);

/**
 * Builds the bodies that write the members of wanted->present a host names. A setting the host lacks is left out,
 * and `settings` counts as written only when at least one of its settings is sent.
 */
naula_status naula_forge_metadata_bodies(const forge_flavor *flavor, const forge_metadata_names *names,
                                         const naula_metadata *wanted, forge_metadata_bodies *bodies,
                                         naula_refusal *refusal);
void naula_forge_metadata_bodies_release(forge_metadata_bodies *bodies);

/**
 * Fills out from a repository answer and its topics list (NULL when the host answered none). A null description or
 * homepage reads as an empty one; a member the answer lacks is left out of `present`.
 */
naula_status naula_forge_read_metadata(const forge_flavor *flavor, const char *homepage_member,
                                       const cJSON *repository, const cJSON *topics, naula_metadata *out,
                                       naula_refusal *refusal);

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
