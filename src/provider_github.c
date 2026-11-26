#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "forge_common.h"
#include "naula/memory.h"
#include "naula/provider.h"
#include "provider_github_hooks.h"
#include "provider_github_internal.h"

#define API_ROOT_DEFAULT "https://api.github.com"
#define API_VERSION_HEADER "X-GitHub-Api-Version: 2022-11-28"
#define ACCEPT_HEADER "Accept: application/vnd.github+json"
#define ROOT_SIZE FORGE_ROOT_SIZE
#define ROUTE_SIZE FORGE_ROUTE_SIZE
#define SMALL_BODY_CAPACITY FORGE_SMALL_BODY_CAPACITY
#define FILE_BODY_CAPACITY FORGE_FILE_BODY_CAPACITY
#define HISTORY_BODY_CAPACITY FORGE_HISTORY_BODY_CAPACITY
#define URL_SIZE 2048
#define PAGE_SIZE 100u
#define PAGE_LIMIT 100u

/**
 * @implNote 403 is labelled forbidden_or_rate_limited because the transport answers no response headers, so
 * x-ratelimit-remaining cannot tell a spent rate limit from a refused token. API version 2022-11-28 was read from
 * x-github-api-version-selected on 2026-11-25.
 */
static const char *const request_headers[] = {API_VERSION_HEADER, ACCEPT_HEADER, NULL};

static const forge_flavor flavor = {"GitHub", "check that the host is the GitHub host the home names", request_headers};

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

static naula_status refuse_with(naula_refusal *refusal, naula_status status, const char *code, const char *fix,
                                const char *format, const char *subject) {
    naula_refuse(refusal, code, fix, format, subject);
    return status;
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
    if (!naula_forge_is_host_text(host)) {
        return naula_forge_refuse_input(&flavor, refusal, "the host");
    }
    snprintf(root, ROOT_SIZE, "https://%s/api/v3", host);
    return NAULA_OK;
}

const forge_flavor *naula_github_flavor(void) {
    return &flavor;
}

