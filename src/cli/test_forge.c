#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "cli.h"
#include "naula/git.h"
#include "naula/json.h"
#include "naula/memory.h"

/**
 * A provider named `forge` whose repositories a test describes in the JSON file `NAULA_TEST_FORGE`: `{ "repos": [
 * { "number", "account", "name", "path", "default_branch"?, "archived"?, "fork_parent"?, "gone"?, "topics"? } ] }`,
 * each served from the local git repository at `path`. It lets a test drive the naula program end to end with no
 * host, and exists only in a test build.
 */
static cJSON *forge;

static const cJSON *repos_of(void) {
    return cJSON_GetObjectItemCaseSensitive(forge, "repos");
}

static const char *text_of(const cJSON *repo, const char *name) {
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(repo, name);
    return cJSON_IsString(member) ? member->valuestring : NULL;
}

static uint64_t number_of(const cJSON *repo, const char *name) {
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(repo, name);
    return cJSON_IsNumber(member) ? (uint64_t)member->valuedouble : 0;
}

static int flag_of(const cJSON *repo, const char *name) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(repo, name));
}

static naula_status refuse(naula_refusal *refusal, naula_status status, const char *code, const char *detail) {
    naula_refuse(refusal, code, "fix the test forge", "the test forge answered %s", detail);
    return status;
}

static const cJSON *by_number(uint64_t number, naula_refusal *refusal, naula_status *status) {
    const cJSON *repo = NULL;
    cJSON_ArrayForEach(repo, repos_of()) {
        if (number_of(repo, "number") == number) {
            *status = flag_of(repo, "gone") ? refuse(refusal, NAULA_NOT_FOUND, "not_found", "gone") : NAULA_OK;
            return *status == NAULA_OK ? repo : NULL;
        }
    }
    *status = refuse(refusal, NAULA_NOT_FOUND, "not_found", "no such number");
    return NULL;
}

static char *copy(const char *text) {
    return text == NULL ? NULL : naula_memory_duplicate_text(text);
}

static char *clone_url_of(const cJSON *repo) {
    const char *path = text_of(repo, "path");
    return path == NULL ? NULL : cli_format("file://%s%s", path[0] == '/' ? "" : "/", path);
}

static naula_status fill(const cJSON *repo, naula_repository *out) {
    uint32_t size = out->size;
    memset(out, 0, sizeof *out);
    out->size = size;
    out->number = number_of(repo, "number");
    out->fork_parent = number_of(repo, "fork_parent");
    out->archived = flag_of(repo, "archived");
    const char *branch = text_of(repo, "default_branch");
    out->account = copy(text_of(repo, "account"));
    out->name = copy(text_of(repo, "name"));
    out->default_branch = copy(branch == NULL ? "main" : branch);
    out->clone_url = clone_url_of(repo);
    if (out->account == NULL || out->name == NULL || out->default_branch == NULL || out->clone_url == NULL) {
        naula_repository_release(out);
        return NAULA_OUT_OF_MEMORY;
    }
    return NAULA_OK;
}

static naula_status forge_describe(void *context, uint32_t *supported, int *anonymous_lookup) {
    (void)context;
    *supported = NAULA_OP_LOOKUP | NAULA_OP_FIND | NAULA_OP_READ_FILE | NAULA_OP_LIST_REPOS | NAULA_OP_READ_METADATA;
    *anonymous_lookup = 1;
    return NAULA_OK;
}

static naula_status forge_lookup(void *context, const char *host, uint64_t number, const char *token,
                                 naula_repository *out, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)token;
    naula_status status = NAULA_OK;
    const cJSON *repo = by_number(number, refusal, &status);
    return repo == NULL ? status : fill(repo, out);
}

static naula_status forge_find(void *context, const char *host, const char *account, const char *name,
                               const char *token, uint64_t *number, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)token;
    (void)refusal;
    *number = 0;
    const cJSON *repo = NULL;
    cJSON_ArrayForEach(repo, repos_of()) {
        const char *each_account = text_of(repo, "account");
        const char *each_name = text_of(repo, "name");
        if (!flag_of(repo, "gone") && each_account != NULL && each_name != NULL && strcmp(each_account, account) == 0 &&
            strcmp(each_name, name) == 0) {
            *number = number_of(repo, "number");
        }
    }
    return NAULA_OK;
}

static naula_status forge_read_file(void *context, const char *host, uint64_t number, const char *ref,
                                    const char *path, const char *token, uint8_t **bytes, size_t *length,
                                    naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)token;
    *bytes = NULL;
    *length = 0;
    naula_status status = NAULA_OK;
    const cJSON *repo = by_number(number, refusal, &status);
    if (repo == NULL) {
        return status;
    }
    char *object = cli_format("%s:%s", ref == NULL ? "HEAD" : ref, path);
    if (object == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    const char *arguments[] = {"cat-file", "blob", object, NULL};
    naula_run_result result;
    status = naula_git_run(text_of(repo, "path"), arguments, NULL, 0, &result, refusal);
    naula_memory_release(object);
    if (status != NAULA_OK) {
        return status;
    }
    if (result.exit_code != 0) {
        naula_run_result_release(&result);
        return refuse(refusal, NAULA_REFUSED, "missing_file_or_ref", "no such file or ref");
    }
    *bytes = (uint8_t *)result.output;
    *length = result.output_length;
    result.output = NULL;
    naula_run_result_release(&result);
    return NAULA_OK;
}

