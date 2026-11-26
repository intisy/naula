#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "forge_common.h"
#include "naula/memory.h"
#include "naula/provider.h"
#include "provider_github_internal.h"

/**
 * @implNote Every write but create goes to `repositories/<number>`, the route GitHub itself redirects a renamed
 * repository to, so a write reaches the repository the number names and never one that took its old name. Writing
 * through that route is unmeasured (nothing is written to a real host before the live proof, plan task 20); reading
 * through it is measured.
 */
typedef struct number_write {
    const char *host;
    uint64_t number;
    const char *token;
} number_write;

typedef struct write_answer {
    naula_status status;
    long refused_status;
    cJSON *json;
} write_answer;

/** What a write's answer is read as: the repository it changed, or the JSON it answers. */
typedef enum answer_kind { ANSWERS_REPOSITORY, ANSWERS_DOCUMENT } answer_kind;

static const forge_metadata_names metadata_names = {"homepage", "names"};

static naula_status check_input(const number_write *at, const char *name, naula_refusal *refusal) {
    if (at->number == 0 || (name != NULL && !naula_forge_is_writable_name(name))) {
        return naula_forge_refuse_input(naula_github_flavor(), refusal, "the number or the name");
    }
    return naula_forge_require_token(naula_github_flavor(), at->token, refusal);
}

/** A 404 on a write reads the same as a repository that is gone, so the repository decides which. */
static naula_status settle_missing(const number_write *at, naula_refusal *refusal) {
    naula_status status = naula_github_still_the_same_number(at->host, at->number, at->token, refusal);
    return status == NAULA_OK ? naula_forge_refuse_not_permitted(naula_github_flavor(), refusal) : status;
}

/** Sends body, which it releases, to the repository's route plus suffix; a repository answer is checked by number. */
static write_answer write_to_number(const number_write *at, const char *method, const char *suffix, cJSON *body,
                                    answer_kind kind, naula_refusal *refusal) {
    char route[FORGE_ROUTE_SIZE];
    forge_reply reply;
    write_answer answer = {NAULA_OUT_OF_MEMORY, 0, NULL};
    if (body == NULL) {
        return answer;
    }
    snprintf(route, sizeof route, "repositories/%llu%s", (unsigned long long)at->number, suffix);
    answer.status = naula_github_request(at->host, method, route, at->token, body, FORGE_SMALL_BODY_CAPACITY, &reply,
                                         refusal);
    cJSON_Delete(body);
    answer.refused_status = reply.status;
    if (answer.status == NAULA_NOT_FOUND) {
        answer.status = settle_missing(at, refusal);
    } else if (answer.status == NAULA_OK && kind == ANSWERS_REPOSITORY) {
        uint64_t number = at->number;
        answer.json = naula_forge_parse_written(naula_github_flavor(), &reply, &number, &answer.status, refusal);
    } else if (answer.status == NAULA_OK && kind == ANSWERS_DOCUMENT) {
        answer.json = naula_forge_parse_reply(naula_github_flavor(), &reply, &answer.status, refusal);
    }
    naula_forge_reply_release(&reply);
    return answer;
}