naula_status naula_github_request(const char *host, const char *method, const char *route, const char *token,
                                  const cJSON *body, size_t capacity, forge_reply *reply, naula_refusal *refusal) {
    char root[ROOT_SIZE];
    char url[URL_SIZE];
    memset(reply, 0, sizeof *reply);
    naula_status status = build_root(host, root, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    if (snprintf(url, sizeof url, "%s/%s", root, route) >= (int)sizeof url) {
        return naula_forge_refuse_input(&flavor, refusal, "the request address (too long)");
    }
    return naula_forge_send(&flavor, method, url, token, body, capacity, reply, refusal);
}

static naula_status github_get(const char *host, const char *route, const char *token, size_t capacity,
                               forge_reply *reply, naula_refusal *refusal) {
    return naula_github_request(host, NULL, route, token, NULL, capacity, reply, refusal);
}

static naula_status github_lookup(void *context, const char *host, uint64_t number, const char *token,
                                  naula_repository *out, naula_refusal *refusal) {
    char route[ROUTE_SIZE];
    forge_reply reply;
    (void)context;
    if (!naula_forge_covers_first_shape(out) || number == 0) {
        return naula_forge_refuse_input(&flavor, refusal, "the repository number or the result struct");
    }
    snprintf(route, sizeof route, "repositories/%llu", (unsigned long long)number);
    naula_status status = github_get(host, route, token, SMALL_BODY_CAPACITY, &reply, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    cJSON *json = naula_forge_parse_reply(&flavor, &reply, &status, refusal);
    uint64_t echoed = 0;
    if (json != NULL && !naula_forge_read_exact_number(json, "id", &echoed)) {
        status = naula_forge_refuse_malformed(&flavor, refusal, "a repository with no number that can be read exactly");
    } else if (json != NULL && echoed != number) {
        status = refuse_with(refusal, NAULA_REFUSED, "number_mismatch", "check the number in the id",
                             "GitHub answered another repository than the one asked for (%s)", route);
    } else if (json != NULL) {
        status = naula_forge_fill_repository(&flavor, json, out, refusal);
    }
    if (status == NAULA_OK) {
        out->etag = reply.etag;
        reply.etag = NULL;
    }
    cJSON_Delete(json);
    naula_forge_reply_release(&reply);
    return status;
}

static naula_status github_find(void *context, const char *host, const char *account, const char *name,
                                const char *token, uint64_t *number, naula_refusal *refusal) {
    char route[ROUTE_SIZE];
    forge_reply reply;
    naula_refusal absent;
    (void)context;
    if (!naula_forge_is_name_segment(account) || !naula_forge_is_name_segment(name)) {
        return naula_forge_refuse_input(&flavor, refusal, "the account or the name");
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
        if (refusal != NULL) {
            *refusal = absent;
        }
        return status;
    }
    cJSON *json = naula_forge_parse_reply(&flavor, &reply, &status, refusal);
    if (json != NULL && !naula_forge_read_exact_number(json, "id", number)) {
        status = naula_forge_refuse_malformed(&flavor, refusal, "a repository with no number that can be read exactly");
    } else if (json != NULL && !naula_forge_answers_to_the_name(json, account, name)) {
        *number = 0;
    }
    cJSON_Delete(json);
    naula_forge_reply_release(&reply);
    return status;
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
    const char *type = naula_forge_text_member(json, "type");
    const char *encoding = naula_forge_text_member(json, "encoding");
    const char *content = naula_forge_text_member(json, "content");
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
        return naula_forge_refuse_malformed(&flavor, refusal, "file content that is not base64");
    }
    return NAULA_OK;
}

static naula_status build_file_route(uint64_t number, const char *ref, const char *path, char *route,
                                     naula_refusal *refusal) {
    char encoded_path[ROUTE_SIZE / 2];
    char encoded_ref[ROUTE_SIZE / 4];
    if (path == NULL || path[0] == '\0' || naula_forge_encode_text(path, 1, encoded_path, sizeof encoded_path) == 0) {
        return naula_forge_refuse_input(&flavor, refusal, "the path");
    }
    if (ref == NULL) {
        snprintf(route, ROUTE_SIZE, "repositories/%llu/contents/%s", (unsigned long long)number, encoded_path);
        return NAULA_OK;
    }
    if (ref[0] == '\0' || naula_forge_encode_text(ref, 0, encoded_ref, sizeof encoded_ref) == 0) {
        return naula_forge_refuse_input(&flavor, refusal, "the ref");
    }
    snprintf(route, ROUTE_SIZE, "repositories/%llu/contents/%s?ref=%s", (unsigned long long)number, encoded_path,
             encoded_ref);
    return NAULA_OK;
}

naula_status naula_github_still_the_same_number(const char *host, uint64_t number, const char *token,
                                               naula_refusal *refusal) {
    naula_repository repository;
    memset(&repository, 0, sizeof repository);
    repository.size = sizeof repository;
    naula_status status = github_lookup(NULL, host, number, token, &repository, refusal);
    naula_repository_release(&repository);
    return status;
}

/** A 404 on a file or a commit reads the same as a repository that is gone, so the repository decides which. */
static naula_status settle_missing(const char *host, uint64_t number, const char *token, const char *code,
                                   const char *what, const char *fix, naula_refusal *refusal) {
    naula_status status = naula_github_still_the_same_number(host, number, token, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    return refuse_with(refusal, NAULA_REFUSED, code, fix, "the repository has no %s asked for", what);
}

static naula_status github_read_file(void *context, const char *host, uint64_t number, const char *ref,
                                     const char *path, const char *token, uint8_t **bytes, size_t *length,
                                     naula_refusal *refusal) {
    char route[ROUTE_SIZE];
    forge_reply reply;
    (void)context;
    *bytes = NULL;
    *length = 0;
    naula_status status = number == 0 ? naula_forge_refuse_input(&flavor, refusal, "the repository number")
                                      : build_file_route(number, ref, path, route, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    status = github_get(host, route, token, FILE_BODY_CAPACITY, &reply, refusal);
    if (status == NAULA_NOT_FOUND) {
        return settle_missing(host, number, token, "missing_file_or_ref", "file or ref",
                              "check the path and the ref exist in the repository", refusal);
    }
    if (status != NAULA_OK) {
        return status;
    }
    cJSON *json = naula_forge_parse_reply(&flavor, &reply, &status, refusal);
    if (json != NULL) {
        status = cJSON_IsObject(json) ? decode_file(json, bytes, length, refusal)
                                      : refuse_with(refusal, NAULA_REFUSED, "not_a_file", "name a file, not a folder",
                                                    "the path %s is a folder", path);
    }
    cJSON_Delete(json);
    naula_forge_reply_release(&reply);
    if (status == NAULA_OK) {
        status = naula_github_still_the_same_number(host, number, token, refusal);
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

static naula_status github_contains(void *context, const char *host, uint64_t number, const char *ref,
                                    const char *commit, const char *token, int *contained, naula_refusal *refusal) {
    char route[ROUTE_SIZE];
    char encoded_ref[ROUTE_SIZE / 2];
    forge_reply reply;
    (void)context;
    *contained = 0;
    if (number == 0 || commit == NULL || !naula_forge_is_commit_text(commit) || ref == NULL || ref[0] == '\0' ||
        ref[0] == '-' || naula_forge_encode_text(ref, 1, encoded_ref, sizeof encoded_ref) == 0) {
        return naula_forge_refuse_input(&flavor, refusal, "the number, the commit or the ref");
    }
    snprintf(route, sizeof route, "repositories/%llu/compare/%s...%s?per_page=1", (unsigned long long)number, commit,
             encoded_ref);
    naula_status status = github_get(host, route, token, HISTORY_BODY_CAPACITY, &reply, refusal);
    if (status == NAULA_NOT_FOUND) {
        return settle_missing(host, number, token, "missing_commit_or_ref", "commit or ref",
                              "check the commit and the branch exist in the repository", refusal);
    }
    if (status != NAULA_OK) {
        return status;
    }
    cJSON *json = naula_forge_parse_reply(&flavor, &reply, &status, refusal);
    const char *relation = json == NULL ? NULL : naula_forge_text_member(json, "status");
    if (json != NULL && relation == NULL) {
        status = naula_forge_refuse_malformed(&flavor, refusal, "a comparison with no status");
    } else if (relation != NULL) {
        *contained = is_contained_status(relation);
    }
    cJSON_Delete(json);
    naula_forge_reply_release(&reply);
    return status;
}

static naula_status github_parent_count(void *context, const char *host, uint64_t number, const char *commit,
                                        const char *token, size_t *count, naula_refusal *refusal) {
    char route[ROUTE_SIZE];
    forge_reply reply;
    (void)context;
    *count = 0;
    if (number == 0 || commit == NULL || !naula_forge_is_commit_text(commit)) {
        return naula_forge_refuse_input(&flavor, refusal, "the number or the commit");
    }
    snprintf(route, sizeof route, "repositories/%llu/commits/%s", (unsigned long long)number, commit);
    naula_status status = github_get(host, route, token, HISTORY_BODY_CAPACITY, &reply, refusal);
    if (status == NAULA_NOT_FOUND) {
        return settle_missing(host, number, token, "missing_commit", "commit",
                              "check the commit exists in the repository", refusal);
    }
    if (status != NAULA_OK) {
        return status;
    }
    cJSON *json = naula_forge_parse_reply(&flavor, &reply, &status, refusal);
    const cJSON *parents = json == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(json, "parents");
    if (json != NULL && !cJSON_IsArray(parents)) {
        status = naula_forge_refuse_malformed(&flavor, refusal, "a commit with no parents list");
    } else if (parents != NULL) {
        *count = (size_t)cJSON_GetArraySize(parents);
    }
    cJSON_Delete(json);
    naula_forge_reply_release(&reply);
    return status;
}

static naula_status read_pages(const char *host, const char *kind, const char *account, const char *token,
                               forge_repository_list *list, naula_refusal *refusal) {
    for (unsigned page = 1; page <= PAGE_LIMIT; page++) {
        char route[ROUTE_SIZE];
        forge_reply reply;
        size_t page_count = 0;
        snprintf(route, sizeof route, "%s/%s/repos?per_page=%u&page=%u", kind, account, PAGE_SIZE, page);
        naula_status status = github_get(host, route, token, HISTORY_BODY_CAPACITY, &reply, refusal);
        if (status == NAULA_OK) {
            status = naula_forge_append_page(&flavor, list, &reply, &page_count, refusal);
            naula_forge_reply_release(&reply);
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
    forge_repository_list list = {NULL, 0, 0};
    naula_refusal absent;
    (void)context;
    *out = NULL;
    *count = 0;
    if (!naula_forge_is_name_segment(account)) {
        return naula_forge_refuse_input(&flavor, refusal, "the account");
    }
    memset(&absent, 0, sizeof absent);
    absent.size = sizeof absent;
    naula_status status = read_pages(host, "orgs", account, token, &list, &absent);
    if (status == NAULA_NOT_FOUND) {
        naula_repository_list_release(list.items, list.count);
        list = (forge_repository_list){NULL, 0, 0};
        status = read_pages(host, "users", account, token, &list, refusal);
    } else if (status != NAULA_OK && refusal != NULL) {
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
                 NAULA_OP_LIST_REPOS | NAULA_OP_CREATE | NAULA_OP_RENAME | NAULA_OP_TRANSFER | NAULA_OP_ARCHIVE |
                 NAULA_OP_READ_METADATA | NAULA_OP_WRITE_METADATA;
    *anonymous_lookup = 1;
    return NAULA_OK;
}

static const naula_provider github_provider = {sizeof(naula_provider), "github",
                                               NULL,                   github_describe,
                                               github_lookup,          github_find,
                                               github_read_file,       github_contains,
                                               github_parent_count,    github_list_repos,
                                               github_release_index,   naula_github_create,
                                               naula_github_rename,    naula_github_transfer,
                                               naula_github_archive,   naula_github_read_metadata,
                                               naula_github_write_metadata};

const naula_provider *naula_provider_github(void) {
    return &github_provider;
}
