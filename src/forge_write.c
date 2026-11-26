#include "forge_common.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "naula/memory.h"
#include "naula/metadata.h"

#define NAME_LENGTH_LIMIT 100u

int naula_forge_is_writable_name(const char *text) {
    return text != NULL && text[0] != '-' && strlen(text) <= NAME_LENGTH_LIMIT && naula_forge_is_name_segment(text);
}

naula_status naula_forge_require_token(const forge_flavor *flavor, const char *token, naula_refusal *refusal) {
    if (token != NULL) {
        return NAULA_OK;
    }
    naula_refuse(refusal, "token_needed", "give a token that may write to the repository", "%s writes only with a token",
                 flavor->label);
    return NAULA_REFUSED;
}

int naula_forge_is_name_conflict(long status) {
    return status == 409 || status == 422;
}

naula_status naula_forge_refuse_name_taken(const forge_flavor *flavor, const char *account, const char *name,
                                           naula_refusal *refusal) {
    naula_refuse(refusal, "name_taken", "pick a name the account does not hold yet, or rename the repository holding it",
                 "%s refused the name %s%s%s as taken (or as a name it reserves)", flavor->label,
                 account == NULL ? "" : account, account == NULL ? "" : "/", name);
    return NAULA_REFUSED;
}

naula_status naula_forge_refuse_not_permitted(const forge_flavor *flavor, naula_refusal *refusal) {
    naula_refuse(refusal, "not_permitted", "give a token with admin rights on the repository",
                 "%s refused the write to a repository that exists", flavor->label);
    return NAULA_REFUSED;
}

cJSON *naula_forge_parse_written(const forge_flavor *flavor, const forge_reply *reply, uint64_t *number,
                                 naula_status *status, naula_refusal *refusal) {
    uint64_t answered = 0;
    cJSON *json = naula_forge_parse_reply(flavor, reply, status, refusal);
    if (json == NULL) {
        return NULL;
    }
    if (!cJSON_IsObject(json) || !naula_forge_read_exact_number(json, "id", &answered)) {
        *status = naula_forge_refuse_malformed(flavor, refusal, "a written repository with no number");
    } else if (*number != 0 && answered != *number) {
        naula_refuse(refusal, "number_mismatch", "check the number in the id",
                     "%s answered the write with another repository than the one asked for", flavor->label);
        *status = NAULA_REFUSED;
    } else {
        *number = answered;
        return json;
    }
    cJSON_Delete(json);
    return NULL;
}

cJSON *naula_forge_text_body(const char *name, const char *text) {
    cJSON *body = cJSON_CreateObject();
    if (body != NULL && cJSON_AddStringToObject(body, name, text) == NULL) {
        cJSON_Delete(body);
        return NULL;
    }
    return body;
}

int naula_forge_wanted_privacy(const naula_metadata *metadata) {
    if (metadata == NULL || (metadata->present & NAULA_METADATA_SETTINGS) == 0) {
        return -1;
    }
    const char *visibility = naula_forge_text_member(metadata->settings, "visibility");
    if (visibility == NULL) {
        return -1;
    }
    return strcmp(visibility, "private") == 0 ? 1 : (strcmp(visibility, "public") == 0 ? 0 : -1);
}

int naula_forge_metadata_covers_first_shape(const naula_metadata *metadata) {
    return metadata->size >= offsetof(naula_metadata, present) + sizeof metadata->present;
}

typedef enum setting_outcome { SETTING_SENT, SETTING_LACKED, SETTING_UNUSABLE, SETTING_NO_MEMORY } setting_outcome;

static setting_outcome added(const cJSON *item) {
    return item != NULL ? SETTING_SENT : SETTING_NO_MEMORY;
}

