#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "naula/git.h"
#include "naula/memory.h"
#include "naula/sha256.h"
#include "repo_file_internal.h"

#define FETCHED_LIMIT 256
#define SPACE_SIZE 17

typedef enum object_format { FORMAT_SHA1 = 0, FORMAT_SHA256 = 1 } object_format;

/** One home's branches as fetched into a scratch repo, under `refs/naula/<space>/`. */
typedef struct fetched_home {
    char *url;
    char space[SPACE_SIZE];
    object_format format;
    naula_status status;
    naula_refusal refusal;
} fetched_home;

struct cli_history {
    naula_history reader;
    naula_providers *providers;
    cli_credentials *credentials;
    char *scratch[2];
    int ready[2];
    fetched_home homes[FETCHED_LIMIT];
    size_t count;
};

static const char *const scratch_names[2] = {"sha1.git", "sha256.git"};

static naula_status refuse_unreadable(const naula_place *home, naula_refusal *refusal) {
    naula_refuse(refusal, "unverifiable", "list a provider home or a url home beside it",
                 "the command line reads history through git, and a %s home names no repository to fetch",
                 home->kind == NAULA_HOME_WEB ? "web" : "numberless");
    return NAULA_REFUSED;
}

naula_status cli_history_lookup(cli_history *history, const naula_place *place, naula_repository *repository,
                                naula_refusal *refusal) {
    memset(repository, 0, sizeof *repository);
    repository->size = sizeof *repository;
    if (place->kind != NAULA_HOME_PROVIDER || place->number == 0) {
        return refuse_unreadable(place, refusal);
    }
    const naula_provider *provider = naula_providers_find(history->providers, place->provider, refusal);
    if (provider == NULL) {
        return NAULA_REFUSED;
    }
    const char *token = cli_credentials_token(history->credentials, place->provider, place->host);
    naula_status status = provider->lookup(provider->context, place->host, place->number, token, repository, refusal);
    if (status == NAULA_OK && !cli_clone_url_is_allowed(repository->clone_url)) {
        naula_refuse(refusal, "clone-url-refused", "ask the host for an https clone url",
                     "the host answered the clone url %.200s, which naula does not hand to git",
                     repository->clone_url == NULL ? "(none)" : repository->clone_url);
        naula_repository_release(repository);
        status = NAULA_REFUSED;
    }
    return status;
}

static void space_of(const char *url, char space[SPACE_SIZE]) {
    naula_sha256 hash;
    char hex[NAULA_SHA256_HEX_SIZE];
    naula_sha256_start(&hash);
    naula_sha256_add(&hash, (const uint8_t *)url, strlen(url));
    naula_sha256_hex(&hash, hex);
    memcpy(space, hex, SPACE_SIZE - 1);
    space[SPACE_SIZE - 1] = '\0';
}

static naula_status prepare_scratch(cli_history *history, object_format format, naula_refusal *refusal) {
    if (history->ready[format]) {
        return NAULA_OK;
    }
    naula_run_result result;
    naula_status status = NAULA_OK;
    if (!cli_exists(history->scratch[format])) {
        const char *sha1[] = {"init", "--quiet", "--bare", history->scratch[format], NULL};
        const char *sha256[] = {"init", "--quiet", "--bare", "--object-format=sha256", history->scratch[format], NULL};
        const char *const *arguments = format == FORMAT_SHA256 ? sha256 : sha1;
        status = cli_git(NULL, arguments, &result, refusal);
        if (status == NAULA_OK) {
            naula_run_result_release(&result);
        }
    }
    history->ready[format] = status == NAULA_OK;
    return status;
}

static int is_format_mismatch(const naula_refusal *refusal) {
    return strstr(refusal->detail, "object format") != NULL || strstr(refusal->detail, "mismatched algorithms") != NULL;
}

