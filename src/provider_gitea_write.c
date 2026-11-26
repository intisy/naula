#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "forge_common.h"
#include "naula/memory.h"
#include "naula/provider.h"
#include "provider_gitea_internal.h"

/**
 * @implNote Gitea and Forgejo write only by owner and name, and no route writes by number, so the race below cannot
 * be closed, only kept short. The number is looked up immediately before every write, and the write goes to the names
 * that lookup answered. If in between this repository is renamed and another one is created under its old name, the
 * write lands on that other repository: its answer then carries another number, and the write is refused
 * `wrote_another_repository`, naming both numbers so the person can repair it. A plain rename in between answers 307
 * (Gitea redirects an old name), which the transport never follows and which is refused `repository_changed`. Each
 * route's first Gitea release is in docs/providers.md; all are older than the 1.20 the reads need.
 */
typedef struct opened {
    naula_repository repository;
    char prefix[FORGE_ROUTE_SIZE / 4];
} opened;

typedef struct write_answer {
    naula_status status;
    long refused_status;
    cJSON *json;
} write_answer;

typedef enum answer_kind { ANSWERS_NOTHING, ANSWERS_REPOSITORY } answer_kind;

static const forge_metadata_names metadata_names = {"website", "topics"};
static const missing_answer missing_repository = {"repository_changed", "repository",
                                                  "try again, the repository was renamed or moved during the read"};

static naula_status refuse_names(const gitea_target *at, naula_refusal *refusal) {
    return naula_forge_refuse_input(&at->edition->flavor, refusal, "the repository names");
}

static naula_status open_repository(const gitea_target *at, opened *repository, naula_refusal *refusal) {
    naula_status status = naula_gitea_lookup(at, &repository->repository, refusal);
    if (status == NAULA_OK) {
        status = naula_gitea_require_version(at, refusal);
    }
    if (status == NAULA_OK &&
        !naula_gitea_repository_prefix(&repository->repository, repository->prefix, sizeof repository->prefix)) {
        status = refuse_names(at, refusal);
    }
    return status;
}

static naula_status open_for_write(const gitea_target *at, const char *name, opened *repository,
                                   naula_refusal *refusal) {
    const forge_flavor *flavor = &at->edition->flavor;
    if (at->number == 0 || (name != NULL && !naula_forge_is_writable_name(name))) {
        return naula_forge_refuse_input(flavor, refusal, "the number or the name");
    }
    naula_status status = naula_forge_require_token(flavor, at->token, refusal);
    return status == NAULA_OK ? open_repository(at, repository, refusal) : status;
}

/** Looks the number up again, so the names a write goes to are as fresh as they can be. */
static naula_status look_up_again(const gitea_target *at, opened *repository, naula_refusal *refusal) {
    naula_repository_release(&repository->repository);
    naula_status status = naula_gitea_lookup(at, &repository->repository, refusal);
    if (status == NAULA_OK &&
        !naula_gitea_repository_prefix(&repository->repository, repository->prefix, sizeof repository->prefix)) {
        status = refuse_names(at, refusal);
    }
    return status;
}

/** A 404 on a write: gone is NOT_FOUND, renamed in between is repository_changed, and still there is not_permitted. */
static naula_status settle_missing(const gitea_target *at, const opened *repository, naula_refusal *refusal) {
    naula_status status = naula_gitea_confirm_names(at, &repository->repository, refusal);
    return status == NAULA_OK ? naula_forge_refuse_not_permitted(&at->edition->flavor, refusal) : status;
}

/**
 * A 307 on a write: Gitea redirects a name the repository was renamed from, so a number that now answers other names
 * is `repository_changed`; a redirect while the names still hold keeps its `redirected` refusal.
 */
static naula_status settle_redirect(const gitea_target *at, const opened *repository, naula_refusal *refusal) {
    naula_refusal checked;
    memset(&checked, 0, sizeof checked);
    checked.size = sizeof checked;
    naula_status status = naula_gitea_confirm_names(at, &repository->repository, &checked);
    if (status == NAULA_OK) {
        return NAULA_REFUSED;
    }
    if (status == NAULA_REFUSED && strcmp(checked.code, "repository_changed") == 0) {
        naula_refuse(refusal, "repository_changed", "run it again once the rename has settled",
                     "%s redirected the write: %s/%s was renamed after it was looked up", at->edition->flavor.label,
                     repository->repository.account, repository->repository.name);
        return NAULA_REFUSED;
    }
    if (refusal != NULL) {
        *refusal = checked;
    }
    return status;
}

