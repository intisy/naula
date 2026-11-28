#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "disk.h"
#include "naula/git.h"
#include "naula/json.h"
#include "naula/memory.h"
#include "naula/reconcile.h"
#include "naula/sha256.h"
#include "naula/repo_file.h"
#include "resolve_internal.h"

#define HISTORY_LIMIT 64
#define HINT_LIMIT 32
#define LINE_SIZE 1400

static const char *const TEMPORARY_REFS = "refs/naula-reconcile/";

typedef struct home_hint {
    char *provider;
    char *host;
    char *account;
    uint64_t number;
} home_hint;

/** A repo a host holds that shows it is this one: its number, what the provider says, and its own repo.json. */
typedef struct found_home {
    uint64_t number;
    naula_repository repository;
    char *branch;
    naula_repo_file file;
    int has_file;
} found_home;

/** One home of the file, and where it was found: under its account, under `old_account` (a transfer), or nowhere. */
typedef struct planned_home {
    const naula_home *wanted;
    const naula_provider *provider;
    found_home found;
    char *old_account;
    int failed;
} planned_home;

typedef struct reconciler {
    naula_session *session;
    const char *repository;
    const naula_reconcile_options *options;
    naula_reconcile_report *report;
    char id_text[80];
    char commit[65];
    int id_is_root;
    naula_repo_file *history;
    size_t history_count;
    home_hint hints[HINT_LIMIT];
    size_t hint_count;
    char hint_name[160];
    found_home serving;
    const naula_provider *serving_provider;
    char *serving_word;
    char *serving_host;
    char tip[65];
    naula_repo_file file;
    planned_home *planned;
    size_t planned_count;
    int refs_fetched;
} reconciler;

static void say(const reconciler *subject, const char *format, ...) {
    if (subject->options == NULL || subject->options->say == NULL) {
        return;
    }
    char line[LINE_SIZE];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    subject->options->say(subject->options->context, line);
}

static naula_refusal fresh_refusal(void) {
    naula_refusal refusal;
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    return refusal;
}

static void record_failure(reconciler *subject, const naula_refusal *refusal) {
    if (subject->report->failed++ == 0) {
        subject->report->first_failure = *refusal;
    }
    say(subject, "warning %s: %s", refusal->code, refusal->detail);
}

static int same_text(const char *left, const char *right) {
    return naula_resolver_equals_ignoring_case(left, right);
}

static int is_unheard(naula_status status) {
    return status != NAULA_OK && status != NAULA_NOT_FOUND && status != NAULA_OUT_OF_MEMORY;
}

static char *place_text(const char *word, const char *host, uint64_t number) {
    naula_place place;
    memset(&place, 0, sizeof place);
    place.size = sizeof place;
    place.kind = NAULA_HOME_PROVIDER;
    place.provider = (char *)word;
    place.host = (char *)host;
    place.number = number;
    return naula_place_text(&place);
}

static int url_is_allowed(const reconciler *subject, const char *url) {
    if (url == NULL) {
        return 0;
    }
    if (subject->options != NULL && subject->options->url_is_allowed != NULL) {
        return subject->options->url_is_allowed(url);
    }
    return strncmp(url, "https://", 8) == 0 && url[8] != '\0';
}

