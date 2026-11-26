#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "forge_common.h"
#include "naula/memory.h"
#include "naula/provider.h"
#include "provider_gitea_hooks.h"
#include "provider_gitea_internal.h"

#define ACCEPT_HEADER "Accept: application/json"
#define URL_SIZE 2048
#define NAME_SIZE 256
#define PAGE_SIZE 50u
#define PAGE_LIMIT 200u
#define MINIMUM_MAJOR 1ul
#define MINIMUM_MINOR 20ul
#define MINIMUM_FIX "upgrade the host to Gitea 1.20 or later, or to a Forgejo built on it"
#define GITEA_PART "+gitea-"
#define VERSION_NUMBER_LIMIT 100000ul
#define API_PATH "/api/v1"
#define ATTACHMENT_ID_LIMIT 64u

/**
 * @implNote Gitea and Forgejo answer one API, so one table serves both words. Forgejo 17.0.0-121 (API of Gitea 1.22.0)
 * was read from codeberg.org/swagger.v1.json on 2026-11-25 and sends no API version header, so none is sent here.
 * contains asks for the commits the commit holds that the branch lacks and reads an empty list as contained; a compare
 * answer carries every commit since the common base, so on a long history it passes the read limit.
 * `not`, and `stat`, `verification` and `files` on both commit routes, arrived in Gitea 1.20 (docs/providers.md), and a
 * host that ignores `not` would answer every commit as missing, which is why the version is checked first.
 */
static const struct {
    const char *by_number;
    const char *by_name;
    const char *raw_file;
    const char *commit;
    const char *commits_not_in;
    const char *version;
    const char *organization_repositories;
    const char *user_repositories;
} routes = {"repositories/%llu",
            "repos/%s/%s",
            "%s/raw/%s",
            "%s/git/commits/%s?stat=false&verification=false&files=false",
            "%s/commits?sha=%s&not=%s&limit=1&stat=false&verification=false&files=false",
            "version",
            "orgs/%s/repos?limit=%u&page=%u",
            "users/%s/repos?limit=%u&page=%u"};

static const missing_answer missing_file = {"missing_file_or_ref", "file or ref",
                                            "check the path and the ref exist in the repository"};
static const missing_answer missing_commit = {"missing_commit", "commit", "check the commit exists in the repository"};
static const missing_answer missing_commit_or_ref = {"missing_commit_or_ref", "commit or ref",
                                                     "check the commit and the branch exist in the repository"};
static const missing_answer missing_release = {"missing_release", "release",
                                               "publish a release, or name a tag the repository has"};
static const missing_answer missing_index = {"missing_index", "index asset", "publish the index with the release again"};

static const char *const request_headers[] = {ACCEPT_HEADER, NULL};

static const gitea_edition gitea_edition_gitea = {
    "gitea", {"Gitea", "check that the host runs Gitea and the home names it", request_headers}};
static const gitea_edition gitea_edition_forgejo = {
    "forgejo", {"Forgejo", "check that the host runs Forgejo and the home names it", request_headers}};

static char api_root_override[FORGE_ROOT_SIZE];

#ifdef NAULA_TEST_HOOKS
void naula_gitea_use_api_root(const char *root) {
    if (root == NULL) {
        api_root_override[0] = '\0';
    } else {
        snprintf(api_root_override, sizeof api_root_override, "%s", root);
    }
}
#endif

static naula_status build_root(const gitea_target *at, char *root, naula_refusal *refusal) {
    if (at->host == NULL || at->host[0] == '\0') {
        naula_refuse(refusal, "host_needed", "write the home with the host it runs on, such as gitea@git.example.com",
                     "%s needs the host it runs on", at->edition->flavor.label);
        return NAULA_REFUSED;
    }
    if (api_root_override[0] != '\0') {
        snprintf(root, FORGE_ROOT_SIZE, "%s", api_root_override);
        return NAULA_OK;
    }
    if (!naula_forge_is_host_text(at->host)) {
        return naula_forge_refuse_input(&at->edition->flavor, refusal, "the host");
    }
    snprintf(root, FORGE_ROOT_SIZE, "https://%s" API_PATH, at->host);
    return NAULA_OK;
}

