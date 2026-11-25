#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "naula/memory.h"
#include "naula/provider.h"
#include "naula/transport.h"
#include "provider_github_hooks.h"

#define API_ROOT_DEFAULT "https://api.github.com"
#define ROOT_SIZE 300
#define URL_SIZE 2048
#define ROUTE_SIZE 1536
#define DEADLINE_MILLISECONDS 30000u
#define SMALL_BODY_CAPACITY (256u * 1024u)
#define FILE_BODY_CAPACITY (2u * 1024u * 1024u)
#define HISTORY_BODY_CAPACITY (8u * 1024u * 1024u)
#define TOKEN_LIMIT 512u
#define AUTHORIZATION_SIZE (TOKEN_LIMIT + 16u)
#define LARGEST_EXACT_INTEGER 9007199254740991.0
#define PAGE_SIZE 100u
#define PAGE_LIMIT 100u
#define COMMIT_HEX_MINIMUM 7u
#define COMMIT_HEX_MAXIMUM 64u

static const char *const HOST_FIX = "check that the host is the GitHub host the home names";

static char api_root_override[ROOT_SIZE];

#ifdef NAULA_TEST_HOOKS
void naula_github_use_api_root(const char *root) {
    if (root == NULL) {
        api_root_override[0] = '\0';
    } else {
        snprintf(api_root_override, sizeof api_root_override, "%s", root);
    }
}
#endif

typedef struct github_reply {
    uint8_t *body;
    size_t length;
    char *etag;
} github_reply;