static cJSON *read_written(const gitea_target *at, const opened *repository, const forge_reply *reply,
                           naula_status *status, naula_refusal *refusal) {
    uint64_t answered = 0;
    cJSON *json = naula_forge_parse_written(&at->edition->flavor, reply, &answered, status, refusal);
    if (json != NULL && answered != at->number) {
        naula_refuse(refusal, "wrote_another_repository",
                     "check the repository the detail names and repair what this write changed there, then run it again",
                     "%s wrote to %s/%s, which answered as repository %llu instead of %llu: another repository took "
                     "the name between the lookup and the write",
                     at->edition->flavor.label, repository->repository.account, repository->repository.name,
                     (unsigned long long)answered, (unsigned long long)at->number);
        *status = NAULA_REFUSED;
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

/** Looks the number up, then sends body, which it releases, to those names plus suffix. */
static write_answer write_to(const gitea_target *at, opened *repository, const char *method, const char *suffix,
                             cJSON *body, answer_kind kind, naula_refusal *refusal) {
    char route[FORGE_ROUTE_SIZE];
    forge_reply reply;
    write_answer answer = {NAULA_OUT_OF_MEMORY, 0, NULL};
    if (body == NULL) {
        return answer;
    }
    answer.status = look_up_again(at, repository, refusal);
    if (answer.status != NAULA_OK) {
        cJSON_Delete(body);
        return answer;
    }
    snprintf(route, sizeof route, "%s%s", repository->prefix, suffix);
    answer.status = naula_gitea_request(at, method, route, body, FORGE_SMALL_BODY_CAPACITY, &reply, refusal);
    cJSON_Delete(body);
    answer.refused_status = reply.status;
    if (answer.status == NAULA_NOT_FOUND) {
        answer.status = settle_missing(at, repository, refusal);
    } else if (answer.status == NAULA_REFUSED && answer.refused_status == 307) {
        answer.status = settle_redirect(at, repository, refusal);
    } else if (answer.status == NAULA_OK && kind == ANSWERS_REPOSITORY) {
        answer.json = read_written(at, repository, &reply, &answer.status, refusal);
    }
    naula_forge_reply_release(&reply);
    return answer;
}

naula_status naula_gitea_rename(void *context, const char *host, uint64_t number, const char *name, const char *token,
                                naula_refusal *refusal) {
    gitea_target at = {context, host, number, token};
    opened repository = {naula_gitea_fresh_repository(), ""};
    naula_status status = open_for_write(&at, name, &repository, refusal);
    if (status == NAULA_OK) {
        write_answer answer =
            write_to(&at, &repository, "PATCH", "", naula_forge_text_body("name", name), ANSWERS_REPOSITORY, refusal);
        status = answer.status;
        if (status == NAULA_REFUSED && naula_forge_is_name_refusal(answer.refused_status)) {
            status = naula_forge_refuse_name(&at.edition->flavor, answer.refused_status, repository.repository.account,
                                             name, refusal);
        } else if (answer.json != NULL) {
            status = naula_forge_check_renamed(&at.edition->flavor, answer.json, name, refusal);
        }
        cJSON_Delete(answer.json);
    }
    naula_repository_release(&repository.repository);
    return status;
}

naula_status naula_gitea_transfer(void *context, const char *host, uint64_t number, const char *account,
                                  const char *token, naula_refusal *refusal) {
    gitea_target at = {context, host, number, token};
    opened repository = {naula_gitea_fresh_repository(), ""};
    naula_status status = open_for_write(&at, account, &repository, refusal);
    if (status == NAULA_OK) {
        write_answer answer = write_to(&at, &repository, "POST", "/transfer",
                                       naula_forge_text_body("new_owner", account), ANSWERS_REPOSITORY, refusal);
        status = answer.status;
        if (status == NAULA_REFUSED && answer.refused_status == 422) {
            naula_refuse(refusal, "transfer_refused",
                         "check that the new owner exists, takes the repository, and holds none of that name",
                         "%s refused to transfer the repository to %s", at.edition->flavor.label, account);
        }
        cJSON_Delete(answer.json);
    }
    naula_repository_release(&repository.repository);
    return status;
}

naula_status naula_gitea_archive(void *context, const char *host, uint64_t number, const char *token,
                                 naula_refusal *refusal) {
    gitea_target at = {context, host, number, token};
    opened repository = {naula_gitea_fresh_repository(), ""};
    naula_status status = open_for_write(&at, NULL, &repository, refusal);
    if (status == NAULA_OK) {
        write_answer answer =
            write_to(&at, &repository, "PATCH", "", naula_forge_archive_body(), ANSWERS_REPOSITORY, refusal);
        status = answer.json != NULL ? naula_forge_check_archived(&at.edition->flavor, answer.json, refusal)
                                     : answer.status;
        cJSON_Delete(answer.json);
    }
    naula_repository_release(&repository.repository);
    return status;
}

static naula_status send_for_create(const void *where, const char *method, const char *route, const cJSON *body,
                                    forge_reply *reply, naula_refusal *refusal) {
    return naula_gitea_request(where, method, route, body, FORGE_SMALL_BODY_CAPACITY, reply, refusal);
}

naula_status naula_gitea_create(void *context, const char *host, const char *account, const char *name,
                                const naula_metadata *metadata, const char *token, uint64_t *number,
                                naula_refusal *refusal) {
    gitea_target at = {context, host, 0, token};
    int is_private = 1;
    *number = 0;
    naula_status status =
        naula_forge_check_creation(&at.edition->flavor, account, name, metadata, token, &is_private, refusal);
    if (status == NAULA_OK) {
        status = naula_gitea_require_version(&at, refusal);
    }
    if (status != NAULA_OK) {
        return status;
    }
    return naula_forge_create(&at.edition->flavor, send_for_create, &at, account, name, metadata, token, number,
                              refusal);
}

static naula_status read_json(const gitea_target *at, const char *route, cJSON **json, naula_refusal *refusal) {
    forge_reply reply;
    *json = NULL;
    naula_status status = naula_gitea_request(at, NULL, route, NULL, FORGE_SMALL_BODY_CAPACITY, &reply, refusal);
    if (status == NAULA_OK) {
        *json = naula_forge_parse_reply(&at->edition->flavor, &reply, &status, refusal);
    }
    naula_forge_reply_release(&reply);
    return status;
}

/** The repository's answer by name, checked to carry the number, and its topics list. */
static naula_status read_by_name(const gitea_target *at, const opened *repository, cJSON **json, cJSON **topics,
                                 naula_refusal *refusal) {
    char route[FORGE_ROUTE_SIZE];
    uint64_t answered = 0;
    naula_status status = read_json(at, repository->prefix, json, refusal);
    if (status == NAULA_OK && (!naula_forge_read_exact_number(*json, "id", &answered) || answered != at->number)) {
        naula_refuse(refusal, "repository_changed", "try again, the repository was renamed or moved during the read",
                     "%s answered another repository by its name", at->edition->flavor.label);
        status = NAULA_REFUSED;
    }
    snprintf(route, sizeof route, "%s/topics", repository->prefix);
    return status == NAULA_OK ? read_json(at, route, topics, refusal) : status;
}

naula_status naula_gitea_read_metadata(void *context, const char *host, uint64_t number, const char *token,
                                       naula_metadata *out, naula_refusal *refusal) {
    gitea_target at = {context, host, number, token};
    opened repository = {naula_gitea_fresh_repository(), ""};
    cJSON *json = NULL;
    cJSON *topics = NULL;
    if (number == 0 || out == NULL || !naula_forge_metadata_covers_first_shape(out)) {
        return naula_forge_refuse_input(&at.edition->flavor, refusal, "the number or the metadata struct");
    }
    naula_status status = open_repository(&at, &repository, refusal);
    if (status == NAULA_OK) {
        status = read_by_name(&at, &repository, &json, &topics, refusal);
        status = naula_gitea_settle_read(&at, &repository.repository, status, &missing_repository, refusal);
    }
    if (status == NAULA_OK) {
        status = naula_forge_read_metadata(&at.edition->flavor, "website", json,
                                           cJSON_GetObjectItemCaseSensitive(topics, "topics"), out, refusal);
    }
    cJSON_Delete(topics);
    cJSON_Delete(json);
    naula_repository_release(&repository.repository);
    return status;
}

/**
 * @implNote The topics are reported applied on the host's 204, which carries no list to compare; the PATCH answer is
 * compared member by member.
 */
static naula_status send_bodies(const gitea_target *at, opened *repository, const naula_metadata *wanted,
                                forge_metadata_bodies *bodies, uint32_t *applied, naula_refusal *refusal) {
    naula_status status = NAULA_OK;
    if (bodies->edit != NULL) {
        write_answer answer = write_to(at, repository, "PATCH", "", bodies->edit, ANSWERS_REPOSITORY, refusal);
        bodies->edit = NULL;
        status = answer.status;
        if (answer.json != NULL) {
            *applied |= naula_forge_reflected_members(&metadata_names, wanted, bodies->edit_members, answer.json);
        }
        cJSON_Delete(answer.json);
    }
    if (status == NAULA_OK && bodies->topics != NULL) {
        write_answer answer = write_to(at, repository, "PUT", "/topics", bodies->topics, ANSWERS_NOTHING, refusal);
        bodies->topics = NULL;
        status = answer.status;
        *applied |= status == NAULA_OK ? (uint32_t)NAULA_METADATA_TOPICS : 0u;
    }
    return status;
}

naula_status naula_gitea_write_metadata(void *context, const char *host, uint64_t number,
                                        const naula_metadata *wanted, const char *token, uint32_t *applied,
                                        naula_refusal *refusal) {
    gitea_target at = {context, host, number, token};
    opened repository = {naula_gitea_fresh_repository(), ""};
    forge_metadata_bodies bodies;
    *applied = 0;
    naula_status status = naula_forge_metadata_bodies(&at.edition->flavor, &metadata_names, wanted, &bodies, refusal);
    if (status == NAULA_OK) {
        status = open_for_write(&at, NULL, &repository, refusal);
    }
    if (status == NAULA_OK) {
        status = send_bodies(&at, &repository, wanted, &bodies, applied, refusal);
    }
    naula_forge_metadata_bodies_release(&bodies);
    naula_repository_release(&repository.repository);
    return status;
}
