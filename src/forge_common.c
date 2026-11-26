#include "forge_common.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "naula/json.h"
#include "naula/memory.h"
#include "naula/transport.h"

#define URL_SIZE 2048
#define DEADLINE_MILLISECONDS 30000u
#define TOKEN_LIMIT 512u
#define AUTHORIZATION_SIZE (TOKEN_LIMIT + 16u)
#define LARGEST_EXACT_INTEGER 9007199254740991.0
#define PAGE_CAPACITY 100u
#define COMMIT_HEX_MINIMUM 7u
#define COMMIT_HEX_MAXIMUM 64u
#define JSON_CONTENT_TYPE "application/json"

void naula_forge_reply_release(forge_reply *reply) {
    naula_memory_release(reply->body);
    naula_memory_release(reply->etag);
    memset(reply, 0, sizeof *reply);
}

static int is_token_text(const char *token) {
    size_t length = strlen(token);
    if (length == 0 || length > TOKEN_LIMIT) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        unsigned char character = (unsigned char)token[index];
        if (character <= 0x20 || character >= 0x7F) {
            return 0;
        }
    }
    return 1;
}

int naula_forge_is_host_text(const char *host) {
    size_t length = strlen(host);
    if (length == 0 || length > 255) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        char character = host[index];
        int allowed = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') || character == '.' || character == '-' ||
                      character == ':';
        if (!allowed) {
            return 0;
        }
    }
    return 1;
}

static int is_name_character(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-';
}

int naula_forge_is_name_segment(const char *text) {
    size_t length = strlen(text);
    if (length == 0 || strcmp(text, ".") == 0 || strcmp(text, "..") == 0) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        if (!is_name_character(text[index])) {
            return 0;
        }
    }
    return 1;
}