static naula_status forge_unsupported(naula_refusal *refusal) {
    return refuse(refusal, NAULA_REFUSED, "unsupported_operation", "an operation it lacks");
}

static naula_status forge_contains(void *context, const char *host, uint64_t number, const char *ref,
                                   const char *commit, const char *token, int *contained, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)number;
    (void)ref;
    (void)commit;
    (void)token;
    *contained = 0;
    return forge_unsupported(refusal);
}

static naula_status forge_parent_count(void *context, const char *host, uint64_t number, const char *commit,
                                       const char *token, size_t *count, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)number;
    (void)commit;
    (void)token;
    *count = 0;
    return forge_unsupported(refusal);
}

static naula_status forge_list_repos(void *context, const char *host, const char *account, const char *token,
                                     naula_repository **out, size_t *count, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)token;
    (void)refusal;
    *out = NULL;
    *count = 0;
    size_t total = (size_t)cJSON_GetArraySize(repos_of());
    naula_repository *list = naula_memory_allocate((total + 1) * sizeof *list);
    if (list == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    size_t made = 0;
    const cJSON *repo = NULL;
    cJSON_ArrayForEach(repo, repos_of()) {
        const char *each_account = text_of(repo, "account");
        if (flag_of(repo, "gone") || each_account == NULL || strcmp(each_account, account) != 0) {
            continue;
        }
        list[made].size = sizeof list[made];
        if (fill(repo, &list[made]) != NAULA_OK) {
            naula_repository_list_release(list, made);
            return NAULA_OUT_OF_MEMORY;
        }
        made++;
    }
    *out = list;
    *count = made;
    return NAULA_OK;
}

static naula_status forge_release_index(void *context, const char *host, uint64_t number, const char *tag,
                                        const char *asset, const char *token, naula_release_index *out,
                                        naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)number;
    (void)tag;
    (void)asset;
    (void)token;
    (void)out;
    return forge_unsupported(refusal);
}

static naula_status forge_create(void *context, const char *host, const char *account, const char *name,
                                 const naula_metadata *metadata, const char *token, uint64_t *number,
                                 naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)account;
    (void)name;
    (void)metadata;
    (void)token;
    *number = 0;
    return forge_unsupported(refusal);
}

static naula_status forge_rename(void *context, const char *host, uint64_t number, const char *name,
                                 const char *token, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)number;
    (void)name;
    (void)token;
    return forge_unsupported(refusal);
}

static naula_status forge_transfer(void *context, const char *host, uint64_t number, const char *account,
                                   const char *token, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)number;
    (void)account;
    (void)token;
    return forge_unsupported(refusal);
}

static naula_status forge_archive(void *context, const char *host, uint64_t number, const char *token,
                                  naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)number;
    (void)token;
    return forge_unsupported(refusal);
}

static naula_status forge_read_metadata(void *context, const char *host, uint64_t number, const char *token,
                                        naula_metadata *out, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)token;
    naula_status status = NAULA_OK;
    const cJSON *repo = by_number(number, refusal, &status);
    if (repo == NULL) {
        return status;
    }
    uint32_t size = out->size;
    memset(out, 0, sizeof *out);
    out->size = size;
    out->present = NAULA_METADATA_TOPICS;
    const cJSON *topics = cJSON_GetObjectItemCaseSensitive(repo, "topics");
    size_t total = (size_t)cJSON_GetArraySize(topics);
    out->topics = total == 0 ? NULL : naula_memory_allocate(total * sizeof *out->topics);
    if (total > 0 && out->topics == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    const cJSON *topic = NULL;
    cJSON_ArrayForEach(topic, topics) {
        if (cJSON_IsString(topic)) {
            out->topics[out->topic_count] = copy(topic->valuestring);
            if (out->topics[out->topic_count] == NULL) {
                naula_metadata_release(out);
                return NAULA_OUT_OF_MEMORY;
            }
            out->topic_count++;
        }
    }
    return NAULA_OK;
}

static naula_status forge_write_metadata(void *context, const char *host, uint64_t number,
                                         const naula_metadata *wanted, const char *token, uint32_t *applied,
                                         naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)number;
    (void)wanted;
    (void)token;
    *applied = 0;
    return forge_unsupported(refusal);
}

static const naula_provider forge_provider = {
    sizeof(naula_provider), "forge",          NULL,           forge_describe,      forge_lookup,
    forge_find,             forge_read_file,  forge_contains, forge_parent_count,  forge_list_repos,
    forge_release_index,    forge_create,     forge_rename,   forge_transfer,      forge_archive,
    forge_read_metadata,    forge_write_metadata,
};

naula_status cli_test_forge_register(naula_providers *providers, naula_refusal *refusal) {
    char *path = cli_environment("NAULA_TEST_FORGE");
    if (path == NULL) {
        return NAULA_OK;
    }
    uint8_t *bytes = NULL;
    size_t length = 0;
    naula_status status = forge != NULL ? NAULA_OK : cli_read_file(path, &bytes, &length, refusal);
    naula_memory_release(path);
    if (status == NAULA_OK && forge == NULL) {
        const char *reason = NULL;
        status = naula_json_parse(bytes, length, &forge, &reason);
        if (status != NAULA_OK) {
            naula_refuse(refusal, "test-forge-invalid", "write NAULA_TEST_FORGE as JSON", "%s",
                         reason == NULL ? "not JSON" : reason);
        }
    }
    naula_memory_release(bytes);
    return status == NAULA_OK ? naula_providers_register(providers, &forge_provider, refusal) : status;
}

