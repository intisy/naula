#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "cli.h"
#include "disk.h"
#include "naula/git.h"
#include "naula/json.h"
#include "naula/memory.h"

/**
 * A provider named `forge` whose repositories a test describes in the JSON file `NAULA_TEST_FORGE`: `{ "repos": [
 * { "number", "account", "name", "path", "default_branch"?, "archived"?, "fork_parent"?, "gone"?, "topics"?,
 * "timed_out"?, "refuse_rename"? } ], "log"?: [] }`, each served from the local git repository at `path`. Every write
 * it takes is applied to the file and appended to `log`, and a repo it creates is a bare repository beside the file.
 * It lets a test drive the naula program end to end with no host, and exists only in a test build.
 */
static cJSON *forge;
static char *forge_path;

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
            *status = flag_of(repo, "gone")        ? refuse(refusal, NAULA_NOT_FOUND, "not_found", "gone")
                      : flag_of(repo, "timed_out") ? refuse(refusal, NAULA_TIMED_OUT, "timed_out", "too slowly")
                                                   : NAULA_OK;
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
    *supported = NAULA_OP_LOOKUP | NAULA_OP_FIND | NAULA_OP_READ_FILE | NAULA_OP_LIST_REPOS | NAULA_OP_READ_METADATA |
                 NAULA_OP_CREATE | NAULA_OP_RENAME | NAULA_OP_TRANSFER | NAULA_OP_ARCHIVE | NAULA_OP_WRITE_METADATA;
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
    *number = 0;
    const cJSON *repo = NULL;
    cJSON_ArrayForEach(repo, repos_of()) {
        const char *each_account = text_of(repo, "account");
        const char *each_name = text_of(repo, "name");
        if (!flag_of(repo, "gone") && each_account != NULL && each_name != NULL && strcmp(each_account, account) == 0 &&
            strcmp(each_name, name) == 0) {
            if (flag_of(repo, "timed_out")) {
                return refuse(refusal, NAULA_TIMED_OUT, "timed_out", "too slowly");
            }
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

/** Appends one line to the log and writes the whole forge back, so the next run and the test both see the write. */
static naula_status keep(const char *format, ...) {
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    cJSON *log = cJSON_GetObjectItemCaseSensitive(forge, "log");
    if (log == NULL) {
        log = cJSON_AddArrayToObject(forge, "log");
    }
    cJSON *entry = cJSON_CreateString(line);
    if (log == NULL || entry == NULL || !cJSON_AddItemToArray(log, entry)) {
        cJSON_Delete(entry);
        return NAULA_OUT_OF_MEMORY;
    }
    char *text = cJSON_PrintUnformatted(forge);
    const char *slash = strrchr(forge_path, '/');
    char *folder = text == NULL || slash == NULL ? NULL : cli_format("%.*s", (int)(slash - forge_path), forge_path);
    unsigned long error = 0;
    naula_status status = folder == NULL ? NAULA_OUT_OF_MEMORY
                                         : disk_file_replace(folder, slash + 1, (const uint8_t *)text, strlen(text),
                                                             DISK_FOR_NAULA_FILE, &error);
    naula_memory_release(folder);
    cJSON_free(text);
    return status;
}

static cJSON *writable(uint64_t number, naula_refusal *refusal, naula_status *status) {
    return (cJSON *)by_number(number, refusal, status);
}

static int name_is_taken(const char *account, const char *name) {
    const cJSON *repo = NULL;
    cJSON_ArrayForEach(repo, repos_of()) {
        const char *each_account = text_of(repo, "account");
        const char *each_name = text_of(repo, "name");
        if (!flag_of(repo, "gone") && each_account != NULL && each_name != NULL && strcmp(each_account, account) == 0 &&
            strcmp(each_name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static naula_status set_member(cJSON *repo, const char *name, cJSON *made) {
    if (made == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    if (cJSON_GetObjectItemCaseSensitive(repo, name) != NULL) {
        cJSON_ReplaceItemInObjectCaseSensitive(repo, name, made);
    } else if (!cJSON_AddItemToObject(repo, name, made)) {
        cJSON_Delete(made);
        return NAULA_OUT_OF_MEMORY;
    }
    return NAULA_OK;
}

static naula_status make_bare(const char *path, naula_refusal *refusal) {
    const char *initialise[] = {"init", "--quiet", "--bare", path, NULL};
    const char *allow[] = {"config", "uploadpack.allowFilter", "true", NULL};
    naula_run_result result;
    naula_status status = naula_git_run(NULL, initialise, NULL, 0, &result, refusal);
    if (status == NAULA_OK) {
        naula_run_result_release(&result);
        status = naula_git_run(path, allow, NULL, 0, &result, refusal);
    }
    if (status == NAULA_OK) {
        naula_run_result_release(&result);
    }
    return status;
}

static naula_status forge_create(void *context, const char *host, const char *account, const char *name,
                                 const naula_metadata *metadata, const char *token, uint64_t *number,
                                 naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)metadata;
    (void)token;
    *number = 0;
    if (name_is_taken(account, name)) {
        return refuse(refusal, NAULA_REFUSED, "name_taken", "the name is taken");
    }
    uint64_t next = 900;
    const cJSON *each = NULL;
    cJSON_ArrayForEach(each, repos_of()) {
        next = number_of(each, "number") >= next ? number_of(each, "number") + 1 : next;
    }
    const char *slash = strrchr(forge_path, '/');
    char *path = cli_format("%.*s/created-%llu.git", (int)(slash - forge_path), forge_path, (unsigned long long)next);
    if (path == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    naula_status status = make_bare(path, refusal);
    cJSON *repo = status == NAULA_OK ? cJSON_CreateObject() : NULL;
    if (status == NAULA_OK &&
        (repo == NULL || !cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(forge, "repos"), repo) ||
         set_member(repo, "number", cJSON_CreateNumber((double)next)) != NAULA_OK ||
         set_member(repo, "account", cJSON_CreateString(account)) != NAULA_OK ||
         set_member(repo, "name", cJSON_CreateString(name)) != NAULA_OK ||
         set_member(repo, "path", cJSON_CreateString(path)) != NAULA_OK)) {
        status = NAULA_OUT_OF_MEMORY;
    }
    naula_memory_release(path);
    if (status == NAULA_OK) {
        *number = next;
        status = keep("create %llu %s/%s", (unsigned long long)next, account, name);
    }
    return status;
}

static naula_status forge_rename(void *context, const char *host, uint64_t number, const char *name,
                                 const char *token, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)token;
    naula_status status = NAULA_OK;
    cJSON *repo = writable(number, refusal, &status);
    if (repo == NULL) {
        return status;
    }
    if (flag_of(repo, "refuse_rename")) {
        return refuse(refusal, NAULA_REFUSED, "refused_by_validation", "422 for the rename");
    }
    if (strcmp(text_of(repo, "name"), name) != 0 && name_is_taken(text_of(repo, "account"), name)) {
        return refuse(refusal, NAULA_REFUSED, "name_taken", "the name is taken");
    }
    status = set_member(repo, "name", cJSON_CreateString(name));
    return status == NAULA_OK ? keep("rename %llu %s", (unsigned long long)number, name) : status;
}

static naula_status forge_transfer(void *context, const char *host, uint64_t number, const char *account,
                                   const char *token, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)token;
    naula_status status = NAULA_OK;
    cJSON *repo = writable(number, refusal, &status);
    if (repo == NULL) {
        return status;
    }
    if (name_is_taken(account, text_of(repo, "name"))) {
        return refuse(refusal, NAULA_REFUSED, "name_taken", "the name is taken");
    }
    status = set_member(repo, "account", cJSON_CreateString(account));
    return status == NAULA_OK ? keep("transfer %llu %s", (unsigned long long)number, account) : status;
}

static naula_status forge_archive(void *context, const char *host, uint64_t number, const char *token,
                                  naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)token;
    naula_status status = NAULA_OK;
    cJSON *repo = writable(number, refusal, &status);
    if (repo == NULL) {
        return status;
    }
    status = set_member(repo, "archived", cJSON_CreateTrue());
    return status == NAULA_OK ? keep("archive %llu", (unsigned long long)number) : status;
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

/** Writes topics alone, the one member the forge shows, and leaves every other member out of `applied`. */
static naula_status forge_write_metadata(void *context, const char *host, uint64_t number,
                                         const naula_metadata *wanted, const char *token, uint32_t *applied,
                                         naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)token;
    *applied = 0;
    naula_status status = NAULA_OK;
    cJSON *repo = writable(number, refusal, &status);
    if (repo == NULL || !(wanted->present & NAULA_METADATA_TOPICS)) {
        return status;
    }
    cJSON *topics = cJSON_CreateArray();
    for (size_t index = 0; topics != NULL && index < wanted->topic_count; index++) {
        cJSON *topic = cJSON_CreateString(wanted->topics[index]);
        if (topic == NULL || !cJSON_AddItemToArray(topics, topic)) {
            cJSON_Delete(topic);
            cJSON_Delete(topics);
            topics = NULL;
        }
    }
    status = set_member(repo, "topics", topics);
    if (status == NAULA_OK) {
        *applied = NAULA_METADATA_TOPICS;
        status = keep("topics %llu %zu", (unsigned long long)number, wanted->topic_count);
    }
    return status;
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
    if (forge_path == NULL) {
        forge_path = path;
    } else {
        naula_memory_release(path);
    }
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


int cli_test_forge_repo_for_url(const char *url, char **account, char **name) {
    *account = NULL;
    *name = NULL;
    const cJSON *repo = NULL;
    cJSON_ArrayForEach(repo, repos_of()) {
        char *clone_url = clone_url_of(repo);
        int same = clone_url != NULL && strcmp(clone_url, url) == 0 && !flag_of(repo, "gone");
        naula_memory_release(clone_url);
        if (same) {
            *account = copy(text_of(repo, "account"));
            *name = copy(text_of(repo, "name"));
            return *account != NULL && *name != NULL;
        }
    }
    return 0;
}