int naula_forge_is_commit_text(const char *text) {
    size_t length = strlen(text);
    if (length < COMMIT_HEX_MINIMUM || length > COMMIT_HEX_MAXIMUM) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        char character = text[index];
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

int naula_forge_equals_ignoring_case(const char *left, const char *right) {
    for (; *left != '\0' && *right != '\0'; left++, right++) {
        if (*left != *right && !(((*left | 0x20) == (*right | 0x20)) && ((*left | 0x20) >= 'a' && (*left | 0x20) <= 'z'))) {
            return 0;
        }
    }
    return *left == *right;
}

size_t naula_forge_encode_text(const char *text, int keep_slash, char *out, size_t capacity) {
    static const char hex[] = "0123456789ABCDEF";
    size_t length = 0;
    for (const unsigned char *at = (const unsigned char *)text; *at != '\0'; at++) {
        int plain = (*at >= 'a' && *at <= 'z') || (*at >= 'A' && *at <= 'Z') || (*at >= '0' && *at <= '9') ||
                    *at == '-' || *at == '.' || *at == '_' || *at == '~' || (keep_slash && *at == '/');
        if (length + 3 >= capacity) {
            return 0;
        }
        if (plain) {
            out[length++] = (char)*at;
        } else {
            out[length++] = '%';
            out[length++] = hex[*at >> 4];
            out[length++] = hex[*at & 0x0F];
        }
    }
    out[length] = '\0';
    return length + 1;
}

naula_status naula_forge_refuse_input(const forge_flavor *flavor, naula_refusal *refusal, const char *what) {
    naula_refuse(refusal, "invalid_request", "pass a value the provider can address", "%s: %s is not usable",
                 flavor->label, what);
    return NAULA_REFUSED;
}

naula_status naula_forge_refuse_malformed(const forge_flavor *flavor, naula_refusal *refusal, const char *what) {
    naula_refuse(refusal, "malformed_answer", flavor->host_fix, "%s answered %s", flavor->label, what);
    return NAULA_MALFORMED;
}

naula_status naula_forge_refuse_unsupported(const forge_flavor *flavor, naula_refusal *refusal, const char *operation,
                                            const char *fix) {
    naula_refuse(refusal, "unsupported_operation", fix, "the %s provider does not support %s", flavor->label,
                 operation);
    return NAULA_REFUSED;
}

static naula_status refuse_answer(const forge_flavor *flavor, const naula_fetch_answer *answer, const char *url,
                                  naula_refusal *refusal) {
    if (answer->status >= 300 && answer->status <= 399) {
        naula_refuse(refusal, "redirected",
                     "check that the home names the repository's current host; naula never repeats a write elsewhere",
                     "%s answered %s with a redirect naula does not follow (%ld)", flavor->label, url,
                     answer->status);
        return NAULA_REFUSED;
    }
    if (answer->status == 404) {
        naula_refuse(refusal, "not_found", "check the number or the name, and that the repository is public or the token reaches it",
                     "%s answered 404 for %s", flavor->label, url);
        return NAULA_NOT_FOUND;
    }
    if (answer->status == 401) {
        naula_refuse(refusal, "token_needed", "give a token the host accepts", "%s wants a token for %s",
                     flavor->label, url);
        return NAULA_REFUSED;
    }
    if (answer->status == 429) {
        naula_refuse(refusal, "rate_limited", "wait for the host's rate limit to reset, then try again",
                     "%s is rate limiting this caller at %s", flavor->label, url);
        return NAULA_REFUSED;
    }
    if (answer->status == 403) {
        naula_refuse(refusal, "forbidden_or_rate_limited",
                     "give a token that reaches the repository, or wait for the host's rate limit to reset",
                     "%s refused this caller at %s", flavor->label, url);
        return NAULA_REFUSED;
    }
    naula_refuse(refusal, "unexpected_status", flavor->host_fix, "%s answered status %ld at %s", flavor->label,
                 answer->status, url);
    return NAULA_REFUSED;
}

static naula_status settle_answer(const forge_flavor *flavor, const naula_fetch_answer *answer, const char *url,
                                  naula_refusal *refusal) {
    switch (answer->outcome) {
    case NAULA_FETCH_RECEIVED:
        return NAULA_OK;
    case NAULA_FETCH_STATUS:
        return refuse_answer(flavor, answer, url, refusal);
    case NAULA_FETCH_CUT_OFF:
        if (answer->declared == 0) {
            naula_refuse(refusal, "timed_out", answer->fix != NULL ? answer->fix : NAULA_FETCH_NETWORK_FIX, "%s",
                         answer->detail);
            return NAULA_TIMED_OUT;
        }
        naula_refuse(refusal, "cut_off", NAULA_FETCH_NETWORK_FIX, "%s ended before its whole answer arrived", url);
        return NAULA_UNREACHABLE;
    case NAULA_FETCH_STOPPED_BY_SINK:
        naula_refuse(refusal, "answer_too_large", flavor->host_fix, "%s answered more than is read", url);
        return NAULA_REFUSED;
    case NAULA_FETCH_OUT_OF_MEMORY:
        return NAULA_OUT_OF_MEMORY;
    default:
        naula_refuse(refusal, answer->host_unresolved ? "host_not_resolved" : "unreachable",
                     answer->fix != NULL ? answer->fix : NAULA_FETCH_NETWORK_FIX, "%s", answer->detail);
        return NAULA_UNREACHABLE;
    }
}

typedef struct forge_request {
    const char *method;
    const char *url;
    const char *body;
} forge_request;

static naula_fetch_request fetch_request_of(const forge_flavor *flavor, const forge_request *sent,
                                            const char *authorization) {
    naula_fetch_request request;
    memset(&request, 0, sizeof request);
    request.size = sizeof request;
    request.url = sent->url;
    request.deadline_milliseconds = DEADLINE_MILLISECONDS;
    request.authorization = authorization;
    request.extra_headers = flavor->request_headers;
    request.method = sent->method;
    request.body = (const uint8_t *)sent->body;
    request.body_length = sent->body == NULL ? 0 : strlen(sent->body);
    request.content_type = sent->body == NULL ? NULL : JSON_CONTENT_TYPE;
    return request;
}

static naula_status fetch_with_authorization(const forge_flavor *flavor, const forge_request *sent, const char *token,
                                             size_t capacity, forge_reply *reply, naula_refusal *refusal) {
    char authorization[AUTHORIZATION_SIZE];
    if (token != NULL) {
        snprintf(authorization, sizeof authorization, "token %s", token);
    }
    reply->body = naula_memory_allocate(capacity);
    if (reply->body == NULL) {
        naula_overwrite(authorization, sizeof authorization);
        return NAULA_OUT_OF_MEMORY;
    }
    naula_fetch_sink sink;
    naula_fetch_sink_start_memory(&sink, reply->body, capacity);
    naula_fetch_answer answer;
    memset(&answer, 0, sizeof answer);
    answer.size = sizeof answer;
    naula_fetch_request request = fetch_request_of(flavor, sent, token == NULL ? NULL : authorization);
    naula_fetch_send_into_memory(&request, &sink, &answer);
    naula_overwrite(authorization, sizeof authorization);
    naula_status status = settle_answer(flavor, &answer, sent->url, refusal);
    reply->length = (size_t)sink.bytes;
    reply->etag = answer.etag;
    answer.etag = NULL;
    naula_memory_release(answer.last_modified);
    if (status != NAULA_OK) {
        naula_forge_reply_release(reply);
        reply->status = answer.outcome == NAULA_FETCH_STATUS ? answer.status : 0;
    }
    return status;
}

static naula_status send_text(const forge_flavor *flavor, const forge_request *sent, const char *token,
                              size_t capacity, forge_reply *reply, naula_refusal *refusal) {
    memset(reply, 0, sizeof *reply);
    if (token != NULL && !is_token_text(token)) {
        naula_refuse(refusal, "invalid_token", "pass the token itself, on one line with no spaces",
                     "the token is not usable");
        return NAULA_REFUSED;
    }
    return fetch_with_authorization(flavor, sent, token, capacity, reply, refusal);
}

naula_status naula_forge_fetch(const forge_flavor *flavor, const char *url, const char *token, size_t capacity,
                               forge_reply *reply, naula_refusal *refusal) {
    forge_request sent = {NULL, url, NULL};
    return send_text(flavor, &sent, token, capacity, reply, refusal);
}

naula_status naula_forge_send(const forge_flavor *flavor, const char *method, const char *url, const char *token,
                              const cJSON *body, size_t capacity, forge_reply *reply, naula_refusal *refusal) {
    memset(reply, 0, sizeof *reply);
    char *text = body == NULL ? NULL : cJSON_PrintUnformatted(body);
    if (body != NULL && text == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    forge_request sent = {method, url, text};
    naula_status status = send_text(flavor, &sent, token, capacity, reply, refusal);
    cJSON_free(text);
    return status;
}

cJSON *naula_forge_parse_reply(const forge_flavor *flavor, const forge_reply *reply, naula_status *status,
                               naula_refusal *refusal) {
    cJSON *json = NULL;
    const char *reason = NULL;
    naula_status parsed = naula_json_parse(reply->body, reply->length, &json, &reason);
    if (parsed == NAULA_MALFORMED) {
        *status = naula_forge_refuse_malformed(flavor, refusal, "something that is not JSON");
    } else if (parsed != NAULA_OK) {
        *status = parsed;
    }
    return json;
}

int naula_forge_read_exact_number(const cJSON *object, const char *member, uint64_t *number) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, member);
    if (!cJSON_IsNumber(item) || item->valuedouble < 1 || item->valuedouble > LARGEST_EXACT_INTEGER ||
        floor(item->valuedouble) != item->valuedouble) {
        return 0;
    }
    *number = (uint64_t)item->valuedouble;
    return 1;
}