static void reply_release(github_reply *reply) {
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

static int is_host_text(const char *host) {
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

static int is_name_segment(const char *text) {
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

static int is_commit_text(const char *text) {
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

static naula_status refuse_with(naula_refusal *refusal, naula_status status, const char *code, const char *fix,
                                const char *format, const char *subject) {
    naula_refuse(refusal, code, fix, format, subject);
    return status;
}

static naula_status refuse_input(naula_refusal *refusal, const char *what) {
    return refuse_with(refusal, NAULA_REFUSED, "invalid_request", "pass a value the GitHub provider can address",
                       "%s is not usable", what);
}

static naula_status refuse_malformed(naula_refusal *refusal, const char *what) {
    return refuse_with(refusal, NAULA_MALFORMED, "malformed_answer", HOST_FIX, "GitHub answered %s", what);
}

static naula_status build_root(const char *host, char *root, naula_refusal *refusal) {
    if (api_root_override[0] != '\0') {
        snprintf(root, ROOT_SIZE, "%s", api_root_override);
        return NAULA_OK;
    }
    if (host == NULL || strcmp(host, "github.com") == 0) {
        snprintf(root, ROOT_SIZE, "%s", API_ROOT_DEFAULT);
        return NAULA_OK;
    }
    if (!is_host_text(host)) {
        return refuse_input(refusal, "the host");
    }
    snprintf(root, ROOT_SIZE, "https://%s/api/v3", host);
    return NAULA_OK;
}

static naula_status refuse_answer(const naula_fetch_answer *answer, const char *url, naula_refusal *refusal) {
    char subject[URL_SIZE + 16];
    snprintf(subject, sizeof subject, "%s: %ld", url, answer->status);
    if (answer->status == 404) {
        return refuse_with(refusal, NAULA_NOT_FOUND, "not_found", "check the number or the name, and that the repository is public or the token reaches it",
                           "GitHub answered 404 for %s", subject);
    }
    if (answer->status == 401) {
        return refuse_with(refusal, NAULA_REFUSED, "token_needed", "give a token the host accepts",
                           "GitHub wants a token for %s", subject);
    }
    if (answer->status == 403 || answer->status == 429) {
        return refuse_with(refusal, NAULA_REFUSED, "rate_limited", "wait for the host's rate limit to reset, then try again",
                           "GitHub is rate limiting this caller at %s", subject);
    }
    return refuse_with(refusal, NAULA_REFUSED, "unexpected_status", HOST_FIX, "GitHub answered an unexpected status at %s", subject);
}

static naula_status settle_answer(const naula_fetch_answer *answer, const char *url, naula_refusal *refusal) {
    switch (answer->outcome) {
    case NAULA_FETCH_RECEIVED:
        return NAULA_OK;
    case NAULA_FETCH_STATUS:
        return refuse_answer(answer, url, refusal);
    case NAULA_FETCH_CUT_OFF:
        if (answer->declared == 0) {
            naula_refuse(refusal, "timed_out", answer->fix != NULL ? answer->fix : NAULA_FETCH_NETWORK_FIX, "%s",
                         answer->detail);
            return NAULA_TIMED_OUT;
        }
        return refuse_with(refusal, NAULA_UNREACHABLE, "cut_off", NAULA_FETCH_NETWORK_FIX,
                           "%s ended before its whole answer arrived", url);
    case NAULA_FETCH_STOPPED_BY_SINK:
        return refuse_with(refusal, NAULA_REFUSED, "answer_too_large", HOST_FIX, "%s answered more than is read", url);
    case NAULA_FETCH_OUT_OF_MEMORY:
        return NAULA_OUT_OF_MEMORY;
    default:
        naula_refuse(refusal, "unreachable", answer->fix != NULL ? answer->fix : NAULA_FETCH_NETWORK_FIX, "%s",
                     answer->detail);
        return NAULA_UNREACHABLE;
    }
}

static naula_status get_answer(const char *url, const char *token, size_t capacity, github_reply *reply,
                               naula_refusal *refusal) {
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
    naula_fetch_into_memory(url, token == NULL ? NULL : authorization, DEADLINE_MILLISECONDS, &sink, &answer);
    naula_overwrite(authorization, sizeof authorization);
    naula_status status = settle_answer(&answer, url, refusal);
    reply->length = (size_t)sink.bytes;
    reply->etag = answer.etag;
    answer.etag = NULL;
    naula_memory_release(answer.last_modified);
    if (status != NAULA_OK) {
        reply_release(reply);
    }
    return status;
}

static naula_status github_get(const char *host, const char *route, const char *token, size_t capacity,
                               github_reply *reply, naula_refusal *refusal) {
    char root[ROOT_SIZE];
    char url[URL_SIZE];
    memset(reply, 0, sizeof *reply);
    if (token != NULL && !is_token_text(token)) {
        return refuse_with(refusal, NAULA_REFUSED, "invalid_token", "pass the token itself, on one line with no spaces",
                           "the %s is not usable", "token");
    }
    naula_status status = build_root(host, root, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    if (snprintf(url, sizeof url, "%s/%s", root, route) >= (int)sizeof url) {
        return refuse_input(refusal, "the request address (too long)");
    }
    return get_answer(url, token, capacity, reply, refusal);
}

static cJSON *parse_reply(const github_reply *reply, naula_status *status, naula_refusal *refusal) {
    cJSON *json = cJSON_ParseWithLength((const char *)reply->body, reply->length);
    if (json == NULL) {
        *status = refuse_malformed(refusal, "something that is not JSON");
    }
    return json;
}

static int read_exact_number(const cJSON *object, const char *member, uint64_t *number) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, member);
    if (!cJSON_IsNumber(item) || item->valuedouble < 1 || item->valuedouble > LARGEST_EXACT_INTEGER ||
        floor(item->valuedouble) != item->valuedouble) {
        return 0;
    }
    *number = (uint64_t)item->valuedouble;
    return 1;
}

static const char *text_member(const cJSON *object, const char *member) {
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

static naula_status fill_repository(const cJSON *json, naula_repository *out, naula_refusal *refusal) {
    const cJSON *owner = cJSON_GetObjectItemCaseSensitive(json, "owner");
    const char *account = cJSON_IsObject(owner) ? text_member(owner, "login") : NULL;
    const char *name = text_member(json, "name");
    const char *default_branch = text_member(json, "default_branch");
    const char *clone_url = text_member(json, "clone_url");
    const cJSON *parent = cJSON_GetObjectItemCaseSensitive(json, "parent");
    uint32_t size = out->size;
    memset(out, 0, sizeof *out);
    out->size = size;
    if (!read_exact_number(json, "id", &out->number) || account == NULL || name == NULL) {
        return refuse_malformed(refusal, "a repository with no readable number, owner or name");
    }
    if (cJSON_IsObject(parent) && !read_exact_number(parent, "id", &out->fork_parent)) {
        return refuse_malformed(refusal, "a fork whose parent has no readable number");
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

static int covers_first_shape(const naula_repository *out) {
    return out->size >= offsetof(naula_repository, etag) + sizeof out->etag;
}

static naula_status github_lookup(void *context, const char *host, uint64_t number, const char *token,
                                  naula_repository *out, naula_refusal *refusal) {
    char route[ROUTE_SIZE];
    github_reply reply;
    (void)context;
    if (!covers_first_shape(out) || number == 0) {
        return refuse_input(refusal, "the repository number or the result struct");
    }
    snprintf(route, sizeof route, "repositories/%llu", (unsigned long long)number);
    naula_status status = github_get(host, route, token, SMALL_BODY_CAPACITY, &reply, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    cJSON *json = parse_reply(&reply, &status, refusal);
    uint64_t echoed = 0;
    if (json != NULL && !read_exact_number(json, "id", &echoed)) {
        status = refuse_malformed(refusal, "a repository with no number that can be read exactly");
    } else if (json != NULL && echoed != number) {
        status = refuse_with(refusal, NAULA_REFUSED, "number_mismatch", "check the number in the id",
                             "GitHub answered another repository than the one asked for (%s)", route);
    } else if (json != NULL) {
        status = fill_repository(json, out, refusal);
    }
    if (status == NAULA_OK) {
        out->etag = reply.etag;
        reply.etag = NULL;
    }
    cJSON_Delete(json);
    reply_release(&reply);
    return status;
}

static naula_status github_find(void *context, const char *host, const char *account, const char *name,
                                const char *token, uint64_t *number, naula_refusal *refusal) {
    char route[ROUTE_SIZE];
    github_reply reply;
    naula_refusal absent;
    (void)context;
    if (!is_name_segment(account) || !is_name_segment(name)) {
        return refuse_input(refusal, "the account or the name");
    }
    *number = 0;
    memset(&absent, 0, sizeof absent);
    absent.size = sizeof absent;
    snprintf(route, sizeof route, "repos/%s/%s", account, name);
    naula_status status = github_get(host, route, token, SMALL_BODY_CAPACITY, &reply, &absent);
    if (status == NAULA_NOT_FOUND) {
        return NAULA_OK;
    }
    if (status != NAULA_OK) {
        *refusal = absent;
        return status;
    }
    cJSON *json = parse_reply(&reply, &status, refusal);
    if (json != NULL && !read_exact_number(json, "id", number)) {
        status = refuse_malformed(refusal, "a repository with no number that can be read exactly");
    }
    cJSON_Delete(json);
    reply_release(&reply);
    return status;
}

static size_t encode_text(const char *text, int keep_slash, char *out, size_t capacity) {
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

static int base64_value(char character) {
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    return character == '+' ? 62 : (character == '/' ? 63 : -1);
}

static int decode_base64(const char *text, uint8_t *out, size_t *length) {
    uint32_t accumulator = 0;
    int bits = 0;
    size_t written = 0;
    for (const char *at = text; *at != '\0' && *at != '='; at++) {
        if (*at == '\n' || *at == '\r') {
            continue;
        }
        int value = base64_value(*at);
        if (value < 0) {
            return 0;
        }
        accumulator = (accumulator << 6) | (uint32_t)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[written++] = (uint8_t)((accumulator >> bits) & 0xFF);
        }
    }
    *length = written;
    return 1;
}

static naula_status decode_file(const cJSON *json, uint8_t **bytes, size_t *length, naula_refusal *refusal) {
    const char *type = text_member(json, "type");
    const char *encoding = text_member(json, "encoding");
    const char *content = text_member(json, "content");
    if (type == NULL || strcmp(type, "file") != 0) {
        return refuse_with(refusal, NAULA_REFUSED, "not_a_file", "name a file, not a folder or a link",
                           "the path is %s, not a file", type == NULL ? "something else" : type);
    }
    if (encoding == NULL || strcmp(encoding, "base64") != 0 || content == NULL) {
        return refuse_with(refusal, NAULA_REFUSED, "file_too_large", "read only files GitHub's contents API inlines (1 MB)",
                           "the file arrived as %s", encoding == NULL ? "no inline content" : encoding);
    }
    *bytes = naula_memory_allocate(strlen(content) / 4 * 3 + 3);
    if (*bytes == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    if (!decode_base64(content, *bytes, length)) {
        naula_memory_release(*bytes);
        *bytes = NULL;
        return refuse_malformed(refusal, "file content that is not base64");
    }
    return NAULA_OK;
}

static naula_status build_file_route(uint64_t number, const char *ref, const char *path, char *route,
                                     naula_refusal *refusal) {
    char encoded_path[ROUTE_SIZE / 2];
    char encoded_ref[ROUTE_SIZE / 4];
    if (path == NULL || path[0] == '\0' || encode_text(path, 1, encoded_path, sizeof encoded_path) == 0) {
        return refuse_input(refusal, "the path");
    }
    if (ref == NULL) {
        snprintf(route, ROUTE_SIZE, "repositories/%llu/contents/%s", (unsigned long long)number, encoded_path);
        return NAULA_OK;
    }
    if (ref[0] == '\0' || encode_text(ref, 0, encoded_ref, sizeof encoded_ref) == 0) {
        return refuse_input(refusal, "the ref");
    }
    snprintf(route, ROUTE_SIZE, "repositories/%llu/contents/%s?ref=%s", (unsigned long long)number, encoded_path,
             encoded_ref);
    return NAULA_OK;
}

static naula_status still_the_same_number(const char *host, uint64_t number, const char *token,
                                          naula_refusal *refusal) {
    naula_repository repository;
    memset(&repository, 0, sizeof repository);
    repository.size = sizeof repository;
    naula_status status = github_lookup(NULL, host, number, token, &repository, refusal);
    naula_repository_release(&repository);
    return status;
}

static naula_status github_read_file(void *context, const char *host, uint64_t number, const char *ref,
                                     const char *path, const char *token, uint8_t **bytes, size_t *length,
                                     naula_refusal *refusal) {
    char route[ROUTE_SIZE];
    github_reply reply;
    (void)context;
    *bytes = NULL;
    *length = 0;
    naula_status status = number == 0 ? refuse_input(refusal, "the repository number")
                                      : build_file_route(number, ref, path, route, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    status = github_get(host, route, token, FILE_BODY_CAPACITY, &reply, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    cJSON *json = parse_reply(&reply, &status, refusal);
    if (json != NULL) {
        status = cJSON_IsObject(json) ? decode_file(json, bytes, length, refusal)
                                      : refuse_with(refusal, NAULA_REFUSED, "not_a_file", "name a file, not a folder",
                                                    "the path %s is a folder", path);
    }
    cJSON_Delete(json);
    reply_release(&reply);
    if (status == NAULA_OK) {
        status = still_the_same_number(host, number, token, refusal);
        if (status != NAULA_OK) {
            naula_memory_release(*bytes);
            *bytes = NULL;
            *length = 0;
        }
    }
    return status;
}

static int is_contained_status(const char *status) {
    return strcmp(status, "ahead") == 0 || strcmp(status, "identical") == 0;
}

/** A commit GitHub does not know answers 404 on compare, the same as a repository that is gone, so the repository decides which. */
static naula_status settle_missing_commit(const char *host, uint64_t number, const char *token, int *contained,
                                          naula_refusal *refusal) {
    naula_status status = still_the_same_number(host, number, token, refusal);
    if (status == NAULA_OK) {
        *contained = 0;
    }
    return status;
}

static naula_status github_contains(void *context, const char *host, uint64_t number, const char *ref,
                                    const char *commit, const char *token, int *contained, naula_refusal *refusal) {
    char route[ROUTE_SIZE];
    char encoded_ref[ROUTE_SIZE / 2];
    github_reply reply;
    (void)context;
    *contained = 0;
    if (number == 0 || commit == NULL || !is_commit_text(commit) || ref == NULL || ref[0] == '\0' ||
        encode_text(ref, 1, encoded_ref, sizeof encoded_ref) == 0) {
        return refuse_input(refusal, "the number, the commit or the ref");
    }
    snprintf(route, sizeof route, "repositories/%llu/compare/%s...%s?per_page=1", (unsigned long long)number, commit,
             encoded_ref);
    naula_status status = github_get(host, route, token, HISTORY_BODY_CAPACITY, &reply, refusal);
    if (status == NAULA_NOT_FOUND) {
        return settle_missing_commit(host, number, token, contained, refusal);
    }
    if (status != NAULA_OK) {
        return status;
    }
    cJSON *json = parse_reply(&reply, &status, refusal);
    const char *relation = json == NULL ? NULL : text_member(json, "status");
    if (json != NULL && relation == NULL) {
        status = refuse_malformed(refusal, "a comparison with no status");
    } else if (relation != NULL) {
        *contained = is_contained_status(relation);
    }
    cJSON_Delete(json);
    reply_release(&reply);
    return status;
}

static naula_status github_parent_count(void *context, const char *host, uint64_t number, const char *commit,
                                        const char *token, size_t *count, naula_refusal *refusal) {
    char route[ROUTE_SIZE];
    github_reply reply;
    (void)context;
    *count = 0;
    if (number == 0 || commit == NULL || !is_commit_text(commit)) {
        return refuse_input(refusal, "the number or the commit");
    }
    snprintf(route, sizeof route, "repositories/%llu/commits/%s", (unsigned long long)number, commit);
    naula_status status = github_get(host, route, token, HISTORY_BODY_CAPACITY, &reply, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    cJSON *json = parse_reply(&reply, &status, refusal);
    const cJSON *parents = json == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(json, "parents");
    if (json != NULL && !cJSON_IsArray(parents)) {
        status = refuse_malformed(refusal, "a commit with no parents list");
    } else if (parents != NULL) {
        *count = (size_t)cJSON_GetArraySize(parents);
    }
    cJSON_Delete(json);
    reply_release(&reply);
    return status;
}

typedef struct repository_list {
    naula_repository *items;
    size_t count;
    size_t capacity;
} repository_list;

static naula_status append_repository(repository_list *list, const cJSON *json, naula_refusal *refusal) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? PAGE_SIZE : list->capacity * 2;
        naula_repository *items = naula_memory_reallocate(list->items, capacity * sizeof *items);
        if (items == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        list->items = items;
        list->capacity = capacity;
    }
    naula_repository *next = &list->items[list->count];
    next->size = sizeof *next;
    naula_status status = fill_repository(json, next, refusal);
    if (status == NAULA_OK) {
        list->count++;
    }
    return status;
}

static naula_status append_page(repository_list *list, const github_reply *reply, size_t *page_count,
                                naula_refusal *refusal) {
    naula_status status = NAULA_OK;
    cJSON *json = parse_reply(reply, &status, refusal);
    if (json == NULL) {
        return status;
    }
    if (!cJSON_IsArray(json)) {
        status = refuse_malformed(refusal, "a page that is not a list");
    }
    *page_count = status == NAULA_OK ? (size_t)cJSON_GetArraySize(json) : 0;
    for (const cJSON *item = status == NAULA_OK ? json->child : NULL; item != NULL && status == NAULA_OK;
         item = item->next) {
        status = append_repository(list, item, refusal);
    }
    cJSON_Delete(json);
    return status;
}

static naula_status read_pages(const char *host, const char *kind, const char *account, const char *token,
                               repository_list *list, naula_refusal *refusal) {
    for (unsigned page = 1; page <= PAGE_LIMIT; page++) {
        char route[ROUTE_SIZE];
        github_reply reply;
        size_t page_count = 0;
        snprintf(route, sizeof route, "%s/%s/repos?per_page=%u&page=%u", kind, account, PAGE_SIZE, page);
        naula_status status = github_get(host, route, token, HISTORY_BODY_CAPACITY, &reply, refusal);
        if (status == NAULA_OK) {
            status = append_page(list, &reply, &page_count, refusal);
            reply_release(&reply);
        }
        if (status != NAULA_OK || page_count < PAGE_SIZE) {
            return status;
        }
    }
    return refuse_with(refusal, NAULA_REFUSED, "too_many_repositories", "list a smaller account",
                       "%s holds more repositories than are listed", account);
}

static naula_status github_list_repos(void *context, const char *host, const char *account, const char *token,
                                      naula_repository **out, size_t *count, naula_refusal *refusal) {
    repository_list list = {NULL, 0, 0};
    naula_refusal absent;
    (void)context;
    *out = NULL;
    *count = 0;
    if (!is_name_segment(account)) {
        return refuse_input(refusal, "the account");
    }
    memset(&absent, 0, sizeof absent);
    absent.size = sizeof absent;
    naula_status status = read_pages(host, "orgs", account, token, &list, &absent);
    if (status == NAULA_NOT_FOUND) {
        naula_repository_list_release(list.items, list.count);
        list = (repository_list){NULL, 0, 0};
        status = read_pages(host, "users", account, token, &list, refusal);
    } else if (status != NAULA_OK) {
        *refusal = absent;
    }
    if (status != NAULA_OK) {
        naula_repository_list_release(list.items, list.count);
        return status;
    }
    *out = list.items;
    *count = list.count;
    return NAULA_OK;
}

static naula_status github_release_index(void *context, const char *host, uint64_t number, const char *tag,
                                         const char *token, uint8_t **bytes, size_t *length, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)number;
    (void)tag;
    (void)token;
    *bytes = NULL;
    *length = 0;
    return refuse_with(refusal, NAULA_REFUSED, "unsupported_operation", "read the index from a web or url home",
                       "the github provider does not support %s", "release_index");
}

static naula_status github_describe(void *context, uint32_t *supported, int *anonymous_lookup) {
    (void)context;
    *supported = NAULA_OP_LOOKUP | NAULA_OP_FIND | NAULA_OP_READ_FILE | NAULA_OP_CONTAINS | NAULA_OP_PARENT_COUNT |
                 NAULA_OP_LIST_REPOS;
    *anonymous_lookup = 1;
    return NAULA_OK;
}

static const naula_provider github_provider = {sizeof(naula_provider), "github", NULL,           github_describe,
                                               github_lookup,          github_find, github_read_file,
                                               github_contains,        github_parent_count,
                                               github_list_repos,      github_release_index};

const naula_provider *naula_provider_github(void) {
    return &github_provider;
}