naula_status naula_github_rename(void *context, const char *host, uint64_t number, const char *name, const char *token,
                                 naula_refusal *refusal) {
    number_write at = {host, number, token};
    (void)context;
    naula_status status = check_input(&at, name, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    write_answer answer =
        write_to_number(&at, "PATCH", "", naula_forge_text_body("name", name), ANSWERS_REPOSITORY, refusal);
    if (answer.status == NAULA_REFUSED && naula_forge_is_name_refusal(answer.refused_status)) {
        return naula_forge_refuse_name(naula_github_flavor(), answer.refused_status, NULL, name, refusal);
    }
    if (answer.json != NULL) {
        answer.status = naula_forge_check_renamed(naula_github_flavor(), answer.json, name, refusal);
    }
    cJSON_Delete(answer.json);
    return answer.status;
}

naula_status naula_github_transfer(void *context, const char *host, uint64_t number, const char *account,
                                   const char *token, naula_refusal *refusal) {
    number_write at = {host, number, token};
    (void)context;
    naula_status status = check_input(&at, account, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    write_answer answer = write_to_number(&at, "POST", "/transfer", naula_forge_text_body("new_owner", account),
                                          ANSWERS_REPOSITORY, refusal);
    if (answer.status == NAULA_REFUSED && answer.refused_status == 422) {
        naula_refuse(refusal, "transfer_refused",
                     "check that the new owner exists, takes the repository, and holds none of that name",
                     "GitHub refused to transfer the repository to %s", account);
    }
    cJSON_Delete(answer.json);
    return answer.status;
}

naula_status naula_github_archive(void *context, const char *host, uint64_t number, const char *token,
                                  naula_refusal *refusal) {
    number_write at = {host, number, token};
    (void)context;
    naula_status status = check_input(&at, NULL, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    write_answer answer = write_to_number(&at, "PATCH", "", naula_forge_archive_body(), ANSWERS_REPOSITORY, refusal);
    if (answer.json != NULL) {
        answer.status = naula_forge_check_archived(naula_github_flavor(), answer.json, refusal);
    }
    cJSON_Delete(answer.json);
    return answer.status;
}

typedef struct host_and_token {
    const char *host;
    const char *token;
} host_and_token;

static naula_status send_for_create(const void *where, const char *method, const char *route, const cJSON *body,
                                    forge_reply *reply, naula_refusal *refusal) {
    const host_and_token *at = where;
    return naula_github_request(at->host, method, route, at->token, body, FORGE_SMALL_BODY_CAPACITY, reply, refusal);
}

naula_status naula_github_create(void *context, const char *host, const char *account, const char *name,
                                 const naula_metadata *metadata, const char *token, uint64_t *number,
                                 naula_refusal *refusal) {
    host_and_token at = {host, token};
    (void)context;
    return naula_forge_create(naula_github_flavor(), send_for_create, &at, account, name, metadata, token, number,
                              refusal);
}

naula_status naula_github_read_metadata(void *context, const char *host, uint64_t number, const char *token,
                                        naula_metadata *out, naula_refusal *refusal) {
    char route[FORGE_ROUTE_SIZE];
    forge_reply reply;
    (void)context;
    if (number == 0 || out == NULL || !naula_forge_metadata_covers_first_shape(out)) {
        return naula_forge_refuse_input(naula_github_flavor(), refusal, "the number or the metadata struct");
    }
    snprintf(route, sizeof route, "repositories/%llu", (unsigned long long)number);
    naula_status status =
        naula_github_request(host, NULL, route, token, NULL, FORGE_SMALL_BODY_CAPACITY, &reply, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    uint64_t answered = number;
    cJSON *json = naula_forge_parse_written(naula_github_flavor(), &reply, &answered, &status, refusal);
    if (json != NULL) {
        status = naula_forge_read_metadata(naula_github_flavor(), "homepage", json,
                                           cJSON_GetObjectItemCaseSensitive(json, "topics"), out, refusal);
    }
    cJSON_Delete(json);
    naula_forge_reply_release(&reply);
    return status;
}

static naula_status send_bodies(const number_write *at, const naula_metadata *wanted, forge_metadata_bodies *bodies,
                                uint32_t *applied, naula_refusal *refusal) {
    naula_status status = NAULA_OK;
    if (bodies->edit != NULL) {
        write_answer answer = write_to_number(at, "PATCH", "", bodies->edit, ANSWERS_REPOSITORY, refusal);
        bodies->edit = NULL;
        status = answer.status;
        if (answer.json != NULL) {
            *applied |= naula_forge_reflected_members(&metadata_names, wanted, bodies->edit_members, answer.json);
        }
        cJSON_Delete(answer.json);
    }
    if (status == NAULA_OK && bodies->topics != NULL) {
        write_answer answer = write_to_number(at, "PUT", "/topics", bodies->topics, ANSWERS_DOCUMENT, refusal);
        bodies->topics = NULL;
        status = answer.status;
        const cJSON *names = answer.json == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(answer.json, "names");
        *applied |= names != NULL && naula_forge_topics_reflected(names, wanted) ? (uint32_t)NAULA_METADATA_TOPICS : 0u;
        cJSON_Delete(answer.json);
    }
    return status;
}

naula_status naula_github_write_metadata(void *context, const char *host, uint64_t number,
                                         const naula_metadata *wanted, const char *token, uint32_t *applied,
                                         naula_refusal *refusal) {
    number_write at = {host, number, token};
    forge_metadata_bodies bodies;
    (void)context;
    *applied = 0;
    naula_status status = check_input(&at, NULL, refusal);
    if (status == NAULA_OK) {
        status = naula_forge_metadata_bodies(naula_github_flavor(), &metadata_names, wanted, &bodies, refusal);
    }
    if (status != NAULA_OK) {
        return status;
    }
    status = send_bodies(&at, wanted, &bodies, applied, refusal);
    naula_forge_metadata_bodies_release(&bodies);
    return status;
}