const char *naula_forge_text_member(const cJSON *object, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, member);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static char *copy_text(const char *text) {
    return text == NULL ? NULL : naula_memory_duplicate_text(text);
}

void naula_repository_release(naula_repository *repository) {
    uint32_t size = repository->size;
    naula_memory_release(repository->account);
    naula_memory_release(repository->name);
    naula_memory_release(repository->default_branch);
    naula_memory_release(repository->clone_url);
    naula_memory_release(repository->etag);
    memset(repository, 0, sizeof *repository);
    repository->size = size;
}

void naula_repository_list_release(naula_repository *repositories, size_t count) {
    for (size_t index = 0; index < count; index++) {
        naula_repository_release(&repositories[index]);
    }
    naula_memory_release(repositories);
}

static int covers_release_index(const naula_release_index *index) {
    return index != NULL && index->size >= offsetof(naula_release_index, url) + sizeof index->url;
}

void naula_release_index_release(naula_release_index *index) {
    if (!covers_release_index(index)) {
        return;
    }
    uint32_t size = index->size;
    naula_memory_release(index->bytes);
    naula_memory_release(index->url);
    memset(index, 0, sizeof *index);
    index->size = size;
}

naula_status naula_forge_check_release_request(const forge_flavor *flavor, uint64_t number, const char *tag,
                                               const char *asset, naula_release_index *out, naula_refusal *refusal) {
    if (!covers_release_index(out) || number == 0) {
        return naula_forge_refuse_input(flavor, refusal, "the repository number or the result struct");
    }
    memset(out, 0, sizeof *out);
    out->size = sizeof *out;
    if (asset == NULL || asset[0] == '\0') {
        return naula_forge_refuse_input(flavor, refusal, "the asset name");
    }
    if (tag != NULL && (tag[0] == '\0' || tag[0] == '-')) {
        return naula_forge_refuse_input(flavor, refusal, "the tag");
    }
    return NAULA_OK;
}

