#include "forge_common.h"

#include <stdio.h>
#include <string.h>

#include "naula/metadata.h"

static naula_status refuse_account(const forge_flavor *flavor, const char *account, naula_refusal *refusal) {
    naula_refuse(refusal, "account_not_writable",
                 "give a token of that account, or name an org the token may create repositories in",
                 "%s holds no org %s, and the token belongs to another user", flavor->label, account);
    return NAULA_REFUSED;
}

static naula_status user_route(const forge_flavor *flavor, forge_send_function send, const void *where,
                               const char *account, char *route, size_t capacity, naula_refusal *refusal) {
    forge_reply reply;
    naula_status status = send(where, NULL, "user", NULL, &reply, refusal);
    if (status == NAULA_NOT_FOUND) {
        return refuse_account(flavor, account, refusal);
    }
    if (status != NAULA_OK) {
        return status;
    }
    cJSON *json = naula_forge_parse_reply(flavor, &reply, &status, refusal);
    const char *login = json == NULL ? NULL : naula_forge_text_member(json, "login");
    if (json != NULL && login == NULL) {
        status = naula_forge_refuse_malformed(flavor, refusal, "a user with no login");
    } else if (login != NULL && !naula_forge_equals_ignoring_case(login, account)) {
        status = refuse_account(flavor, account, refusal);
    } else if (login != NULL) {
        snprintf(route, capacity, "user/repos");
    }
    cJSON_Delete(json);
    naula_forge_reply_release(&reply);
    return status;
}

naula_status naula_forge_create_route(const forge_flavor *flavor, forge_send_function send, const void *where,
                                      const char *account, char *route, size_t capacity, naula_refusal *refusal) {
    char probe[FORGE_NAME_LENGTH_LIMIT + 8];
    forge_reply reply;
    naula_refusal absent;
    memset(&absent, 0, sizeof absent);
    absent.size = sizeof absent;
    snprintf(probe, sizeof probe, "orgs/%s", account);
    naula_status status = send(where, NULL, probe, NULL, &reply, &absent);
    naula_forge_reply_release(&reply);
    if (status == NAULA_OK) {
        snprintf(route, capacity, "orgs/%s/repos", account);
        return NAULA_OK;
    }
    if (status == NAULA_NOT_FOUND) {
        return user_route(flavor, send, where, account, route, capacity, refusal);
    }
    if (refusal != NULL) {
        *refusal = absent;
    }
    return status;
}

/** The visibility to create with: private unless the settings say public; -1 for one that is neither. */
static int creation_privacy(const naula_metadata *metadata) {
    if (metadata == NULL || (metadata->present & NAULA_METADATA_SETTINGS) == 0) {
        return 1;
    }
    if (!cJSON_IsObject(metadata->settings)) {
        return -1;
    }
    const cJSON *visibility = cJSON_GetObjectItemCaseSensitive(metadata->settings, "visibility");
    if (visibility == NULL) {
        return 1;
    }
    if (cJSON_IsString(visibility) && strcmp(visibility->valuestring, "public") == 0) {
        return 0;
    }
    return cJSON_IsString(visibility) && strcmp(visibility->valuestring, "private") == 0 ? 1 : -1;
}

naula_status naula_forge_check_creation(const forge_flavor *flavor, const char *account, const char *name,
                                        const naula_metadata *metadata, const char *token, int *is_private,
                                        naula_refusal *refusal) {
    int usable_metadata = metadata == NULL || naula_forge_metadata_covers_first_shape(metadata);
    *is_private = usable_metadata ? creation_privacy(metadata) : -1;
    if (!naula_forge_is_writable_name(account) || !naula_forge_is_writable_name(name) || *is_private < 0) {
        return naula_forge_refuse_input(flavor, refusal, "the account, the name or the metadata's visibility");
    }
    return naula_forge_require_token(flavor, token, refusal);
}

static cJSON *create_body(const char *name, int is_private) {
    cJSON *body = naula_forge_text_body("name", name);
    if (body != NULL && cJSON_AddBoolToObject(body, "private", is_private) == NULL) {
        cJSON_Delete(body);
        return NULL;
    }
    return body;
}

static naula_status read_created(const forge_flavor *flavor, const forge_reply *reply, const char *account,
                                 const char *name, uint64_t *number, naula_refusal *refusal) {
    naula_status status = NAULA_OK;
    uint64_t answered = 0;
    cJSON *json = naula_forge_parse_written(flavor, reply, &answered, &status, refusal);
    if (json != NULL && !naula_forge_answers_to_the_name(json, account, name)) {
        naula_refuse(refusal, "created_elsewhere", "check the account and the name, then look the repository up",
                     "%s created the repository under another name than %s/%s", flavor->label, account, name);
        status = NAULA_REFUSED;
    } else if (json != NULL) {
        *number = answered;
    }
    cJSON_Delete(json);
    return status;
}

naula_status naula_forge_create(const forge_flavor *flavor, forge_send_function send, const void *where,
                                const char *account, const char *name, const naula_metadata *metadata,
                                const char *token, uint64_t *number, naula_refusal *refusal) {
    char route[FORGE_NAME_LENGTH_LIMIT + 16];
    forge_reply reply;
    int is_private = 1;
    *number = 0;
    naula_status status = naula_forge_check_creation(flavor, account, name, metadata, token, &is_private, refusal);
    if (status == NAULA_OK) {
        status = naula_forge_create_route(flavor, send, where, account, route, sizeof route, refusal);
    }
    cJSON *body = status == NAULA_OK ? create_body(name, is_private) : NULL;
    if (status != NAULA_OK || body == NULL) {
        return status != NAULA_OK ? status : NAULA_OUT_OF_MEMORY;
    }
    status = send(where, "POST", route, body, &reply, refusal);
    cJSON_Delete(body);
    if (status == NAULA_REFUSED && naula_forge_is_name_refusal(reply.status)) {
        return naula_forge_refuse_name(flavor, reply.status, account, name, refusal);
    }
    if (status == NAULA_NOT_FOUND) {
        return naula_forge_refuse_not_permitted(flavor, refusal);
    }
    if (status == NAULA_OK) {
        status = read_created(flavor, &reply, account, name, number, refusal);
    }
    naula_forge_reply_release(&reply);
    return status;
}