naula_status naula_gitea_request(const gitea_target *at, const char *method, const char *route, const cJSON *body,
                                 size_t capacity, forge_reply *reply, naula_refusal *refusal) {
    char root[FORGE_ROOT_SIZE];
    char url[URL_SIZE];
    memset(reply, 0, sizeof *reply);
    naula_status status = build_root(at, root, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    if (snprintf(url, sizeof url, "%s/%s", root, route) >= (int)sizeof url) {
        return naula_forge_refuse_input(&at->edition->flavor, refusal, "the request address (too long)");
    }
    return naula_forge_send(&at->edition->flavor, method, url, at->token, body, capacity, reply, refusal);
}

static naula_status get(const gitea_target *at, const char *route, size_t capacity, forge_reply *reply,
                        naula_refusal *refusal) {
    return naula_gitea_request(at, NULL, route, NULL, capacity, reply, refusal);
}

naula_repository naula_gitea_fresh_repository(void) {
    naula_repository repository;
    memset(&repository, 0, sizeof repository);
    repository.size = sizeof repository;
    return repository;
}

naula_status naula_gitea_lookup(const gitea_target *at, naula_repository *out, naula_refusal *refusal) {
    char route[FORGE_ROUTE_SIZE];
    forge_reply reply;
    const forge_flavor *flavor = &at->edition->flavor;
    if (!naula_forge_covers_first_shape(out) || at->number == 0) {
        return naula_forge_refuse_input(flavor, refusal, "the repository number or the result struct");
    }
    snprintf(route, sizeof route, routes.by_number, (unsigned long long)at->number);
    naula_status status = get(at, route, FORGE_SMALL_BODY_CAPACITY, &reply, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    cJSON *json = naula_forge_parse_reply(flavor, &reply, &status, refusal);
    uint64_t echoed = 0;
    if (json != NULL && !naula_forge_read_exact_number(json, "id", &echoed)) {
        status = naula_forge_refuse_malformed(flavor, refusal, "a repository with no number that can be read exactly");
    } else if (json != NULL && echoed != at->number) {
        naula_refuse(refusal, "number_mismatch", "check the number in the id",
                     "%s answered another repository than the one asked for (%s)", flavor->label, route);
        status = NAULA_REFUSED;
    } else if (json != NULL) {
        status = naula_forge_fill_repository(flavor, json, out, refusal);
    }
    if (status == NAULA_OK) {
        out->etag = reply.etag;
        reply.etag = NULL;
    }
    cJSON_Delete(json);
    naula_forge_reply_release(&reply);
    return status;
}

static naula_status gitea_lookup(void *context, const char *host, uint64_t number, const char *token,
                                 naula_repository *out, naula_refusal *refusal) {
    gitea_target at = {context, host, number, token};
    return naula_gitea_lookup(&at, out, refusal);
}

static naula_status gitea_find(void *context, const char *host, const char *account, const char *name,
                               const char *token, uint64_t *number, naula_refusal *refusal) {
    char route[FORGE_ROUTE_SIZE];
    forge_reply reply;
    naula_refusal absent;
    gitea_target at = {context, host, 0, token};
    const forge_flavor *flavor = &at.edition->flavor;
    if (!naula_forge_is_name_segment(account) || !naula_forge_is_name_segment(name)) {
        return naula_forge_refuse_input(flavor, refusal, "the account or the name");
    }
    *number = 0;
    memset(&absent, 0, sizeof absent);
    absent.size = sizeof absent;
    snprintf(route, sizeof route, routes.by_name, account, name);
    naula_status status = get(&at, route, FORGE_SMALL_BODY_CAPACITY, &reply, &absent);
    if (status == NAULA_NOT_FOUND) {
        return NAULA_OK;
    }
    if (status != NAULA_OK) {
        if (refusal != NULL) {
            *refusal = absent;
        }
        return status;
    }
    cJSON *json = naula_forge_parse_reply(flavor, &reply, &status, refusal);
    if (json != NULL && !naula_forge_read_exact_number(json, "id", number)) {
        status = naula_forge_refuse_malformed(flavor, refusal, "a repository with no number that can be read exactly");
    } else if (json != NULL && !naula_forge_answers_to_the_name(json, account, name)) {
        *number = 0;
    }
    cJSON_Delete(json);
    naula_forge_reply_release(&reply);
    return status;
}

int naula_gitea_repository_prefix(const naula_repository *repository, char *prefix, size_t capacity) {
    char account[NAME_SIZE];
    char name[NAME_SIZE];
    if (naula_forge_encode_text(repository->account, 0, account, sizeof account) == 0 ||
        naula_forge_encode_text(repository->name, 0, name, sizeof name) == 0) {
        return 0;
    }
    return snprintf(prefix, capacity, routes.by_name, account, name) < (int)capacity;
}

/**
 * A name-based route reads whatever repository holds the name when it is asked, so the repository is looked up by
 * number again afterwards and the answer is used only when it still has the names the route was built from.
 */
naula_status naula_gitea_confirm_names(const gitea_target *at, const naula_repository *before,
                                       naula_refusal *refusal) {
    naula_repository now = naula_gitea_fresh_repository();
    naula_status status = naula_gitea_lookup(at, &now, refusal);
    if (status == NAULA_OK && (strcmp(now.account, before->account) != 0 || strcmp(now.name, before->name) != 0)) {
        naula_refuse(refusal, "repository_changed", "try again, the repository was renamed or moved during the read",
                     "%s renamed the repository while it was read", at->edition->flavor.label);
        status = NAULA_REFUSED;
    }
    naula_repository_release(&now);
    return status;
}

/** A 404 on a file or a commit reads the same as a repository that is gone, so the repository decides which. */
naula_status naula_gitea_settle_read(const gitea_target *at, const naula_repository *before, naula_status status,
                                     const missing_answer *missing, naula_refusal *refusal) {
    if (status != NAULA_NOT_FOUND) {
        return status == NAULA_OK ? naula_gitea_confirm_names(at, before, refusal) : status;
    }
    status = naula_gitea_confirm_names(at, before, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    naula_refuse(refusal, missing->code, missing->fix, "the repository has no %s asked for", missing->what);
    return NAULA_REFUSED;
}

static const char *read_version_number(const char *text, unsigned long *number) {
    unsigned long value = 0;
    if (*text < '0' || *text > '9') {
        return NULL;
    }
    for (; *text >= '0' && *text <= '9'; text++) {
        if (value > VERSION_NUMBER_LIMIT) {
            return NULL;
        }
        value = value * 10 + (unsigned long)(*text - '0');
    }
    *number = value;
    return text;
}

/**
 * The Gitea version a host's API is, from its `version` answer. Forgejo from 7 on names its own version and the Gitea
 * it carries after "+gitea-" ("7.0.0+gitea-1.22.0"); Gitea, and Forgejo before 7 ("1.21.11-1"), lead with it.
 */
static int read_gitea_version(const char *version, unsigned long *major, unsigned long *minor) {
    const char *gitea_part = strstr(version, GITEA_PART);
    const char *at = read_version_number(gitea_part != NULL ? gitea_part + strlen(GITEA_PART) : version, major);
    return at != NULL && *at == '.' && read_version_number(at + 1, minor) != NULL;
}

static naula_status refuse_old_host(const forge_flavor *flavor, const char *answered, naula_refusal *refusal) {
    naula_refuse(refusal, "unsupported_operation", MINIMUM_FIX,
                 "%s answered %s for its version; naula reads hosts on Gitea 1.20 or later", flavor->label, answered);
    return NAULA_REFUSED;
}

naula_status naula_gitea_require_version(const gitea_target *at, naula_refusal *refusal) {
    forge_reply reply;
    unsigned long major = 0;
    unsigned long minor = 0;
    const forge_flavor *flavor = &at->edition->flavor;
    naula_status status = get(at, routes.version, FORGE_SMALL_BODY_CAPACITY, &reply, refusal);
    if (status == NAULA_NOT_FOUND) {
        return refuse_old_host(flavor, "404", refusal);
    }
    if (status != NAULA_OK) {
        return status;
    }
    cJSON *json = naula_forge_parse_reply(flavor, &reply, &status, refusal);
    const char *version = json == NULL ? NULL : naula_forge_text_member(json, "version");
    if (json != NULL && (version == NULL || !read_gitea_version(version, &major, &minor))) {
        status = naula_forge_refuse_malformed(flavor, refusal, "a version that cannot be read");
    } else if (json != NULL && (major < MINIMUM_MAJOR || (major == MINIMUM_MAJOR && minor < MINIMUM_MINOR))) {
        status = refuse_old_host(flavor, version, refusal);
    }
    cJSON_Delete(json);
    naula_forge_reply_release(&reply);
    return status;
}

static naula_status build_raw_route(const naula_repository *repository, const char *ref, const char *path, char *route,
                                    const forge_flavor *flavor, naula_refusal *refusal) {
    char prefix[FORGE_ROUTE_SIZE / 4];
    char encoded_path[FORGE_ROUTE_SIZE / 2];
    char encoded_ref[FORGE_ROUTE_SIZE / 4];
    if (path == NULL || path[0] == '\0' || naula_forge_encode_text(path, 1, encoded_path, sizeof encoded_path) == 0 ||
        !naula_gitea_repository_prefix(repository, prefix, sizeof prefix)) {
        return naula_forge_refuse_input(flavor, refusal, "the path");
    }
    if (ref == NULL) {
        snprintf(route, FORGE_ROUTE_SIZE, routes.raw_file, prefix, encoded_path);
        return NAULA_OK;
    }
    if (ref[0] == '\0' || naula_forge_encode_text(ref, 0, encoded_ref, sizeof encoded_ref) == 0) {
        return naula_forge_refuse_input(flavor, refusal, "the ref");
    }
    snprintf(route, FORGE_ROUTE_SIZE, routes.raw_file, prefix, encoded_path);
    size_t used = strlen(route);
    snprintf(route + used, FORGE_ROUTE_SIZE - used, "?ref=%s", encoded_ref);
    return NAULA_OK;
}

static naula_status keep_body(forge_reply *reply, uint8_t **bytes, size_t *length) {
    uint8_t *kept = naula_memory_reallocate(reply->body, reply->length == 0 ? 1 : reply->length);
    if (kept == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    *bytes = kept;
    *length = reply->length;
    reply->body = NULL;
    return NAULA_OK;
}

static naula_status read_raw(const gitea_target *at, const naula_repository *repository, const char *ref, const char *path,
                             uint8_t **bytes, size_t *length, naula_refusal *refusal) {
    char route[FORGE_ROUTE_SIZE];
    forge_reply reply;
    naula_status status = build_raw_route(repository, ref, path, route, &at->edition->flavor, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    status = get(at, route, FORGE_FILE_BODY_CAPACITY, &reply, refusal);
    if (status == NAULA_OK) {
        status = keep_body(&reply, bytes, length);
        naula_forge_reply_release(&reply);
    }
    return status;
}

static naula_status gitea_read_file(void *context, const char *host, uint64_t number, const char *ref,
                                    const char *path, const char *token, uint8_t **bytes, size_t *length,
                                    naula_refusal *refusal) {
    gitea_target at = {context, host, number, token};
    naula_repository repository = naula_gitea_fresh_repository();
    *bytes = NULL;
    *length = 0;
    naula_status status = naula_gitea_lookup(&at, &repository, refusal);
    if (status == NAULA_OK) {
        status = naula_gitea_require_version(&at, refusal);
    }
    if (status == NAULA_OK) {
        status = read_raw(&at, &repository, ref, path, bytes, length, refusal);
        status = naula_gitea_settle_read(&at, &repository, status, &missing_file, refusal);
    }
    if (status != NAULA_OK) {
        naula_memory_release(*bytes);
        *bytes = NULL;
        *length = 0;
    }
    naula_repository_release(&repository);
    return status;
}

static naula_status read_contained(const gitea_target *at, const char *prefix, const char *ref, const char *commit,
                                   int *contained, naula_refusal *refusal) {
    char route[FORGE_ROUTE_SIZE];
    char encoded_ref[FORGE_ROUTE_SIZE / 4];
    forge_reply reply;
    const forge_flavor *flavor = &at->edition->flavor;
    if (naula_forge_encode_text(ref, 0, encoded_ref, sizeof encoded_ref) == 0 ||
        snprintf(route, sizeof route, routes.commits_not_in, prefix, commit, encoded_ref) >= (int)sizeof route) {
        return naula_forge_refuse_input(flavor, refusal, "the commit or the ref");
    }
    naula_status status = get(at, route, FORGE_SMALL_BODY_CAPACITY, &reply, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    cJSON *json = naula_forge_parse_reply(flavor, &reply, &status, refusal);
    if (json != NULL && !cJSON_IsArray(json)) {
        status = naula_forge_refuse_malformed(flavor, refusal, "a commit list that is not a list");
    } else if (json != NULL) {
        *contained = cJSON_GetArraySize(json) == 0;
    }
    cJSON_Delete(json);
    naula_forge_reply_release(&reply);
    return status;
}

static int is_usable_ref(const char *ref) {
    return ref != NULL && ref[0] != '\0' && ref[0] != '-';
}

static naula_status gitea_contains(void *context, const char *host, uint64_t number, const char *ref,
                                   const char *commit, const char *token, int *contained, naula_refusal *refusal) {
    char prefix[FORGE_ROUTE_SIZE / 4];
    gitea_target at = {context, host, number, token};
    naula_repository repository = naula_gitea_fresh_repository();
    *contained = 0;
    if (number == 0 || commit == NULL || !naula_forge_is_commit_text(commit) || !is_usable_ref(ref)) {
        return naula_forge_refuse_input(&at.edition->flavor, refusal, "the number, the commit or the ref");
    }
    naula_status status = naula_gitea_lookup(&at, &repository, refusal);
    if (status == NAULA_OK && !naula_gitea_repository_prefix(&repository, prefix, sizeof prefix)) {
        status = naula_forge_refuse_input(&at.edition->flavor, refusal, "the repository names");
    }
    if (status == NAULA_OK) {
        status = naula_gitea_require_version(&at, refusal);
    }
    if (status == NAULA_OK) {
        status = read_contained(&at, prefix, ref, commit, contained, refusal);
        status = naula_gitea_settle_read(&at, &repository, status, &missing_commit_or_ref, refusal);
    }
    if (status != NAULA_OK) {
        *contained = 0;
    }
    naula_repository_release(&repository);
    return status;
}

static naula_status read_parent_count(const gitea_target *at, const char *prefix, const char *commit, size_t *count,
                                      naula_refusal *refusal) {
    char route[FORGE_ROUTE_SIZE];
    forge_reply reply;
    const forge_flavor *flavor = &at->edition->flavor;
    if (snprintf(route, sizeof route, routes.commit, prefix, commit) >= (int)sizeof route) {
        return naula_forge_refuse_input(flavor, refusal, "the commit");
    }
    naula_status status = get(at, route, FORGE_SMALL_BODY_CAPACITY, &reply, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    cJSON *json = naula_forge_parse_reply(flavor, &reply, &status, refusal);
    const cJSON *parents = json == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(json, "parents");
    if (json != NULL && !cJSON_IsArray(parents)) {
        status = naula_forge_refuse_malformed(flavor, refusal, "a commit with no parents list");
    } else if (parents != NULL) {
        *count = (size_t)cJSON_GetArraySize(parents);
    }
    cJSON_Delete(json);
    naula_forge_reply_release(&reply);
    return status;
}

static naula_status gitea_parent_count(void *context, const char *host, uint64_t number, const char *commit,
                                       const char *token, size_t *count, naula_refusal *refusal) {
    char prefix[FORGE_ROUTE_SIZE / 4];
    gitea_target at = {context, host, number, token};
    naula_repository repository = naula_gitea_fresh_repository();
    *count = 0;
    if (number == 0 || commit == NULL || !naula_forge_is_commit_text(commit)) {
        return naula_forge_refuse_input(&at.edition->flavor, refusal, "the number or the commit");
    }
    naula_status status = naula_gitea_lookup(&at, &repository, refusal);
    if (status == NAULA_OK && !naula_gitea_repository_prefix(&repository, prefix, sizeof prefix)) {
        status = naula_forge_refuse_input(&at.edition->flavor, refusal, "the repository names");
    }
    if (status == NAULA_OK) {
        status = naula_gitea_require_version(&at, refusal);
    }
    if (status == NAULA_OK) {
        status = read_parent_count(&at, prefix, commit, count, refusal);
        status = naula_gitea_settle_read(&at, &repository, status, &missing_commit, refusal);
    }
    if (status != NAULA_OK) {
        *count = 0;
    }
    naula_repository_release(&repository);
    return status;
}

static naula_status read_pages(const gitea_target *at, const char *format, const char *account,
                               forge_repository_list *list, naula_refusal *refusal) {
    for (unsigned page = 1; page <= PAGE_LIMIT; page++) {
        char route[FORGE_ROUTE_SIZE];
        forge_reply reply;
        size_t page_count = 0;
        snprintf(route, sizeof route, format, account, PAGE_SIZE, page);
        naula_status status = get(at, route, FORGE_HISTORY_BODY_CAPACITY, &reply, refusal);
        if (status == NAULA_OK) {
            status = naula_forge_append_page(&at->edition->flavor, list, &reply, &page_count, refusal);
            naula_forge_reply_release(&reply);
        }
        if (status != NAULA_OK || page_count == 0) {
            return status;
        }
    }
    naula_refuse(refusal, "too_many_repositories", "list a smaller account", "%s holds more repositories than are listed",
                 account);
    return NAULA_REFUSED;
}

static naula_status gitea_list_repos(void *context, const char *host, const char *account, const char *token,
                                     naula_repository **out, size_t *count, naula_refusal *refusal) {
    forge_repository_list list = {NULL, 0, 0};
    naula_refusal absent;
    gitea_target at = {context, host, 0, token};
    *out = NULL;
    *count = 0;
    if (!naula_forge_is_name_segment(account)) {
        return naula_forge_refuse_input(&at.edition->flavor, refusal, "the account");
    }
    memset(&absent, 0, sizeof absent);
    absent.size = sizeof absent;
    naula_status status = read_pages(&at, routes.organization_repositories, account, &list, &absent);
    if (status == NAULA_NOT_FOUND) {
        naula_repository_list_release(list.items, list.count);
        list = (forge_repository_list){NULL, 0, 0};
        status = read_pages(&at, routes.user_repositories, account, &list, refusal);
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

static naula_status read_release(const gitea_target *at, const naula_repository *repository, const char *tag,
                                 cJSON **release, naula_refusal *refusal) {
    char prefix[FORGE_ROUTE_SIZE / 4];
    char suffix[FORGE_ROUTE_SIZE / 2];
    char route[FORGE_ROUTE_SIZE];
    forge_reply reply;
    *release = NULL;
    if (!naula_gitea_repository_prefix(repository, prefix, sizeof prefix) ||
        !naula_forge_release_route(tag, suffix, sizeof suffix)) {
        return naula_forge_refuse_input(&at->edition->flavor, refusal, "the tag");
    }
    snprintf(route, sizeof route, "%s/%s", prefix, suffix);
    naula_status status = get(at, route, FORGE_SMALL_BODY_CAPACITY, &reply, refusal);
    if (status == NAULA_OK) {
        *release = naula_forge_parse_reply(&at->edition->flavor, &reply, &status, refusal);
        naula_forge_reply_release(&reply);
    }
    return status;
}

static int is_attachment_id(const char *text) {
    size_t length = text == NULL ? 0 : strlen(text);
    if (length == 0 || length > ATTACHMENT_ID_LIMIT) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        char character = text[index];
        int hex = (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
                  (character >= 'A' && character <= 'F');
        if (!hex && character != '-') {
            return 0;
        }
    }
    return 1;
}

/**
 * The asset by its attachment id on the host's web root, the one address Gitea serves an asset's bytes at that is not
 * by owner and name.
 */
static naula_status read_attachment(const gitea_target *at, const cJSON *asset, naula_release_index *out,
                                    naula_refusal *refusal) {
    char root[FORGE_ROOT_SIZE];
    char url[URL_SIZE];
    forge_reply reply;
    const forge_flavor *flavor = &at->edition->flavor;
    const char *attachment = naula_forge_text_member(asset, "uuid");
    if (!is_attachment_id(attachment)) {
        return naula_forge_refuse_malformed(flavor, refusal, "an asset with no attachment id");
    }
    naula_status status = build_root(at, root, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    size_t length = strlen(root);
    if (length >= strlen(API_PATH) && strcmp(root + length - strlen(API_PATH), API_PATH) == 0) {
        root[length - strlen(API_PATH)] = '\0';
    }
    snprintf(url, sizeof url, "%s/attachments/%s", root, attachment);
    status = naula_forge_fetch(flavor, url, at->token, FORGE_INDEX_BODY_CAPACITY, &reply, refusal);
    if (status == NAULA_OK) {
        status = naula_forge_keep_release_index(flavor, &reply, asset, out, refusal);
        naula_forge_reply_release(&reply);
    }
    return status;
}

static naula_status read_release_index(const gitea_target *at, const naula_repository *repository, const char *tag,
                                       const char *asset, naula_release_index *out, naula_refusal *refusal) {
    cJSON *release = NULL;
    const missing_answer *missing = &missing_release;
    naula_status status = read_release(at, repository, tag, &release, refusal);
    const cJSON *found =
        status == NAULA_OK ? naula_forge_release_asset(&at->edition->flavor, release, asset, &status, refusal) : NULL;
    if (found != NULL) {
        missing = &missing_index;
        status = read_attachment(at, found, out, refusal);
    }
    cJSON_Delete(release);
    return naula_gitea_settle_read(at, repository, status, missing, refusal);
}

static naula_status gitea_release_index(void *context, const char *host, uint64_t number, const char *tag,
                                        const char *asset, const char *token, naula_release_index *out,
                                        naula_refusal *refusal) {
    gitea_target at = {context, host, number, token};
    naula_repository repository = naula_gitea_fresh_repository();
    naula_status status = naula_forge_check_release_request(&at.edition->flavor, number, tag, asset, out, refusal);
    if (status == NAULA_OK) {
        status = naula_gitea_lookup(&at, &repository, refusal);
    }
    if (status == NAULA_OK) {
        status = naula_gitea_require_version(&at, refusal);
    }
    if (status == NAULA_OK) {
        status = read_release_index(&at, &repository, tag, asset, out, refusal);
    }
    if (status != NAULA_OK) {
        naula_release_index_release(out);
    }
    naula_repository_release(&repository);
    return status;
}

static naula_status gitea_describe(void *context, uint32_t *supported, int *anonymous_lookup) {
    (void)context;
    *supported = NAULA_OP_LOOKUP | NAULA_OP_FIND | NAULA_OP_READ_FILE | NAULA_OP_CONTAINS | NAULA_OP_PARENT_COUNT |
                 NAULA_OP_LIST_REPOS | NAULA_OP_RELEASE_INDEX | NAULA_OP_CREATE | NAULA_OP_RENAME | NAULA_OP_TRANSFER |
                 NAULA_OP_ARCHIVE | NAULA_OP_READ_METADATA | NAULA_OP_WRITE_METADATA;
    *anonymous_lookup = 0;
    return NAULA_OK;
}

#define GITEA_PROVIDER(word, edition)                                                                               \
    {sizeof(naula_provider), word,                 (void *)&(edition),   gitea_describe,                            \
     gitea_lookup,           gitea_find,           gitea_read_file,      gitea_contains,                            \
     gitea_parent_count,     gitea_list_repos,     gitea_release_index,  naula_gitea_create,                        \
     naula_gitea_rename,     naula_gitea_transfer, naula_gitea_archive,  naula_gitea_read_metadata,                 \
     naula_gitea_write_metadata}

static const naula_provider gitea_provider = GITEA_PROVIDER("gitea", gitea_edition_gitea);
static const naula_provider forgejo_provider = GITEA_PROVIDER("forgejo", gitea_edition_forgejo);

const naula_provider *naula_provider_gitea(void) {
    return &gitea_provider;
}

const naula_provider *naula_provider_forgejo(void) {
    return &forgejo_provider;
}