static naula_status fetch_into(cli_history *history, fetched_home *home, naula_refusal *refusal) {
    naula_status status = prepare_scratch(history, home->format, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    char refspec[96];
    snprintf(refspec, sizeof refspec, "+refs/heads/*:refs/naula/%s/*", home->space);
    const char *arguments[] = {"-c", "gc.auto=0", "-c", "maintenance.auto=false", "-c", "http.lowSpeedLimit=1",
                               "-c", "http.lowSpeedTime=60", "fetch", "--quiet", "--no-tags", "--prune",
                               "--filter=tree:0", home->url, refspec, NULL};
    naula_run_result result;
    status = cli_git(history->scratch[home->format], arguments, &result, refusal);
    if (status == NAULA_OK) {
        naula_run_result_release(&result);
    }
    return status;
}

/**
 * Fetches the home's branches once in a run. A fetch that fails is NAULA_UNREACHABLE, never NAULA_NOT_FOUND: a
 * provider already answered that the repository exists, so git failing says only that it could not be read now.
 */
static naula_status fetch_home(cli_history *history, const char *url, fetched_home **found, naula_refusal *refusal) {
    for (size_t index = 0; index < history->count; index++) {
        if (strcmp(history->homes[index].url, url) == 0) {
            *found = &history->homes[index];
            if (refusal != NULL && (*found)->status != NAULA_OK) {
                *refusal = (*found)->refusal;
            }
            return (*found)->status;
        }
    }
    if (history->count >= FETCHED_LIMIT) {
        naula_refuse(refusal, "too-many-homes", "resolve fewer repos in one run", "more than %d homes in one run",
                     FETCHED_LIMIT);
        return NAULA_REFUSED;
    }
    fetched_home *home = &history->homes[history->count];
    memset(home, 0, sizeof *home);
    home->url = naula_memory_duplicate_text(url);
    if (home->url == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    history->count++;
    space_of(url, home->space);
    home->refusal = cli_fresh_refusal();
    home->status = fetch_into(history, home, &home->refusal);
    if (home->status == NAULA_REFUSED && is_format_mismatch(&home->refusal)) {
        home->format = FORMAT_SHA256;
        home->refusal = cli_fresh_refusal();
        home->status = fetch_into(history, home, &home->refusal);
    }
    if (home->status == NAULA_REFUSED && strcmp(home->refusal.code, "git-failed") == 0) {
        home->status = NAULA_UNREACHABLE;
        memcpy(home->refusal.code, "fetch-failed", sizeof "fetch-failed");
    }
    *found = home;
    if (refusal != NULL && home->status != NAULA_OK) {
        *refusal = home->refusal;
    }
    return home->status;
}

static int is_safe_branch(const char *ref) {
    return ref != NULL && repo_file_is_branch(ref) && strstr(ref, "..") == NULL;
}

/** Whether `refs/naula/<space>/<branch>` names a commit in the scratch repo. */
static naula_status has_branch(cli_history *history, const fetched_home *home, const char *branch, int *exists,
                               naula_refusal *refusal) {
    char *name = cli_format("refs/naula/%s/%s^{commit}", home->space, branch);
    if (name == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    const char *arguments[] = {"rev-parse", "--verify", "--quiet", name, NULL};
    naula_run_result result;
    naula_status status = naula_git_run(history->scratch[home->format], arguments, NULL, 0, &result, refusal);
    naula_memory_release(name);
    if (status == NAULA_OK) {
        *exists = result.exit_code == 0;
        naula_run_result_release(&result);
    }
    return status;
}

static naula_status has_commit(cli_history *history, const fetched_home *home, const char *commit, int *exists,
                               naula_refusal *refusal) {
    char *name = cli_format("%s^{commit}", commit);
    if (name == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    const char *arguments[] = {"cat-file", "-e", name, NULL};
    naula_run_result result;
    naula_status status = naula_git_run(history->scratch[home->format], arguments, NULL, 0, &result, refusal);
    naula_memory_release(name);
    if (status == NAULA_OK) {
        *exists = result.exit_code == 0;
        naula_run_result_release(&result);
    }
    return status;
}

static naula_status fetch_place(cli_history *history, const naula_place *place, fetched_home **home,
                                naula_refusal *refusal) {
    naula_repository repository;
    naula_status status = cli_history_lookup(history, place, &repository, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    status = fetch_home(history, repository.clone_url, home, refusal);
    naula_repository_release(&repository);
    return status;
}

static naula_status history_contains(void *context, const naula_place *place, const char *ref, const char *commit,
                                     int *contained, naula_refusal *refusal) {
    cli_history *history = context;
    fetched_home *home = NULL;
    *contained = 0;
    if (place->kind != NAULA_HOME_PROVIDER || place->number == 0) {
        return refuse_unreadable(place, refusal);
    }
    if (!repo_file_is_commit(commit) || !is_safe_branch(ref)) {
        naula_refuse(refusal, "missing_commit_or_ref", "name a branch the home has", "the ref or the commit is not one naula reads");
        return NAULA_REFUSED;
    }
    naula_status status = fetch_place(history, place, &home, refusal);
    int exists = 0;
    if (status == NAULA_OK) {
        status = has_branch(history, home, ref, &exists, refusal);
    }
    if (status == NAULA_OK && !exists) {
        naula_refuse(refusal, "missing_commit_or_ref", "name a branch the home has", "the home has no branch %s", ref);
        return NAULA_REFUSED;
    }
    if (status == NAULA_OK) {
        status = has_commit(history, home, commit, &exists, refusal);
    }
    if (status != NAULA_OK || !exists) {
        return status;
    }
    char *tip = cli_format("refs/naula/%s/%s", home->space, ref);
    if (tip == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    status = naula_git_is_ancestor(history->scratch[home->format], commit, tip, contained, refusal);
    naula_memory_release(tip);
    return status;
}

static naula_status history_parent_count(void *context, const naula_place *place, const char *commit, size_t *count,
                                         naula_refusal *refusal) {
    cli_history *history = context;
    fetched_home *home = NULL;
    *count = 0;
    if (!repo_file_is_commit(commit)) {
        naula_refuse(refusal, "missing_commit", "name a whole commit", "%s is not a commit naula reads", commit);
        return NAULA_REFUSED;
    }
    naula_status status = fetch_place(history, place, &home, refusal);
    int exists = 0;
    if (status == NAULA_OK) {
        status = has_commit(history, home, commit, &exists, refusal);
    }
    if (status == NAULA_OK && !exists) {
        naula_refuse(refusal, "missing_commit", "name a commit the home holds", "the home does not hold %s", commit);
        return NAULA_REFUSED;
    }
    return status == NAULA_OK ? naula_git_parent_count(history->scratch[home->format], commit, count, refusal) : status;
}

naula_status cli_history_id(cli_history *history, const char *url, const char *default_branch, const char *stated_id,
                            naula_id *id, char **working_branch, naula_refusal *refusal) {
    fetched_home *home = NULL;
    *working_branch = NULL;
    memset(id, 0, sizeof *id);
    id->size = sizeof *id;
    naula_status status = fetch_home(history, url, &home, refusal);
    int exists = 0;
    if (status == NAULA_OK) {
        status = has_branch(history, home, "development", &exists, refusal);
    }
    if (status != NAULA_OK) {
        return status;
    }
    const char *branch = exists ? "development" : default_branch;
    if (!is_safe_branch(branch)) {
        naula_refuse(refusal, "no-working-branch", "give the repo a development or default branch",
                     "%s has neither a development branch nor a default branch naula reads", url);
        return NAULA_REFUSED;
    }
    char *tip = cli_format("refs/naula/%s/%s", home->space, branch);
    *working_branch = naula_memory_duplicate_text(branch);
    if (tip == NULL || *working_branch == NULL) {
        naula_memory_release(tip);
        naula_memory_release(*working_branch);
        *working_branch = NULL;
        return NAULA_OUT_OF_MEMORY;
    }
    status = naula_git_repository_id(history->scratch[home->format], tip, stated_id, id, refusal);
    naula_memory_release(tip);
    if (status != NAULA_OK) {
        naula_memory_release(*working_branch);
        *working_branch = NULL;
    }
    return status;
}

static naula_status id_at_commit(cli_history *history, const fetched_home *home, const char *commit,
                                 const char *stated_id, int *same, const naula_id *wanted, naula_refusal *refusal) {
    naula_id found;
    naula_status status = naula_git_repository_id(history->scratch[home->format], commit, stated_id, &found, refusal);
    if (status == NAULA_OK) {
        *same = naula_id_compare(&found, wanted) == 0;
        naula_id_release(&found);
    }
    return status;
}

naula_status cli_history_check_commit(cli_history *history, const char *url, const char *commit, const char *id_text,
                                      naula_refusal *refusal) {
    fetched_home *home = NULL;
    naula_id wanted;
    memset(&wanted, 0, sizeof wanted);
    wanted.size = sizeof wanted;
    if (!repo_file_is_commit(commit)) {
        naula_refuse(refusal, "commit-invalid", "pin the dependency by a whole commit", "%s is not a whole commit", commit);
        return NAULA_REFUSED;
    }
    naula_status status = naula_id_parse(id_text, strlen(id_text), &wanted, refusal);
    if (status == NAULA_OK) {
        status = fetch_home(history, url, &home, refusal);
    }
    int exists = 0;
    if (status == NAULA_OK) {
        status = has_commit(history, home, commit, &exists, refusal);
    }
    if (status == NAULA_OK && !exists) {
        naula_refuse(refusal, "commit-not-on-home", "pin a commit that a branch of the primary holds",
                     "no branch of %s holds %s", url, commit);
        status = NAULA_REFUSED;
    }
    int same = 0;
    if (status == NAULA_OK) {
        status = id_at_commit(history, home, commit, NULL, &same, &wanted, refusal);
    }
    if (status == NAULA_OK && !same) {
        naula_refusal stated = cli_fresh_refusal();
        if (id_at_commit(history, home, commit, id_text, &same, &wanted, &stated) != NAULA_OK) {
            same = 0;
        }
    }
    if (status == NAULA_OK && !same) {
        naula_refuse(refusal, "commit-not-in-id", "pin a commit of the repo the id names",
                     "%s is not a commit of %s: its first-parent line neither starts at the id nor passes it",
                     commit, id_text);
        status = NAULA_REFUSED;
    }
    naula_id_release(&wanted);
    return status;
}

naula_status cli_history_open(const char *cache_directory, naula_providers *providers, cli_credentials *credentials,
                              cli_history **opened, naula_refusal *refusal) {
    cli_history *history = naula_memory_allocate(sizeof *history);
    if (history == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    memset(history, 0, sizeof *history);
    history->providers = providers;
    history->credentials = credentials;
    char *folder = cli_join(cache_directory, "history");
    for (int format = 0; format < 2 && folder != NULL; format++) {
        history->scratch[format] = cli_join(folder, scratch_names[format]);
    }
    naula_status status = folder == NULL || history->scratch[0] == NULL || history->scratch[1] == NULL
                              ? NAULA_OUT_OF_MEMORY
                              : cli_make_folders(folder, refusal);
    naula_memory_release(folder);
    if (status != NAULA_OK) {
        cli_history_close(history);
        return status;
    }
    history->reader.size = sizeof history->reader;
    history->reader.context = history;
    history->reader.contains = history_contains;
    history->reader.parent_count = history_parent_count;
    *opened = history;
    return NAULA_OK;
}

const naula_history *cli_history_reader(cli_history *history) {
    return &history->reader;
}

void cli_history_close(cli_history *history) {
    if (history == NULL) {
        return;
    }
    for (size_t index = 0; index < history->count; index++) {
        naula_memory_release(history->homes[index].url);
    }
    naula_memory_release(history->scratch[0]);
    naula_memory_release(history->scratch[1]);
    naula_memory_release(history);
}