int naula_forge_release_route(const char *tag, char *route, size_t capacity) {
    char encoded[FORGE_ROUTE_SIZE / 2];
    if (tag == NULL) {
        return snprintf(route, capacity, "releases/latest") < (int)capacity;
    }
    return naula_forge_encode_text(tag, 0, encoded, sizeof encoded) != 0 &&
           snprintf(route, capacity, "releases/tags/%s", encoded) < (int)capacity;
}

const cJSON *naula_forge_release_asset(const forge_flavor *flavor, const cJSON *release, const char *asset,
                                       naula_status *status, naula_refusal *refusal) {
    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(release, "assets");
    const cJSON *each = NULL;
    if (!cJSON_IsArray(assets)) {
        *status = naula_forge_refuse_malformed(flavor, refusal, "a release with no assets list");
        return NULL;
    }
    cJSON_ArrayForEach(each, assets) {
        const char *name = naula_forge_text_member(each, "name");
        if (name != NULL && strcmp(name, asset) == 0) {
            return each;
        }
    }
    naula_refuse(refusal, "no_index_asset", "ask the owner to publish the index with each release",
                 "the release has no asset named %s", asset);
    *status = NAULA_REFUSED;
    return NULL;
}

naula_status naula_forge_keep_release_index(const forge_flavor *flavor, forge_reply *reply, const cJSON *asset,
                                            naula_release_index *out, naula_refusal *refusal) {
    const char *url = naula_forge_text_member(asset, "browser_download_url");
    if (url == NULL || url[0] == '\0') {
        return naula_forge_refuse_malformed(flavor, refusal, "an asset with no download address");
    }
    uint8_t *bytes = naula_memory_reallocate(reply->body, reply->length == 0 ? 1 : reply->length);
    if (bytes == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    reply->body = NULL;
    out->bytes = bytes;
    out->length = reply->length;
    out->url = copy_text(url);
    if (out->url == NULL) {
        naula_release_index_release(out);
        return NAULA_OUT_OF_MEMORY;
    }
    return NAULA_OK;
}

naula_status naula_forge_fill_repository(const forge_flavor *flavor, const cJSON *json, naula_repository *out,
                                         naula_refusal *refusal) {
    const cJSON *owner = cJSON_GetObjectItemCaseSensitive(json, "owner");
    const char *account = cJSON_IsObject(owner) ? naula_forge_text_member(owner, "login") : NULL;
    const char *name = naula_forge_text_member(json, "name");
    const char *default_branch = naula_forge_text_member(json, "default_branch");
    const char *clone_url = naula_forge_text_member(json, "clone_url");
    const cJSON *parent = cJSON_GetObjectItemCaseSensitive(json, "parent");
    uint32_t size = out->size;
    memset(out, 0, sizeof *out);
    out->size = size;
    if (!naula_forge_read_exact_number(json, "id", &out->number) || account == NULL || name == NULL) {
        return naula_forge_refuse_malformed(flavor, refusal, "a repository with no readable number, owner or name");
    }
    if (cJSON_IsObject(parent) && !naula_forge_read_exact_number(parent, "id", &out->fork_parent)) {
        return naula_forge_refuse_malformed(flavor, refusal, "a fork whose parent has no readable number");
    }
    out->archived = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "archived")) ? 1 : 0;
    out->account = copy_text(account);
    out->name = copy_text(name);
    out->default_branch = copy_text(default_branch);
    out->clone_url = copy_text(clone_url);
    if (out->account == NULL || out->name == NULL || (default_branch != NULL && out->default_branch == NULL) ||
        (clone_url != NULL && out->clone_url == NULL)) {
        naula_repository_release(out);
        return NAULA_OUT_OF_MEMORY;
    }
    return NAULA_OK;
}