/** git's standard output when it exits 0, trailing line ends removed; a git that fails is refused `git-failed`. */
static naula_status git_output(const char *repository, const char *const *arguments, char **output,
                               naula_refusal *refusal) {
    naula_run_result result;
    *output = NULL;
    naula_status status = naula_git_run(repository, arguments, NULL, 0, &result, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    if (result.exit_code != 0) {
        size_t length = strlen(result.error);
        naula_refuse(refusal, "git-failed", "run the same git command by hand to see why", "git %s exited %d: %.*s",
                     arguments[0], result.exit_code, (int)(length > 600 ? 600 : length), result.error);
        naula_run_result_release(&result);
        return NAULA_REFUSED;
    }
    size_t length = result.output_length;
    while (length > 0 && (result.output[length - 1] == '\n' || result.output[length - 1] == '\r')) {
        result.output[--length] = '\0';
    }
    *output = result.output;
    result.output = NULL;
    naula_run_result_release(&result);
    return NAULA_OK;
}

static int git_succeeds(const char *repository, const char *const *arguments) {
    naula_run_result result;
    naula_refusal refusal = fresh_refusal();
    if (naula_git_run(repository, arguments, NULL, 0, &result, &refusal) != NAULA_OK) {
        return 0;
    }
    int succeeded = result.exit_code == 0;
    naula_run_result_release(&result);
    return succeeded;
}

/** repo.json at `commit` of the local repository, `present` 0 when that commit holds none. */
static naula_status file_at(const char *repository, const char *commit, naula_repo_file *file, int *present,
                            naula_refusal *refusal) {
    memset(file, 0, sizeof *file);
    file->size = sizeof *file;
    *present = 0;
    char object[128];
    snprintf(object, sizeof object, "%s:repo.json", commit);
    const char *arguments[] = {"cat-file", "blob", object, NULL};
    naula_run_result result;
    naula_status status = naula_git_run(repository, arguments, NULL, 0, &result, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    if (result.exit_code == 0) {
        naula_refusal ignored = fresh_refusal();
        status = naula_repo_file_parse((const uint8_t *)result.output, result.output_length, file, &ignored);
        *present = status == NAULA_OK;
        status = status == NAULA_OUT_OF_MEMORY ? status : NAULA_OK;
    }
    naula_run_result_release(&result);
    return status;
}

static void release_history(reconciler *subject) {
    for (size_t index = 0; index < subject->history_count; index++) {
        naula_repo_file_release(&subject->history[index]);
    }
    naula_memory_release(subject->history);
    subject->history = NULL;
    subject->history_count = 0;
}

/** Every format 2 repo.json the first-parent line of `ref` held, newest first: the names and homes the repo had. */
static naula_status read_history(reconciler *subject, const char *ref, naula_refusal *refusal) {
    release_history(subject);
    char limit[16];
    snprintf(limit, sizeof limit, "%d", HISTORY_LIMIT);
    const char *arguments[] = {"log", "--first-parent", "--format=%H", "-n", limit, ref, "--", "repo.json", NULL};
    char *output = NULL;
    naula_status status = git_output(subject->repository, arguments, &output, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    subject->history = naula_memory_allocate(HISTORY_LIMIT * sizeof *subject->history);
    status = subject->history == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    for (char *line = output; status == NAULA_OK && line != NULL && *line != '\0';) {
        char *end = strchr(line, '\n');
        if (end != NULL) {
            *end = '\0';
        }
        size_t length = strcspn(line, "\r");
        line[length] = '\0';
        int present = 0;
        naula_refusal ignored = fresh_refusal();
        naula_repo_file *file = &subject->history[subject->history_count];
        naula_status read = file_at(subject->repository, line, file, &present, &ignored);
        if (read == NAULA_OUT_OF_MEMORY) {
            status = read;
        } else if (present && subject->history_count < HISTORY_LIMIT) {
            subject->history_count++;
        } else {
            naula_repo_file_release(file);
        }
        line = end == NULL ? NULL : end + 1;
    }
    naula_memory_release(output);
    return status;
}

static void release_found(found_home *found) {
    naula_repository_release(&found->repository);
    naula_memory_release(found->branch);
    naula_repo_file_release(&found->file);
    memset(found, 0, sizeof *found);
}

static char *hint_path(const reconciler *subject) {
    if (subject->options == NULL || subject->options->cache_directory == NULL) {
        return NULL;
    }
    size_t length = strlen(subject->options->cache_directory) + sizeof subject->hint_name + 16;
    char *path = naula_memory_allocate(length);
    if (path != NULL) {
        snprintf(path, length, "%s/homes/%s.json", subject->options->cache_directory, subject->hint_name);
    }
    return path;
}

static char *copy_or_null(const char *text) {
    return text == NULL ? NULL : naula_memory_duplicate_text(text);
}

static void read_hints(reconciler *subject) {
    char *path = hint_path(subject);
    uint8_t *bytes = NULL;
    size_t length = 0;
    unsigned long error = 0;
    cJSON *json = NULL;
    const char *reason = NULL;
    if (path != NULL && disk_file_read(path, &bytes, &length, &error) == NAULA_OK &&
        naula_json_parse(bytes, length, &json, &reason) == NAULA_OK) {
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(json, "homes")) {
            const cJSON *provider = cJSON_GetObjectItemCaseSensitive(entry, "provider");
            const cJSON *host = cJSON_GetObjectItemCaseSensitive(entry, "host");
            const cJSON *account = cJSON_GetObjectItemCaseSensitive(entry, "account");
            const cJSON *number = cJSON_GetObjectItemCaseSensitive(entry, "number");
            if (subject->hint_count == HINT_LIMIT || !cJSON_IsString(provider) || !cJSON_IsString(account) ||
                !cJSON_IsNumber(number) || number->valuedouble < 1) {
                continue;
            }
            home_hint *hint = &subject->hints[subject->hint_count];
            hint->provider = copy_or_null(provider->valuestring);
            hint->host = cJSON_IsString(host) ? copy_or_null(host->valuestring) : NULL;
            hint->account = copy_or_null(account->valuestring);
            hint->number = (uint64_t)number->valuedouble;
            subject->hint_count += hint->provider != NULL && hint->account != NULL;
        }
    }
    cJSON_Delete(json);
    naula_memory_release(bytes);
    naula_memory_release(path);
}

static uint64_t hint_for(const reconciler *subject, const char *word, const char *host, const char *account) {
    for (size_t index = 0; index < subject->hint_count; index++) {
        const home_hint *hint = &subject->hints[index];
        if (strcmp(hint->provider, word) == 0 && same_text(hint->host, host) && same_text(hint->account, account)) {
            return hint->number;
        }
    }
    return 0;
}

static naula_status add_hint(cJSON *homes, const char *word, const char *host, const char *account, uint64_t number) {
    cJSON *entry = cJSON_CreateObject();
    if (entry == NULL || !cJSON_AddItemToArray(homes, entry) || cJSON_AddStringToObject(entry, "provider", word) == NULL ||
        (host != NULL && cJSON_AddStringToObject(entry, "host", host) == NULL) ||
        cJSON_AddStringToObject(entry, "account", account) == NULL ||
        cJSON_AddNumberToObject(entry, "number", (double)number) == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    return NAULA_OK;
}

/** Keeps where each home was found for the next run; a hint that cannot be kept only costs that run a search. */
static void write_hints(const reconciler *subject) {
    if (subject->options == NULL || subject->options->cache_directory == NULL) {
        return;
    }
    cJSON *json = cJSON_CreateObject();
    cJSON *homes = json == NULL ? NULL : cJSON_AddArrayToObject(json, "homes");
    naula_status status = homes == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    for (size_t index = 0; status == NAULA_OK && index < subject->planned_count; index++) {
        const planned_home *home = &subject->planned[index];
        if (home->found.number != 0 && !home->failed) {
            status = add_hint(homes, home->wanted->provider, home->wanted->host, home->wanted->account,
                              home->found.number);
        }
        if (status == NAULA_OK && home->found.number != 0 && home->old_account != NULL) {
            status = add_hint(homes, home->wanted->provider, home->wanted->host, home->old_account,
                              home->found.number);
        }
    }
    char *text = status == NAULA_OK ? cJSON_PrintUnformatted(json) : NULL;
    const char *cache = subject->options->cache_directory;
    char below[sizeof subject->hint_name + 16];
    snprintf(below, sizeof below, "homes/%s.json", subject->hint_name);
    disk_kind kind = DISK_MISSING;
    unsigned long error = 0;
    char *folder = text == NULL ? NULL : naula_memory_allocate(strlen(cache) + 8);
    if (folder != NULL) {
        snprintf(folder, strlen(cache) + 8, "%s/homes", cache);
        if (disk_kind_of(folder, &kind, &error) == NAULA_OK && kind == DISK_MISSING) {
            disk_folder_make(cache, "homes", DISK_FOR_NAULA_FILE, &error);
        }
        disk_file_replace(cache, below, (const uint8_t *)text, strlen(text), DISK_FOR_NAULA_FILE, &error);
    }
    naula_memory_release(folder);
    cJSON_free(text);
    cJSON_Delete(json);
}

/**
 * Whether the repo at `number` is this one, the resolver's way: its working branch holds the id's commit, and the
 * commit has no parents or the repo's own repo.json states the id; a repo stating another id is another repo.
 */
static naula_status examine(reconciler *subject, const naula_provider *provider, const char *word, const char *host,
                            uint64_t number, found_home *found, int *ours, naula_refusal *refusal) {
    *ours = 0;
    memset(found, 0, sizeof *found);
    found->repository.size = sizeof found->repository;
    found->file.size = sizeof found->file;
    found->number = number;
    const char *token = naula_session_token(subject->session, word, host);
    naula_status status = provider->lookup(provider->context, host, number, token, &found->repository, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    naula_place place;
    memset(&place, 0, sizeof place);
    place.size = sizeof place;
    place.kind = NAULA_HOME_PROVIDER;
    place.provider = (char *)word;
    place.host = (char *)host;
    place.number = number;
    const naula_history *history = &subject->session->history;
    int contained = 0;
    const char *branch = "development";
    status = history->contains(history->context, &place, branch, subject->commit, &contained, refusal);
    if (status == NAULA_REFUSED && strcmp(refusal->code, "missing_commit_or_ref") == 0) {
        branch = found->repository.default_branch;
        status = branch == NULL ? NAULA_REFUSED
                                : history->contains(history->context, &place, branch, subject->commit, &contained,
                                                    refusal);
        if (status == NAULA_REFUSED && strcmp(refusal->code, "missing_commit_or_ref") == 0) {
            contained = 0;
            status = NAULA_OK;
        }
    }
    if (status != NAULA_OK) {
        return status;
    }
    found->branch = copy_or_null(branch);
    if (found->branch == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    uint8_t *bytes = NULL;
    size_t length = 0;
    status = provider->read_file(provider->context, host, number, branch, "repo.json", token, &bytes, &length, refusal);
    if (status == NAULA_OK) {
        naula_refusal ignored = fresh_refusal();
        naula_status parsed = naula_repo_file_parse(bytes, length, &found->file, &ignored);
        found->has_file = parsed == NAULA_OK;
        status = parsed == NAULA_OUT_OF_MEMORY ? parsed : NAULA_OK;
    } else if (status == NAULA_REFUSED && strcmp(refusal->code, "missing_file_or_ref") == 0) {
        status = NAULA_OK;
    }
    naula_memory_release(bytes);
    if (status != NAULA_OK) {
        return status;
    }
    const char *stated = found->has_file ? found->file.id : NULL;
    *ours = contained && (stated != NULL ? strcmp(stated, subject->id_text) == 0 : subject->id_is_root);
    return NAULA_OK;
}

/**
 * Finds this repo under `account`: at the number the last run found it at, else under the file's name or any name its
 * history gave it. A repo found that is not this one is passed over; a home that cannot be heard stops the search.
 */
static naula_status locate(reconciler *subject, const naula_provider *provider, const char *word, const char *host,
                           const char *account, found_home *found, int *located, naula_refusal *refusal) {
    *located = 0;
    memset(found, 0, sizeof *found);
    const char *token = naula_session_token(subject->session, word, host);
    uint64_t hinted = hint_for(subject, word, host, account);
    int ours = 0;
    if (hinted != 0) {
        naula_status status = examine(subject, provider, word, host, hinted, found, &ours, refusal);
        if (status == NAULA_OK && ours && same_text(found->repository.account, account)) {
            *located = 1;
            return NAULA_OK;
        }
        release_found(found);
        if (status != NAULA_OK && status != NAULA_NOT_FOUND) {
            return status;
        }
    }
    for (size_t index = 0; index < subject->history_count; index++) {
        const char *name = subject->history[index].name;
        int seen = 0;
        for (size_t earlier = 0; earlier < index; earlier++) {
            seen = seen || same_text(subject->history[earlier].name, name);
        }
        uint64_t number = 0;
        naula_status status = seen ? NAULA_OK : provider->find(provider->context, host, account, name, token, &number,
                                                                refusal);
        if (status != NAULA_OK) {
            return status;
        }
        if (number == 0 || number == hinted) {
            continue;
        }
        status = examine(subject, provider, word, host, number, found, &ours, refusal);
        if (status == NAULA_OK && ours) {
            *located = 1;
            return NAULA_OK;
        }
        release_found(found);
        if (status != NAULA_OK && status != NAULA_NOT_FOUND) {
            return status;
        }
    }
    return NAULA_OK;
}

static naula_status refuse_unheard(const char *word, const char *account, naula_status status,
                                   naula_refusal *refusal) {
    if (status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    naula_refusal heard = *refusal;
    naula_refuse(refusal, "home-unheard", "run again once the home answers; nothing was changed",
                 "%s account %s could not be heard, so nothing is decided from it: %s: %s", word, account, heard.code,
                 heard.detail);
    return NAULA_REFUSED;
}

static naula_status local_identity(reconciler *subject, char **branch, naula_refusal *refusal) {
    const char *development[] = {"rev-parse", "--verify", "--quiet", "refs/heads/development", NULL};
    const char *current[] = {"symbolic-ref", "--quiet", "--short", "HEAD", NULL};
    naula_status status = NAULA_OK;
    if (git_succeeds(subject->repository, development)) {
        *branch = naula_memory_duplicate_text("development");
        status = *branch == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    } else {
        status = git_output(subject->repository, current, branch, refusal);
    }
    naula_repo_file local;
    memset(&local, 0, sizeof local);
    int present = 0;
    if (status == NAULA_OK) {
        status = file_at(subject->repository, *branch, &local, &present, refusal);
    }
    if (status == NAULA_OK && !present) {
        naula_refuse(refusal, "unadopted", "run naula adopt, commit and push first",
                     "the working branch %s holds no repo.json of format 2", *branch);
        status = NAULA_REFUSED;
    }
    naula_id id;
    if (status == NAULA_OK) {
        status = naula_git_repository_id(subject->repository, *branch, local.id, &id, refusal);
        naula_repo_file_release(&local);
    }
    if (status != NAULA_OK) {
        return status;
    }
    snprintf(subject->commit, sizeof subject->commit, "%s", id.commit);
    snprintf(subject->id_text, sizeof subject->id_text, "git:%s", id.commit);
    naula_id_release(&id);
    size_t parents = 0;
    status = naula_git_parent_count(subject->repository, subject->commit, &parents, refusal);
    subject->id_is_root = parents == 0;
    return status;
}

static int by_text(const void *left, const void *right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

/**
 * Hints are kept per checkout identity, the way the resolver keys its cache by a consumer's `at`: the id and every
 * provider home the checkout's own repo.json history listed, each `<provider>@<host>:<account>` lowercased, sorted,
 * joined by newlines and hashed. Another checkout of the id, a fork naming its own homes, never reads or writes them.
 */
static naula_status name_the_hints(reconciler *subject) {
    size_t total = 0;
    for (size_t index = 0; index < subject->history_count; index++) {
        total += subject->history[index].home_count;
    }
    char **entries = naula_memory_allocate((total + 1) * sizeof *entries);
    if (entries == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    size_t count = 0;
    naula_status status = NAULA_OK;
    for (size_t index = 0; status == NAULA_OK && index < subject->history_count; index++) {
        const naula_repo_file *file = &subject->history[index];
        for (size_t entry = 0; status == NAULA_OK && entry < file->home_count; entry++) {
            const naula_home *home = &file->homes[entry];
            if (home->kind != NAULA_HOME_PROVIDER) {
                continue;
            }
            size_t length = strlen(home->provider) + (home->host == NULL ? 0 : strlen(home->host)) +
                            strlen(home->account) + 3;
            char *text = naula_memory_allocate(length);
            if (text == NULL) {
                status = NAULA_OUT_OF_MEMORY;
                break;
            }
            snprintf(text, length, "%s@%s:%s", home->provider, home->host == NULL ? "" : home->host, home->account);
            for (char *letter = text; *letter != '\0'; letter++) {
                *letter = (*letter >= 'A' && *letter <= 'Z') ? (char)(*letter + ('a' - 'A')) : *letter;
            }
            int repeated = 0;
            for (size_t other = 0; other < count && !repeated; other++) {
                repeated = strcmp(entries[other], text) == 0;
            }
            if (repeated) {
                naula_memory_release(text);
            } else {
                entries[count++] = text;
            }
        }
    }
    if (status == NAULA_OK) {
        qsort(entries, count, sizeof *entries, by_text);
        naula_sha256 hash;
        char key[NAULA_SHA256_HEX_SIZE];
        naula_sha256_start(&hash);
        for (size_t index = 0; index < count; index++) {
            if (index > 0) {
                naula_sha256_add(&hash, (const uint8_t *)"\n", 1);
            }
            naula_sha256_add(&hash, (const uint8_t *)entries[index], strlen(entries[index]));
        }
        naula_sha256_hex(&hash, key);
        snprintf(subject->hint_name, sizeof subject->hint_name, "%s-%s", subject->commit, key);
    }
    for (size_t index = 0; index < count; index++) {
        naula_memory_release(entries[index]);
    }
    naula_memory_release(entries);
    return status;
}

static naula_status try_serving(reconciler *subject, const char *word, const char *host, const char *account,
                                int *located, naula_refusal *refusal) {
    *located = 0;
    const naula_provider *provider = naula_providers_find(subject->session->providers, word, refusal);
    if (provider == NULL) {
        return NAULA_OK;
    }
    naula_status status = locate(subject, provider, word, host, account, &subject->serving, located, refusal);
    if (is_unheard(status) || status == NAULA_OUT_OF_MEMORY) {
        return refuse_unheard(word, account, status, refusal);
    }
    if (*located) {
        subject->serving_provider = provider;
        subject->serving_word = naula_memory_duplicate_text(word);
        subject->serving_host = copy_or_null(host);
        return subject->serving_word == NULL || (host != NULL && subject->serving_host == NULL) ? NAULA_OUT_OF_MEMORY
                                                                                                : NAULA_OK;
    }
    return NAULA_OK;
}

static int history_lists(const reconciler *subject, const char *word, const char *host, const char *account) {
    for (size_t index = 0; index < subject->history_count; index++) {
        const naula_repo_file *file = &subject->history[index];
        for (size_t entry = 0; entry < file->home_count; entry++) {
            const naula_home *home = &file->homes[entry];
            if (home->kind == NAULA_HOME_PROVIDER && strcmp(home->provider, word) == 0 &&
                same_text(home->host, host) && same_text(home->account, account)) {
                return 1;
            }
        }
    }
    return 0;
}

/**
 * The repo at a hinted number, taken as the serving home once it holds the id: the hint's account must be one the
 * checkout's own repo.json history listed, which is what lets a home transferred since the last pull still be found.
 */
static naula_status try_hinted_serving(reconciler *subject, const home_hint *hint, int *located,
                                       naula_refusal *refusal) {
    *located = 0;
    const naula_provider *provider = naula_providers_find(subject->session->providers, hint->provider, refusal);
    if (provider == NULL) {
        return NAULA_OK;
    }
    int ours = 0;
    naula_status status =
        examine(subject, provider, hint->provider, hint->host, hint->number, &subject->serving, &ours, refusal);
    if (status == NAULA_NOT_FOUND || (status == NAULA_OK && !ours)) {
        release_found(&subject->serving);
        return NAULA_OK;
    }
    if (status != NAULA_OK) {
        release_found(&subject->serving);
        return refuse_unheard(hint->provider, hint->account, status, refusal);
    }
    *located = 1;
    subject->serving_provider = provider;
    subject->serving_word = naula_memory_duplicate_text(hint->provider);
    subject->serving_host = copy_or_null(hint->host);
    return subject->serving_word == NULL || (hint->host != NULL && subject->serving_host == NULL) ? NAULA_OUT_OF_MEMORY
                                                                                                  : NAULA_OK;
}

/**
 * The first home of the local file found holding this repo, else a hinted home of an account the checkout's history
 * listed, since a checkout not pulled since a transfer still names the old account. A hint for any other account is
 * never a serving home (`serving-unproven`): the file it serves would steer every write.
 */
static naula_status find_serving(reconciler *subject, const naula_repo_file *local, naula_refusal *refusal) {
    int located = 0;
    naula_status status = NAULA_OK;
    for (size_t index = 0; status == NAULA_OK && !located && index < local->home_count; index++) {
        const naula_home *home = &local->homes[index];
        if (home->kind == NAULA_HOME_PROVIDER) {
            status = try_serving(subject, home->provider, home->host, home->account, &located, refusal);
        }
    }
    size_t unproven = 0;
    for (size_t index = 0; status == NAULA_OK && !located && index < subject->hint_count; index++) {
        const home_hint *hint = &subject->hints[index];
        if (history_lists(subject, hint->provider, hint->host, hint->account)) {
            status = try_hinted_serving(subject, hint, &located, refusal);
        } else {
            unproven++;
        }
    }
    if (status == NAULA_OK && !located && unproven > 0) {
        naula_refuse(refusal, "serving-unproven", "pull the working branch into the checkout, then run again",
                     "no home the checkout's repo.json has listed holds %s, and a hint naming another account is not "
                     "taken as the serving home",
                     subject->id_text);
        status = NAULA_REFUSED;
    } else if (status == NAULA_OK && !located) {
        naula_refuse(refusal, "no-home-found", "push the repo to a home its repo.json lists, then run again",
                     "no home the local repo.json lists holds %s", subject->id_text);
        status = NAULA_REFUSED;
    }
    return status;
}

/** Fetches the serving home's working branch into the checkout and reads the file its tip holds. */
static naula_status read_wanted(reconciler *subject, naula_refusal *refusal) {
    const char *url = subject->serving.repository.clone_url;
    if (!url_is_allowed(subject, url)) {
        naula_refuse(refusal, "clone-url-refused", "ask the host for an https url",
                     "the serving home answers no url naula hands to git");
        return NAULA_REFUSED;
    }
    char refspec[300];
    snprintf(refspec, sizeof refspec, "refs/heads/%s", subject->serving.branch);
    const char *fetch[] = {"fetch", "--quiet", "--no-tags", url, refspec, NULL};
    const char *head[] = {"rev-parse", "--verify", "FETCH_HEAD^{commit}", NULL};
    char *output = NULL;
    naula_status status = git_output(subject->repository, fetch, &output, refusal);
    naula_memory_release(output);
    output = NULL;
    if (status == NAULA_OK) {
        status = git_output(subject->repository, head, &output, refusal);
    }
    if (status != NAULA_OK) {
        return status;
    }
    snprintf(subject->tip, sizeof subject->tip, "%s", output);
    naula_memory_release(output);
    int present = 0;
    status = file_at(subject->repository, subject->tip, &subject->file, &present, refusal);
    if (status == NAULA_OK && !present) {
        naula_refuse(refusal, "primary-file-invalid", "push a valid repo.json of format 2 to the primary",
                     "the serving home's working branch holds no valid repo.json of format 2");
        status = NAULA_REFUSED;
    }
    naula_id id;
    if (status == NAULA_OK) {
        status = naula_git_repository_id(subject->repository, subject->tip, subject->file.id, &id, refusal);
    }
    if (status != NAULA_OK) {
        return status;
    }
    int same = strcmp(id.commit, subject->commit) == 0;
    naula_id_release(&id);
    if (!same) {
        naula_refuse(refusal, "id-changed", "push the working branch the checkout holds, or make a fresh checkout",
                     "the serving home's working branch is not %s", subject->id_text);
        return NAULA_REFUSED;
    }
    return read_history(subject, subject->tip, refusal);
}

static int file_lists(const naula_repo_file *file, const char *word, const char *host, const char *account) {
    for (size_t index = 0; index < file->home_count; index++) {
        const naula_home *home = &file->homes[index];
        if (home->kind == NAULA_HOME_PROVIDER && strcmp(home->provider, word) == 0 && same_text(home->host, host) &&
            same_text(home->account, account)) {
            return 1;
        }
    }
    return 0;
}

static int number_is_planned(const reconciler *subject, uint64_t number, const char *word, size_t before) {
    for (size_t index = 0; index < before; index++) {
        const planned_home *home = &subject->planned[index];
        if (home->found.number == number && strcmp(home->wanted->provider, word) == 0) {
            return 1;
        }
    }
    return 0;
}

/** A home the file no longer lists under its account, of the same provider and host: where a transfer starts. */
static naula_status locate_transferred(reconciler *subject, planned_home *home, size_t index, int *located,
                                       naula_refusal *refusal) {
    const naula_home *wanted = home->wanted;
    for (size_t entry = 0; entry < subject->history_count && !*located; entry++) {
        const naula_repo_file *file = &subject->history[entry];
        for (size_t other = 0; other < file->home_count && !*located; other++) {
            const naula_home *was = &file->homes[other];
            if (was->kind != NAULA_HOME_PROVIDER || strcmp(was->provider, wanted->provider) != 0 ||
                !same_text(was->host, wanted->host) ||
                file_lists(&subject->file, was->provider, was->host, was->account)) {
                continue;
            }
            naula_status status = locate(subject, home->provider, wanted->provider, wanted->host, was->account,
                                         &home->found, located, refusal);
            if (status != NAULA_OK) {
                return status == NAULA_NOT_FOUND ? NAULA_OK : refuse_unheard(was->provider, was->account, status, refusal);
            }
            if (*located && number_is_planned(subject, home->found.number, wanted->provider, index)) {
                release_found(&home->found);
                *located = 0;
            }
            if (*located) {
                home->old_account = naula_memory_duplicate_text(was->account);
                return home->old_account == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
            }
        }
    }
    return NAULA_OK;
}

static naula_status plan_homes(reconciler *subject, naula_refusal *refusal) {
    subject->planned = naula_memory_allocate((subject->file.home_count + 1) * sizeof *subject->planned);
    if (subject->planned == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < subject->file.home_count; index++) {
        const naula_home *wanted = &subject->file.homes[index];
        if (wanted->kind != NAULA_HOME_PROVIDER) {
            say(subject, "note home-not-reconciled: homes[%zu] is a %s home, which naula reads and never writes", index,
                wanted->kind == NAULA_HOME_WEB ? "web" : "url");
            continue;
        }
        planned_home *home = &subject->planned[subject->planned_count++];
        memset(home, 0, sizeof *home);
        home->wanted = wanted;
        home->provider = naula_providers_find(subject->session->providers, wanted->provider, refusal);
        if (home->provider == NULL) {
            return NAULA_REFUSED;
        }
        int located = 0;
        naula_status status =
            locate(subject, home->provider, wanted->provider, wanted->host, wanted->account, &home->found, &located,
                   refusal);
        if (status != NAULA_OK && status != NAULA_NOT_FOUND) {
            return refuse_unheard(wanted->provider, wanted->account, status, refusal);
        }
        if (located && number_is_planned(subject, home->found.number, wanted->provider, subject->planned_count - 1)) {
            release_found(&home->found);
            located = 0;
        }
        if (!located) {
            status = locate_transferred(subject, home, subject->planned_count - 1, &located, refusal);
            if (status != NAULA_OK) {
                return status;
            }
        }
    }
    return NAULA_OK;
}

/** Every home found, the serving one aside, must list the serving home in its own repo.json, or it is not ours. */
static naula_status check_both_ways(reconciler *subject, naula_refusal *refusal) {
    const naula_repository *serving = &subject->serving.repository;
    int serving_planned = 0;
    for (size_t index = 0; index < subject->planned_count; index++) {
        const planned_home *home = &subject->planned[index];
        if (home->found.number == 0) {
            continue;
        }
        int is_serving = home->found.number == subject->serving.number &&
                         strcmp(home->wanted->provider, subject->serving_word) == 0 &&
                         same_text(home->wanted->host, subject->serving_host);
        serving_planned = serving_planned || is_serving;
        if (is_serving) {
            continue;
        }
        char *text = place_text(home->wanted->provider, home->wanted->host, home->found.number);
        naula_status status = NAULA_OK;
        if (!home->found.has_file) {
            naula_refuse(refusal, "home-unadopted", "push the adopted repo.json to that home by hand, then run again",
                         "%s holds %s but no repo.json of format 2, so nothing shows it is this repo's home",
                         text == NULL ? "a home" : text, subject->id_text);
            status = NAULA_REFUSED;
        } else if (!file_lists(&home->found.file, subject->serving_word, subject->serving_host, serving->account)) {
            naula_refuse(refusal, "disowned-by",
                         "push the working branch to that home by hand if it is this repo's, or drop it from homes",
                         "%s holds %s but its repo.json does not list the serving home %s/%s",
                         text == NULL ? "a home" : text, subject->id_text, serving->account, serving->name);
            status = NAULA_REFUSED;
        }
        naula_memory_release(text);
        if (status != NAULA_OK) {
            return status;
        }
    }
    if (!serving_planned) {
        naula_refuse(refusal, "primary-not-listed", "list the home in homes, or push the file to the home that is",
                     "the repo.json %s/%s serves does not list it", serving->account, serving->name);
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

static int target_is_allowed(const reconciler *subject, const naula_home *target, const char *text) {
    const naula_reconcile_options *options = subject->options;
    for (size_t index = 0; options != NULL && index < options->held_account_count; index++) {
        const naula_home *held = &options->held_accounts[index];
        if (held->kind == NAULA_HOME_PROVIDER && strcmp(held->provider, target->provider) == 0 &&
            same_text(held->host, target->host) && same_text(held->account, target->account)) {
            return 1;
        }
    }
    for (size_t index = 0; options != NULL && index < options->accepted_home_count; index++) {
        if (same_text(options->accepted_homes[index], text)) {
            return 1;
        }
    }
    return 0;
}

/**
 * A home to create, or an account to transfer one into, that is neither an account an org of the root holds nor one
 * this run accepted refuses the whole run before anything is sent: whoever can push the file could otherwise name any
 * account and have every branch pushed there, or the repo moved there, with the user's credentials.
 */
static naula_status check_targets(reconciler *subject, naula_refusal *refusal) {
    for (size_t index = 0; index < subject->planned_count; index++) {
        const planned_home *home = &subject->planned[index];
        if (home->found.number != 0 && home->old_account == NULL) {
            continue;
        }
        const naula_home *target = home->wanted;
        char text[300];
        snprintf(text, sizeof text, "%s%s%s:%s", target->provider, target->host == NULL ? "" : "@",
                 target->host == NULL ? "" : target->host, target->account);
        if (!target_is_allowed(subject, target, text)) {
            naula_refuse(refusal, "home-unaccepted",
                         "if that account is meant to hold the repo, run again with --accept-home <the home named>",
                         "the file asks to %s %s, an account no org of this root holds and no run accepted; nothing "
                         "was sent",
                         home->found.number == 0 ? "create the repo in" : "transfer the repo into", text);
            return NAULA_REFUSED;
        }
    }
    return NAULA_OK;
}

static int needs_rename(const reconciler *subject, const planned_home *home) {
    return home->found.number != 0 && home->found.repository.name != NULL && strcmp(home->found.repository.name, subject->file.name) != 0;
}

static int path_is_workflow(const char *path) {
    size_t length = strlen(path);
    return (length > 4 && strcmp(path + length - 4, ".yml") == 0) ||
           (length > 5 && strcmp(path + length - 5, ".yaml") == 0);
}

static naula_status holds_reusable_workflow(reconciler *subject, char *found, size_t capacity, naula_refusal *refusal) {
    const char *list[] = {"ls-tree", "-r", "--name-only", subject->tip, "--", ".github/workflows", NULL};
    char *output = NULL;
    naula_status status = git_output(subject->repository, list, &output, refusal);
    for (char *line = output; status == NAULA_OK && line != NULL && *line != '\0' && found[0] == '\0';) {
        char *end = strchr(line, '\n');
        if (end != NULL) {
            *end = '\0';
        }
        line[strcspn(line, "\r")] = '\0';
        if (path_is_workflow(line)) {
            char object[600];
            snprintf(object, sizeof object, "%s:%s", subject->tip, line);
            const char *show[] = {"cat-file", "blob", object, NULL};
            char *text = NULL;
            status = git_output(subject->repository, show, &text, refusal);
            if (status == NAULA_OK && strstr(text, "workflow_call") != NULL) {
                snprintf(found, capacity, "%s holds a workflow_call workflow, which callers reach by owner and name",
                         line);
            }
            naula_memory_release(text);
        }
        line = end == NULL ? NULL : end + 1;
    }
    naula_memory_release(output);
    return status;
}

/**
 * github-gradle names a release's assets after the repo, so a Gradle build whose primary has a release is reached by
 * name; a primary that cannot say whether it has one is counted as having one.
 */
static naula_status publishes_by_name(reconciler *subject, char *found, size_t capacity, naula_refusal *refusal) {
    char groovy[96];
    char kotlin[96];
    snprintf(groovy, sizeof groovy, "%s:build.gradle", subject->tip);
    snprintf(kotlin, sizeof kotlin, "%s:build.gradle.kts", subject->tip);
    const char *has_groovy[] = {"cat-file", "-e", groovy, NULL};
    const char *has_kotlin[] = {"cat-file", "-e", kotlin, NULL};
    if (!git_succeeds(subject->repository, has_groovy) && !git_succeeds(subject->repository, has_kotlin)) {
        return NAULA_OK;
    }
    const naula_provider *provider = subject->serving_provider;
    naula_release_index index;
    memset(&index, 0, sizeof index);
    index.size = sizeof index;
    naula_refusal answer = fresh_refusal();
    naula_status status = provider->release_index(
        provider->context, subject->serving_host, subject->serving.number, NULL, "naula-release-probe",
        naula_session_token(subject->session, subject->serving_word, subject->serving_host), &index, &answer);
    naula_release_index_release(&index);
    if (status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    if (status == NAULA_REFUSED && strcmp(answer.code, "missing_release") == 0) {
        return NAULA_OK;
    }
    snprintf(found, capacity, "it has a Gradle build and %s, and github-gradle names release assets after the repo",
             status == NAULA_OK || strcmp(answer.code, "no_index_asset") == 0 ? "a release"
                                                                               : "its primary cannot say it has no release");
    (void)refusal;
    return NAULA_OK;
}

static naula_status check_literal_consumers(reconciler *subject, naula_refusal *refusal) {
    int renames = 0;
    for (size_t index = 0; index < subject->planned_count; index++) {
        renames = renames || needs_rename(subject, &subject->planned[index]) || subject->planned[index].old_account != NULL;
    }
    if (!renames) {
        return NAULA_OK;
    }
    char found[700] = "";
    naula_status status = holds_reusable_workflow(subject, found, sizeof found, refusal);
    if (status == NAULA_OK && found[0] == '\0') {
        status = publishes_by_name(subject, found, sizeof found, refusal);
    }
    if (status == NAULA_OK && found[0] != '\0') {
        naula_refuse(refusal, "literal-consumers",
                     "update the consumers that name this repo by owner and name first, then rename it",
                     "%s is renamed or transferred and %s", subject->file.name, found);
        status = NAULA_REFUSED;
    }
    return status;
}

/** Another repo already holding the file's name under a home's account refuses the whole run before any write. */
static naula_status check_names_free(reconciler *subject, naula_refusal *refusal) {
    for (size_t index = 0; index < subject->planned_count; index++) {
        const planned_home *home = &subject->planned[index];
        if (home->found.number != 0 && !needs_rename(subject, home) && home->old_account == NULL) {
            continue;
        }
        uint64_t number = 0;
        const naula_home *wanted = home->wanted;
        naula_status status = home->provider->find(home->provider->context, wanted->host, wanted->account,
                                                   subject->file.name,
                                                   naula_session_token(subject->session, wanted->provider, wanted->host),
                                                   &number, refusal);
        if (status == NAULA_OUT_OF_MEMORY) {
            return status;
        }
        if (status != NAULA_OK) {
            naula_refusal heard = *refusal;
            naula_refuse(refusal, "name-unverifiable", "run again once the home answers; nothing was changed",
                         "whether %s account %s holds a repository named %s is unknown: %s: %s", wanted->provider,
                         wanted->account, subject->file.name, heard.code, heard.detail);
            return NAULA_REFUSED;
        }
        if (number != 0 && number != home->found.number) {
            naula_refuse(refusal, "name-taken", "choose another name, or retire the repo holding it",
                         "%s account %s already holds a repository named %s (number %llu) that is not this repo; "
                         "nothing was changed",
                         wanted->provider, wanted->account, subject->file.name, (unsigned long long)number);
            return NAULA_REFUSED;
        }
    }
    return NAULA_OK;
}

static const char *home_token(const reconciler *subject, const planned_home *home) {
    return naula_session_token(subject->session, home->wanted->provider, home->wanted->host);
}

static void rename_home(reconciler *subject, planned_home *home) {
    if (!needs_rename(subject, home)) {
        return;
    }
    naula_refusal refusal = fresh_refusal();
    naula_status status = home->provider->rename(home->provider->context, home->wanted->host, home->found.number,
                                                 subject->file.name, home_token(subject, home), &refusal);
    char *text = place_text(home->wanted->provider, home->wanted->host, home->found.number);
    if (status == NAULA_OK) {
        subject->report->changed++;
        say(subject, "rename %s %s to %s", text == NULL ? "(home)" : text, home->found.repository.name,
            subject->file.name);
        char *name = naula_memory_duplicate_text(subject->file.name);
        if (name != NULL) {
            naula_memory_release(home->found.repository.name);
            home->found.repository.name = name;
        }
    } else {
        naula_refusal heard = refusal;
        naula_refuse(&refusal, "rename-refused", "the next naula sync tries again",
                     "%s refused renaming it from %s to %s: %s: %s", text == NULL ? "a home" : text,
                     home->found.repository.name, subject->file.name, heard.code, heard.detail);
        home->failed = 1;
        record_failure(subject, &refusal);
    }
    naula_memory_release(text);
}

static void transfer_home(reconciler *subject, planned_home *home) {
    if (home->old_account == NULL || home->failed) {
        return;
    }
    naula_refusal refusal = fresh_refusal();
    naula_status status = home->provider->transfer(home->provider->context, home->wanted->host, home->found.number,
                                                   home->wanted->account, home_token(subject, home), &refusal);
    char *text = place_text(home->wanted->provider, home->wanted->host, home->found.number);
    if (status == NAULA_OK) {
        subject->report->changed++;
        say(subject, "transfer %s from %s to %s", text == NULL ? "(home)" : text, home->old_account,
            home->wanted->account);
    } else {
        naula_refusal heard = refusal;
        naula_refuse(&refusal, "transfer-refused", "the next naula sync tries again",
                     "%s refused moving from %s to %s: %s: %s", text == NULL ? "a home" : text, home->old_account,
                     home->wanted->account, heard.code, heard.detail);
        home->failed = 1;
        record_failure(subject, &refusal);
    }
    naula_memory_release(text);
}

static void create_home(reconciler *subject, planned_home *home) {
    if (home->found.number != 0) {
        return;
    }
    naula_refusal refusal = fresh_refusal();
    uint64_t number = 0;
    naula_status status = home->provider->create(home->provider->context, home->wanted->host, home->wanted->account,
                                                 subject->file.name, &subject->file.metadata,
                                                 home_token(subject, home), &number, &refusal);
    if (status != NAULA_OK) {
        naula_refusal heard = refusal;
        naula_refuse(&refusal, "create-refused", "the next naula sync tries again",
                     "%s account %s refused creating %s: %s: %s", home->wanted->provider, home->wanted->account,
                     subject->file.name, heard.code, heard.detail);
        home->failed = 1;
        record_failure(subject, &refusal);
        return;
    }
    home->found.number = number;
    home->found.repository.size = sizeof home->found.repository;
    subject->report->changed++;
    char *text = place_text(home->wanted->provider, home->wanted->host, number);
    say(subject, "create %s %s/%s", text == NULL ? "(home)" : text, home->wanted->account, subject->file.name);
    naula_memory_release(text);
}

static naula_status run_quietly(reconciler *subject, const char *const *arguments, naula_refusal *refusal) {
    char *output = NULL;
    naula_status status = git_output(subject->repository, arguments, &output, refusal);
    naula_memory_release(output);
    return status;
}

/** Removes every ref naula keeps below refs/naula-reconcile/, which only a run of its own puts there. */
static void drop_temporary_refs(reconciler *subject) {
    const char *list[] = {"for-each-ref", "--format=%(refname)", TEMPORARY_REFS, NULL};
    char *output = NULL;
    naula_refusal refusal = fresh_refusal();
    if (git_output(subject->repository, list, &output, &refusal) != NAULA_OK) {
        return;
    }
    for (char *line = output; line != NULL && *line != '\0';) {
        char *end = strchr(line, '\n');
        if (end != NULL) {
            *end = '\0';
        }
        line[strcspn(line, "\r")] = '\0';
        const char *remove[] = {"update-ref", "-d", line, NULL};
        run_quietly(subject, remove, &refusal);
        line = end == NULL ? NULL : end + 1;
    }
    naula_memory_release(output);
}

static naula_status fetch_every_ref(reconciler *subject, naula_refusal *refusal) {
    if (subject->refs_fetched) {
        return NAULA_OK;
    }
    drop_temporary_refs(subject);
    const char *fetch[] = {"fetch", "--quiet", "--no-tags", subject->serving.repository.clone_url,
                           "+refs/heads/*:refs/naula-reconcile/heads/*", "+refs/tags/*:refs/naula-reconcile/tags/*",
                           NULL};
    naula_status status = run_quietly(subject, fetch, refusal);
    subject->refs_fetched = status == NAULA_OK;
    return status;
}

/** Pushes every branch and tag of the serving home, never forcing: a home ahead of it is reported, never rewound. */
static void push_home(reconciler *subject, planned_home *home) {
    int is_serving = home->found.number == subject->serving.number &&
                     strcmp(home->wanted->provider, subject->serving_word) == 0;
    if (home->found.number == 0 || is_serving) {
        return;
    }
    naula_refusal refusal = fresh_refusal();
    naula_repository repository;
    memset(&repository, 0, sizeof repository);
    repository.size = sizeof repository;
    naula_status status = home->provider->lookup(home->provider->context, home->wanted->host, home->found.number,
                                                 home_token(subject, home), &repository, &refusal);
    if (status == NAULA_OK && !url_is_allowed(subject, repository.clone_url)) {
        naula_refuse(&refusal, "clone-url-refused", "ask the host for an https url",
                     "the home answers no url naula hands to git");
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK) {
        status = fetch_every_ref(subject, &refusal);
    }
    naula_run_result result;
    memset(&result, 0, sizeof result);
    if (status == NAULA_OK) {
        const char *push[] = {"push", "--porcelain", repository.clone_url,
                              "refs/naula-reconcile/heads/*:refs/heads/*", "refs/naula-reconcile/tags/*:refs/tags/*",
                              NULL};
        status = naula_git_run(subject->repository, push, NULL, 0, &result, &refusal);
    }
    char *text = place_text(home->wanted->provider, home->wanted->host, home->found.number);
    if (status == NAULA_OK && result.exit_code == 0) {
        int updated = 0;
        for (const char *line = result.output; line != NULL && *line != '\0';) {
            updated = updated || ((line[0] == '*' || line[0] == ' ' || line[0] == '+') && line[1] == '\t');
            line = strchr(line, '\n');
            line = line == NULL ? NULL : line + 1;
        }
        if (updated) {
            subject->report->changed++;
            say(subject, "push %s", text == NULL ? "(home)" : text);
        }
    } else {
        if (status == NAULA_OK) {
            size_t length = strlen(result.error);
            naula_refuse(&refusal, "push-failed", "the next naula sync tries again",
                         "pushing to %s failed: %.*s", text == NULL ? "a home" : text,
                         (int)(length > 600 ? 600 : length), result.error);
        }
        home->failed = 1;
        record_failure(subject, &refusal);
    }
    naula_run_result_release(&result);
    naula_memory_release(text);
    naula_repository_release(&repository);
}

static int same_topics(const naula_metadata *left, const naula_metadata *right) {
    if (left->topic_count != right->topic_count) {
        return 0;
    }
    for (size_t index = 0; index < left->topic_count; index++) {
        int found = 0;
        for (size_t other = 0; other < right->topic_count && !found; other++) {
            found = strcmp(left->topics[index], right->topics[other]) == 0;
        }
        if (!found) {
            return 0;
        }
    }
    return 1;
}

static int same_optional(const char *left, const char *right) {
    return (left == NULL ? "" : left)[0] == '\0' ? (right == NULL || right[0] == '\0')
                                                 : right != NULL && strcmp(left, right) == 0;
}

static int settings_hold(const cJSON *wanted, const cJSON *shown) {
    const cJSON *member = NULL;
    cJSON_ArrayForEach(member, wanted) {
        const cJSON *other = cJSON_GetObjectItemCaseSensitive(shown, member->string);
        if (other == NULL || !cJSON_Compare(member, other, 1)) {
            return 0;
        }
    }
    return 1;
}

static uint32_t differing_members(const naula_metadata *wanted, const naula_metadata *shown) {
    uint32_t differing = 0;
    if ((wanted->present & NAULA_METADATA_DESCRIPTION) && !same_optional(wanted->description, shown->description)) {
        differing |= NAULA_METADATA_DESCRIPTION;
    }
    if ((wanted->present & NAULA_METADATA_TOPICS) && !same_topics(wanted, shown)) {
        differing |= NAULA_METADATA_TOPICS;
    }
    if ((wanted->present & NAULA_METADATA_HOMEPAGE) && !same_optional(wanted->homepage, shown->homepage)) {
        differing |= NAULA_METADATA_HOMEPAGE;
    }
    if ((wanted->present & NAULA_METADATA_SETTINGS) && !settings_hold(wanted->settings, shown->settings)) {
        differing |= NAULA_METADATA_SETTINGS;
    }
    return differing;
}

static void write_metadata(reconciler *subject, planned_home *home) {
    const naula_metadata *wanted = &subject->file.metadata;
    if (home->found.number == 0 || wanted->present == 0) {
        return;
    }
    uint32_t supported = 0;
    int anonymous = 0;
    home->provider->describe(home->provider->context, &supported, &anonymous);
    naula_refusal refusal = fresh_refusal();
    uint32_t differing = wanted->present;
    naula_status status = NAULA_OK;
    if (supported & NAULA_OP_READ_METADATA) {
        naula_metadata shown;
        memset(&shown, 0, sizeof shown);
        shown.size = sizeof shown;
        status = home->provider->read_metadata(home->provider->context, home->wanted->host, home->found.number,
                                               home_token(subject, home), &shown, &refusal);
        if (status == NAULA_OK) {
            differing = differing_members(wanted, &shown);
        }
        naula_metadata_release(&shown);
    }
    char *text = place_text(home->wanted->provider, home->wanted->host, home->found.number);
    uint32_t applied = 0;
    if (status == NAULA_OK && differing != 0) {
        naula_metadata asked = *wanted;
        asked.present = differing;
        status = home->provider->write_metadata(home->provider->context, home->wanted->host, home->found.number,
                                                &asked, home_token(subject, home), &applied, &refusal);
    }
    if (status == NAULA_OK && applied != 0) {
        subject->report->changed++;
        say(subject, "metadata %s%s%s%s%s", text == NULL ? "(home)" : text,
            (applied & NAULA_METADATA_DESCRIPTION) ? " description" : "",
            (applied & NAULA_METADATA_TOPICS) ? " topics" : "", (applied & NAULA_METADATA_HOMEPAGE) ? " homepage" : "",
            (applied & NAULA_METADATA_SETTINGS) ? " settings" : "");
    } else if (status != NAULA_OK) {
        naula_refusal heard = refusal;
        naula_refuse(&refusal, "metadata-refused", "the next naula sync tries again", "%s refused its metadata: %s: %s",
                     text == NULL ? "a home" : text, heard.code, heard.detail);
        record_failure(subject, &refusal);
    }
    naula_memory_release(text);
}

static void write_hosts(reconciler *subject) {
    for (size_t index = 0; index < subject->planned_count; index++) {
        rename_home(subject, &subject->planned[index]);
    }
    for (size_t index = 0; index < subject->planned_count; index++) {
        transfer_home(subject, &subject->planned[index]);
    }
    for (size_t index = 0; index < subject->planned_count; index++) {
        create_home(subject, &subject->planned[index]);
    }
    for (size_t index = 0; index < subject->planned_count; index++) {
        push_home(subject, &subject->planned[index]);
    }
    for (size_t index = 0; index < subject->planned_count; index++) {
        write_metadata(subject, &subject->planned[index]);
    }
}

static void release_reconciler(reconciler *subject) {
    release_history(subject);
    for (size_t index = 0; index < subject->hint_count; index++) {
        naula_memory_release(subject->hints[index].provider);
        naula_memory_release(subject->hints[index].host);
        naula_memory_release(subject->hints[index].account);
    }
    for (size_t index = 0; index < subject->planned_count; index++) {
        release_found(&subject->planned[index].found);
        naula_memory_release(subject->planned[index].old_account);
    }
    naula_memory_release(subject->planned);
    release_found(&subject->serving);
    naula_memory_release(subject->serving_word);
    naula_memory_release(subject->serving_host);
    naula_repo_file_release(&subject->file);
}

static naula_status prepare(reconciler *subject, naula_refusal *refusal) {
    char *branch = NULL;
    naula_status status = local_identity(subject, &branch, refusal);
    if (status == NAULA_OK) {
        status = read_history(subject, branch, refusal);
    }
    naula_memory_release(branch);
    if (status != NAULA_OK) {
        return status;
    }
    status = name_the_hints(subject);
    if (status != NAULA_OK) {
        return status;
    }
    read_hints(subject);
    if (subject->history_count == 0) {
        naula_refuse(refusal, "unadopted", "run naula adopt, commit and push first", "no repo.json of format 2 is committed");
        return NAULA_REFUSED;
    }
    status = find_serving(subject, &subject->history[0], refusal);
    if (status == NAULA_OK) {
        status = read_wanted(subject, refusal);
    }
    if (status == NAULA_OK) {
        status = plan_homes(subject, refusal);
    }
    if (status == NAULA_OK) {
        status = check_both_ways(subject, refusal);
    }
    if (status == NAULA_OK) {
        status = check_targets(subject, refusal);
    }
    if (status == NAULA_OK) {
        status = check_literal_consumers(subject, refusal);
    }
    return status == NAULA_OK ? check_names_free(subject, refusal) : status;
}

naula_status naula_reconcile(naula_session *session, const char *repository, const naula_reconcile_options *options,
                             naula_reconcile_report *report, naula_refusal *refusal) {
    uint32_t size = report->size;
    memset(report, 0, sizeof *report);
    report->size = size;
    report->first_failure.size = sizeof report->first_failure;
    if (options != NULL &&
        options->size < offsetof(naula_reconcile_options, accepted_home_count) + sizeof options->accepted_home_count) {
        naula_refuse(refusal, "invalid_options", "set size to sizeof(naula_reconcile_options)",
                     "the options are smaller than their first shape");
        return NAULA_REFUSED;
    }
    reconciler subject;
    memset(&subject, 0, sizeof subject);
    subject.session = session;
    subject.repository = repository;
    subject.options = options;
    subject.report = report;
    subject.file.size = sizeof subject.file;
    naula_status status = prepare(&subject, refusal);
    if (status == NAULA_OK) {
        write_hosts(&subject);
        write_hints(&subject);
        if (report->failed > 0) {
            *refusal = report->first_failure;
            naula_refuse(refusal, "reconcile-incomplete", "run naula sync again once the hosts answer",
                         "%zu of the changes %s's file asks for failed; the first: %s: %s", report->failed,
                         subject.file.name, report->first_failure.code, report->first_failure.detail);
            status = NAULA_REFUSED;
        }
    }
    drop_temporary_refs(&subject);
    release_reconciler(&subject);
    return status;
}