/** Maps one setting of the file onto the edit body: `visibility` is `private`, the rest are renamed. */
static setting_outcome add_setting(cJSON *edit, const cJSON *setting) {
    if (strcmp(setting->string, "visibility") == 0) {
        int is_private = cJSON_IsString(setting) && strcmp(setting->valuestring, "private") == 0;
        int is_public = cJSON_IsString(setting) && strcmp(setting->valuestring, "public") == 0;
        return is_private || is_public ? added(cJSON_AddBoolToObject(edit, "private", is_private)) : SETTING_UNUSABLE;
    }
    if (strcmp(setting->string, "defaultBranch") == 0) {
        int usable = cJSON_IsString(setting) && setting->valuestring[0] != '\0';
        return usable ? added(cJSON_AddStringToObject(edit, "default_branch", setting->valuestring)) : SETTING_UNUSABLE;
    }
    const char *host_name = strcmp(setting->string, "issues") == 0 ? "has_issues"
                            : strcmp(setting->string, "wiki") == 0 ? "has_wiki"
                                                                   : NULL;
    if (host_name == NULL) {
        return SETTING_LACKED;
    }
    return cJSON_IsBool(setting) ? added(cJSON_AddBoolToObject(edit, host_name, cJSON_IsTrue(setting)))
                                 : SETTING_UNUSABLE;
}

static naula_status add_settings(const forge_flavor *flavor, cJSON *edit, const cJSON *settings, int *sent,
                                 naula_refusal *refusal) {
    if (!cJSON_IsObject(settings)) {
        return naula_forge_refuse_input(flavor, refusal, "the settings");
    }
    for (const cJSON *setting = settings->child; setting != NULL; setting = setting->next) {
        setting_outcome outcome = add_setting(edit, setting);
        if (outcome == SETTING_UNUSABLE) {
            return naula_forge_refuse_input(flavor, refusal, "a setting");
        }
        if (outcome == SETTING_NO_MEMORY) {
            return NAULA_OUT_OF_MEMORY;
        }
        *sent = *sent || outcome == SETTING_SENT;
    }
    return NAULA_OK;
}