int naula_forge_covers_first_shape(const naula_repository *out) {
    return out->size >= offsetof(naula_repository, etag) + sizeof out->etag;
}

int naula_forge_answers_to_the_name(const cJSON *json, const char *account, const char *name) {
    const cJSON *owner = cJSON_GetObjectItemCaseSensitive(json, "owner");
    const char *answered_account = cJSON_IsObject(owner) ? naula_forge_text_member(owner, "login") : NULL;
    const char *answered_name = naula_forge_text_member(json, "name");
    return answered_account != NULL && answered_name != NULL &&
           naula_forge_equals_ignoring_case(answered_account, account) &&
           naula_forge_equals_ignoring_case(answered_name, name);
}

static naula_status append_repository(const forge_flavor *flavor, forge_repository_list *list, const cJSON *json,
                                      naula_refusal *refusal) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? PAGE_CAPACITY : list->capacity * 2;
        naula_repository *items = naula_memory_reallocate(list->items, capacity * sizeof *items);
        if (items == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        list->items = items;
        list->capacity = capacity;
    }
    naula_repository *next = &list->items[list->count];
    next->size = sizeof *next;
    naula_status status = naula_forge_fill_repository(flavor, json, next, refusal);
    if (status == NAULA_OK) {
        list->count++;
    }
    return status;
}

naula_status naula_forge_append_page(const forge_flavor *flavor, forge_repository_list *list, const forge_reply *reply,
                                     size_t *page_count, naula_refusal *refusal) {
    naula_status status = NAULA_OK;
    cJSON *json = naula_forge_parse_reply(flavor, reply, &status, refusal);
    if (json == NULL) {
        return status;
    }
    if (!cJSON_IsArray(json)) {
        status = naula_forge_refuse_malformed(flavor, refusal, "a page that is not a list");
    }
    *page_count = status == NAULA_OK ? (size_t)cJSON_GetArraySize(json) : 0;
    for (const cJSON *item = status == NAULA_OK ? json->child : NULL; item != NULL && status == NAULA_OK;
         item = item->next) {
        status = append_repository(flavor, list, item, refusal);
    }
    cJSON_Delete(json);
    return status;
}