static naula_status add_text(cJSON *edit, const char *name, const char *text, uint32_t bit, uint32_t *members) {
    if (cJSON_AddStringToObject(edit, name, text == NULL ? "" : text) == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    *members |= bit;
    return NAULA_OK;
}

static naula_status build_edit(const forge_flavor *flavor, const forge_metadata_names *names,
                               const naula_metadata *wanted, forge_metadata_bodies *bodies, naula_refusal *refusal) {
    uint32_t present = wanted->present;
    if ((present & (NAULA_METADATA_DESCRIPTION | NAULA_METADATA_HOMEPAGE | NAULA_METADATA_SETTINGS)) == 0) {
        return NAULA_OK;
    }
    bodies->edit = cJSON_CreateObject();
    if (bodies->edit == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    naula_status status = NAULA_OK;
    if (present & NAULA_METADATA_DESCRIPTION) {
        status = add_text(bodies->edit, "description", wanted->description, NAULA_METADATA_DESCRIPTION,
                          &bodies->edit_members);
    }
    if (status == NAULA_OK && (present & NAULA_METADATA_HOMEPAGE)) {
        status = add_text(bodies->edit, names->homepage, wanted->homepage, NAULA_METADATA_HOMEPAGE,
                          &bodies->edit_members);
    }
    int sent = 0;
    if (status == NAULA_OK && (present & NAULA_METADATA_SETTINGS)) {
        status = add_settings(flavor, bodies->edit, wanted->settings, &sent, refusal);
    }
    bodies->edit_members |= sent ? NAULA_METADATA_SETTINGS : 0u;
    return status;
}

static naula_status build_topics(const forge_flavor *flavor, const forge_metadata_names *names,
                                 const naula_metadata *wanted, forge_metadata_bodies *bodies, naula_refusal *refusal) {
    if ((wanted->present & NAULA_METADATA_TOPICS) == 0 || names->topics == NULL) {
        return NAULA_OK;
    }
    if (wanted->topic_count > 0 && wanted->topics == NULL) {
        return naula_forge_refuse_input(flavor, refusal, "the topics");
    }
    bodies->topics = cJSON_CreateObject();
    cJSON *list = bodies->topics == NULL ? NULL : cJSON_AddArrayToObject(bodies->topics, names->topics);
    if (list == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < wanted->topic_count; index++) {
        if (wanted->topics[index] == NULL) {
            return naula_forge_refuse_input(flavor, refusal, "a topic");
        }
        cJSON *topic = cJSON_CreateString(wanted->topics[index]);
        if (topic == NULL || !cJSON_AddItemToArray(list, topic)) {
            cJSON_Delete(topic);
            return NAULA_OUT_OF_MEMORY;
        }
    }
    return NAULA_OK;
}

void naula_forge_metadata_bodies_release(forge_metadata_bodies *bodies) {
    cJSON_Delete(bodies->edit);
    cJSON_Delete(bodies->topics);
    memset(bodies, 0, sizeof *bodies);
}

naula_status naula_forge_metadata_bodies(const forge_flavor *flavor, const forge_metadata_names *names,
                                         const naula_metadata *wanted, forge_metadata_bodies *bodies,
                                         naula_refusal *refusal) {
    memset(bodies, 0, sizeof *bodies);
    if (wanted == NULL || !naula_forge_metadata_covers_first_shape(wanted)) {
        return naula_forge_refuse_input(flavor, refusal, "the metadata struct");
    }
    naula_status status = build_edit(flavor, names, wanted, bodies, refusal);
    if (status == NAULA_OK) {
        status = build_topics(flavor, names, wanted, bodies, refusal);
    }
    if (status == NAULA_OK && bodies->edit != NULL && bodies->edit_members == 0) {
        cJSON_Delete(bodies->edit);
        bodies->edit = NULL;
    }
    if (status != NAULA_OK) {
        naula_forge_metadata_bodies_release(bodies);
    }
    return status;
}

static naula_status read_text(const forge_flavor *flavor, const cJSON *repository, const char *member, char **out,
                              uint32_t bit, uint32_t *present, naula_refusal *refusal) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(repository, member);
    if (item == NULL) {
        return NAULA_OK;
    }
    if (!cJSON_IsString(item) && !cJSON_IsNull(item)) {
        return naula_forge_refuse_malformed(flavor, refusal, "a description or homepage that is not text");
    }
    *out = naula_memory_duplicate_text(cJSON_IsString(item) ? item->valuestring : "");
    if (*out == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    *present |= bit;
    return NAULA_OK;
}

static naula_status read_topics(const forge_flavor *flavor, const cJSON *topics, naula_metadata *out,
                                naula_refusal *refusal) {
    if (topics == NULL) {
        return NAULA_OK;
    }
    if (!cJSON_IsArray(topics)) {
        return naula_forge_refuse_malformed(flavor, refusal, "topics that are not a list");
    }
    size_t count = (size_t)cJSON_GetArraySize(topics);
    out->topics = count == 0 ? NULL : naula_memory_allocate(count * sizeof *out->topics);
    if (count > 0 && out->topics == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    for (const cJSON *topic = topics->child; topic != NULL; topic = topic->next) {
        if (!cJSON_IsString(topic)) {
            return naula_forge_refuse_malformed(flavor, refusal, "a topic that is not text");
        }
        out->topics[out->topic_count] = naula_memory_duplicate_text(topic->valuestring);
        if (out->topics[out->topic_count] == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        out->topic_count++;
    }
    out->present |= NAULA_METADATA_TOPICS;
    return NAULA_OK;
}

static int add_boolean_setting(cJSON *settings, const cJSON *repository, const char *host_name, const char *name,
                               int *read) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(repository, host_name);
    if (!cJSON_IsBool(item)) {
        return 1;
    }
    *read = 1;
    return cJSON_AddBoolToObject(settings, name, cJSON_IsTrue(item)) != NULL;
}

static naula_status read_settings(const cJSON *repository, naula_metadata *out) {
    cJSON *settings = cJSON_CreateObject();
    if (settings == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    int read = 0;
    int built = 1;
    const cJSON *is_private = cJSON_GetObjectItemCaseSensitive(repository, "private");
    if (cJSON_IsBool(is_private)) {
        read = 1;
        built = cJSON_AddStringToObject(settings, "visibility", cJSON_IsTrue(is_private) ? "private" : "public") != NULL;
    }
    const char *branch = naula_forge_text_member(repository, "default_branch");
    if (built && branch != NULL) {
        read = 1;
        built = cJSON_AddStringToObject(settings, "defaultBranch", branch) != NULL;
    }
    built = built && add_boolean_setting(settings, repository, "has_issues", "issues", &read) &&
            add_boolean_setting(settings, repository, "has_wiki", "wiki", &read);
    if (!built || !read) {
        cJSON_Delete(settings);
        return built ? NAULA_OK : NAULA_OUT_OF_MEMORY;
    }
    out->settings = settings;
    out->present |= NAULA_METADATA_SETTINGS;
    return NAULA_OK;
}

naula_status naula_forge_read_metadata(const forge_flavor *flavor, const char *homepage_member,
                                       const cJSON *repository, const cJSON *topics, naula_metadata *out,
                                       naula_refusal *refusal) {
    if (!naula_forge_metadata_covers_first_shape(out)) {
        return naula_forge_refuse_input(flavor, refusal, "the metadata struct");
    }
    uint32_t size = out->size;
    memset(out, 0, sizeof *out);
    out->size = size;
    naula_status status = read_text(flavor, repository, "description", &out->description, NAULA_METADATA_DESCRIPTION,
                                    &out->present, refusal);
    if (status == NAULA_OK) {
        status = read_text(flavor, repository, homepage_member, &out->homepage, NAULA_METADATA_HOMEPAGE,
                           &out->present, refusal);
    }
    if (status == NAULA_OK) {
        status = read_topics(flavor, topics, out, refusal);
    }
    if (status == NAULA_OK) {
        status = read_settings(repository, out);
    }
    if (status != NAULA_OK) {
        naula_metadata_release(out);
    }
    return status;
}

cJSON *naula_forge_archive_body(void) {
    cJSON *body = cJSON_CreateObject();
    if (body != NULL && cJSON_AddTrueToObject(body, "archived") == NULL) {
        cJSON_Delete(body);
        return NULL;
    }
    return body;
}

naula_status naula_forge_check_renamed(const forge_flavor *flavor, const cJSON *answer, const char *name,
                                       naula_refusal *refusal) {
    const char *answered = naula_forge_text_member(answer, "name");
    if (answered != NULL && naula_forge_equals_ignoring_case(answered, name)) {
        return NAULA_OK;
    }
    naula_refuse(refusal, "rename_not_applied", "read the repository by number and try the rename again",
                 "%s answered the rename with the name %s", flavor->label, answered == NULL ? "(none)" : answered);
    return NAULA_REFUSED;
}

naula_status naula_forge_check_archived(const forge_flavor *flavor, const cJSON *answer, naula_refusal *refusal) {
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer, "archived"))) {
        return NAULA_OK;
    }
    naula_refuse(refusal, "archive_not_applied", "read the repository by number and try archiving it again",
                 "%s answered the archive with a repository that is not archived", flavor->label);
    return NAULA_REFUSED;
}

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
    char probe[NAME_LENGTH_LIMIT + 8];
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

static int is_usable_creation(const naula_metadata *metadata) {
    return metadata == NULL || naula_forge_metadata_covers_first_shape(metadata);
}

static cJSON *create_body(const char *name, const naula_metadata *metadata) {
    cJSON *body = naula_forge_text_body("name", name);
    int is_private = naula_forge_wanted_privacy(metadata);
    if (body != NULL && is_private >= 0 && cJSON_AddBoolToObject(body, "private", is_private) == NULL) {
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
    char route[NAME_LENGTH_LIMIT + 16];
    forge_reply reply;
    *number = 0;
    if (!naula_forge_is_writable_name(account) || !naula_forge_is_writable_name(name) ||
        !is_usable_creation(metadata)) {
        return naula_forge_refuse_input(flavor, refusal, "the account, the name or the metadata");
    }
    naula_status status = naula_forge_require_token(flavor, token, refusal);
    if (status == NAULA_OK) {
        status = naula_forge_create_route(flavor, send, where, account, route, sizeof route, refusal);
    }
    cJSON *body = status == NAULA_OK ? create_body(name, metadata) : NULL;
    if (status != NAULA_OK || body == NULL) {
        return status != NAULA_OK ? status : NAULA_OUT_OF_MEMORY;
    }
    status = send(where, "POST", route, body, &reply, refusal);
    cJSON_Delete(body);
    if (status == NAULA_REFUSED && naula_forge_is_name_conflict(reply.status)) {
        return naula_forge_refuse_name_taken(flavor, account, name, refusal);
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
