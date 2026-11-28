#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "naula/git.h"
#include "naula/memory.h"
#include "repo_file_internal.h"

#define REMOTE_LIMIT 16
#define LINE_LIMIT 100000

typedef struct adoption {
    const cli_arguments *arguments;
    char *repository;
    char *branch;
    char root[65];
    naula_repo_file file;
    cli_context context;
    int context_open;
    uint64_t primary_number;
} adoption;

/** A remote's account and name, from the host its url names; the test build also knows its own forge. */
static int home_for_url(const char *url, naula_home *home, char **name) {
    memset(home, 0, sizeof *home);
    home->size = sizeof *home;
    home->kind = NAULA_HOME_PROVIDER;
    *name = NULL;
#if defined(NAULA_TEST_HOOKS)
    if (cli_test_forge_repo_for_url(url, &home->account, name)) {
        home->provider = naula_memory_duplicate_text("forge");
        return home->provider != NULL;
    }
    naula_memory_release(home->account);
    naula_memory_release(*name);
    home->account = NULL;
    *name = NULL;
#endif
    static const char *const hosts[][2] = {{"https://github.com/", "github"}, {"https://codeberg.org/", "forgejo"}};
    for (size_t index = 0; index < sizeof hosts / sizeof hosts[0]; index++) {
        size_t prefix = strlen(hosts[index][0]);
        const char *path = url;
        if (strncmp(url, hosts[index][0], prefix) != 0) {
            continue;
        }
        path += prefix;
        const char *slash = strchr(path, '/');
        if (slash == NULL || slash == path || slash[1] == '\0' || strchr(slash + 1, '/') != NULL) {
            return 0;
        }
        size_t name_length = strlen(slash + 1);
        if (name_length > 4 && strcmp(slash + 1 + name_length - 4, ".git") == 0) {
            name_length -= 4;
        }
        home->provider = naula_memory_duplicate_text(hosts[index][1]);
        home->host = index == 0 ? NULL : naula_memory_duplicate_text("codeberg.org");
        home->account = cli_format("%.*s", (int)(slash - path), path);
        *name = cli_format("%.*s", (int)name_length, slash + 1);
        return home->provider != NULL && home->account != NULL && *name != NULL && (index == 0 || home->host != NULL);
    }
    return 0;
}

static int same_home(const naula_home *left, const naula_home *right) {
    return left->kind == NAULA_HOME_PROVIDER && right->kind == NAULA_HOME_PROVIDER &&
           strcmp(left->provider, right->provider) == 0 &&
           ((left->host == NULL && right->host == NULL) ||
            (left->host != NULL && right->host != NULL && strcmp(left->host, right->host) == 0)) &&
           strcmp(left->account, right->account) == 0;
}

/** Every remote's url, `origin` first, each read from git and never from a file naula parses. */
static naula_status remote_urls(const char *repository, char **urls, size_t *count, naula_refusal *refusal) {
    const char *list[] = {"remote", NULL};
    naula_run_result result;
    *count = 0;
    naula_status status = cli_git(repository, list, &result, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    char *names[REMOTE_LIMIT];
    size_t name_count = 0;
    for (char *line = strtok(result.output, "\r\n"); line != NULL && name_count < REMOTE_LIMIT;
         line = strtok(NULL, "\r\n")) {
        names[name_count++] = line;
    }
    for (size_t index = 1; index < name_count; index++) {
        if (strcmp(names[index], "origin") == 0) {
            char *first = names[0];
            names[0] = names[index];
            names[index] = first;
        }
    }
    for (size_t index = 0; status == NAULA_OK && index < name_count; index++) {
        const char *get[] = {"remote", "get-url", names[index], NULL};
        naula_run_result url;
        status = cli_git(repository, get, &url, refusal);
        if (status == NAULA_OK) {
            url.output[strcspn(url.output, "\r\n")] = '\0';
            urls[*count] = naula_memory_duplicate_text(url.output);
            status = urls[*count] == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
            *count += status == NAULA_OK;
            naula_run_result_release(&url);
        }
    }
    naula_run_result_release(&result);
    return status;
}

static naula_status add_home(naula_repo_file *file, naula_home *home) {
    naula_home *grown = naula_memory_reallocate(file->homes, (file->home_count + 1) * sizeof *grown);
    if (grown == NULL) {
        naula_home_release(home);
        return NAULA_OUT_OF_MEMORY;
    }
    file->homes = grown;
    file->homes[file->home_count++] = *home;
    return NAULA_OK;
}

/**
 * `homes` from the file's format 1 `hosts`, else from the remotes by account; `name` from the primary's host. A
 * remote whose host naula cannot tell is left out, and no home at all is refused.
 */
static naula_status take_homes_and_name(adoption *run, naula_refusal *refusal) {
    char *urls[REMOTE_LIMIT];
    size_t url_count = 0;
    naula_status status = remote_urls(run->repository, urls, &url_count, refusal);
    char *names[REMOTE_LIMIT];
    naula_home found[REMOTE_LIMIT];
    size_t found_count = 0;
    for (size_t index = 0; status == NAULA_OK && index < url_count; index++) {
        if (home_for_url(urls[index], &found[found_count], &names[found_count])) {
            found_count++;
        } else {
            naula_home_release(&found[found_count]);
            naula_memory_release(names[found_count]);
            if (strncmp(urls[index], "naula::", 7) != 0) {
                printf("note remote-not-a-home: %s names no host naula knows, so it is no home\n", urls[index]);
            }
        }
    }
    int from_hosts = run->file.home_count > 0;
    for (size_t index = 0; status == NAULA_OK && !from_hosts && index < found_count; index++) {
        int repeated = 0;
        for (size_t other = 0; other < run->file.home_count; other++) {
            repeated = repeated || same_home(&run->file.homes[other], &found[index]);
        }
        if (!repeated) {
            naula_home copy = found[index];
            memset(&found[index], 0, sizeof found[index]);
            status = add_home(&run->file, &copy);
        }
    }
    if (status == NAULA_OK && run->file.home_count == 0) {
        naula_refuse(refusal, "homes-unknown", "add a remote on github.com or codeberg.org, or write homes by hand",
                     "no remote of the repo names a home naula knows");
        status = NAULA_REFUSED;
    }
    const char *remote_name = NULL;
    for (size_t index = 0; status == NAULA_OK && index < found_count && remote_name == NULL; index++) {
        if (run->file.homes[0].kind == NAULA_HOME_PROVIDER && found[index].provider != NULL &&
            strcmp(found[index].provider, run->file.homes[0].provider) == 0 &&
            strcmp(found[index].account, run->file.homes[0].account) == 0) {
            remote_name = names[index];
        }
    }
    if (status == NAULA_OK && remote_name == NULL) {
        naula_refuse(refusal, "name-unknown", "add a remote that reaches the primary home, then run again",
                     "no remote reaches the primary home, so its name on the host is unknown");
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK) {
        const naula_home *primary = &run->file.homes[0];
        const naula_provider *provider = naula_providers_find(run->context.providers, primary->provider, refusal);
        const char *token = cli_credentials_token(run->context.credentials, primary->provider, primary->host);
        uint64_t number = 0;
        status = provider == NULL ? NAULA_REFUSED
                                  : provider->find(provider->context, primary->host, primary->account, remote_name,
                                                   token, &number, refusal);
        naula_repository repository;
        memset(&repository, 0, sizeof repository);
        repository.size = sizeof repository;
        if (status == NAULA_OK && number == 0) {
            naula_refuse(refusal, "name-unknown", "check the remote reaches the primary home",
                         "%s account %s holds no repository %s", primary->provider, primary->account, remote_name);
            status = NAULA_REFUSED;
        }
        if (status == NAULA_OK) {
            status = provider->lookup(provider->context, primary->host, number, token, &repository, refusal);
        }
        if (status == NAULA_OK) {
            run->primary_number = number;
            naula_memory_release(run->file.name);
            run->file.name = naula_memory_duplicate_text(repository.name);
            status = run->file.name == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
        }
        naula_repository_release(&repository);
    }
    for (size_t index = 0; index < found_count; index++) {
        naula_home_release(&found[index]);
        naula_memory_release(names[index]);
    }
    for (size_t index = 0; index < url_count; index++) {
        naula_memory_release(urls[index]);
    }
    return status;
}

/** Each format 1 forge id becomes `{ id, at }`, the id derived from that repo's own history and its commit checked. */
static naula_status convert_dependency(adoption *run, naula_dependency *dependency, naula_refusal *refusal) {
    naula_reference *reference = &dependency->reference;
    if (reference->id == NULL || strncmp(reference->id, "git:", 4) == 0 || reference->at_count != 1) {
        return NAULA_OK;
    }
    char *derived = NULL;
    naula_status status = cli_derive_id(&run->context, &reference->at[0], &derived, refusal);
    const char *segments = strchr(reference->id, '/');
    char *id = status == NAULA_OK ? cli_format("%s%s", derived, segments == NULL ? "" : segments) : NULL;
    status = status != NAULA_OK ? status : id == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    naula_repository repository;
    memset(&repository, 0, sizeof repository);
    repository.size = sizeof repository;
    if (status == NAULA_OK) {
        status = cli_history_lookup(run->context.history, &reference->at[0], &repository, refusal);
    }
    if (status == NAULA_OK) {
        status = cli_history_check_commit(run->context.history, repository.clone_url, dependency->commit, derived,
                                          refusal);
    }
    if (status == NAULA_OK) {
        naula_memory_release(reference->id);
        reference->id = id;
        id = NULL;
    } else if (status != NAULA_OUT_OF_MEMORY) {
        naula_refusal heard = *refusal;
        naula_refuse(refusal, "dependency-unconverted", "fix the dependency's id or commit by hand, then run again",
                     "the dependency %s (%s) cannot become an id: %s: %s", dependency->import_name, reference->id,
                     heard.code, heard.detail);
    }
    naula_repository_release(&repository);
    naula_memory_release(id);
    naula_memory_release(derived);
    return status;
}

/** Format 1 `projects` named no org; the org takes the primary's account and its name, which adopt prints. */
static naula_status name_the_org(adoption *run) {
    naula_org_declaration *org = run->file.org;
    if (org == NULL || org->name != NULL) {
        return NAULA_OK;
    }
    const naula_home *primary = &run->file.homes[0];
    org->name = naula_memory_duplicate_text(primary->account);
    org->accounts = naula_memory_allocate(sizeof *org->accounts);
    if (org->name == NULL || org->accounts == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    memset(org->accounts, 0, sizeof *org->accounts);
    org->accounts[0].size = sizeof org->accounts[0];
    org->accounts[0].kind = NAULA_HOME_PROVIDER;
    org->accounts[0].provider = naula_memory_duplicate_text(primary->provider);
    org->accounts[0].host = primary->host == NULL ? NULL : naula_memory_duplicate_text(primary->host);
    org->accounts[0].account = naula_memory_duplicate_text(primary->account);
    org->account_count = 1;
    char *rules = cli_join(run->repository, "RULES.md");
    if (rules != NULL && cli_exists(rules)) {
        org->rules = naula_memory_duplicate_text("RULES.md");
    }
    naula_memory_release(rules);
    printf("note org-derived: the org is named %s and holds %s account %s; check both before committing\n", org->name,
           primary->provider, primary->account);
    return NAULA_OK;
}

static const naula_history *history_reader(adoption *run) {
    return cli_history_reader(run->context.history);
}

/**
 * The first commit of our first-parent line the other repo's working branch does not hold: the commit the repo that
 * takes the fork exception states. The line held is a prefix, so a binary search finds it.
 */
static naula_status first_own_commit(adoption *run, const naula_place *other, const char *other_branch, char out[65],
                                     naula_refusal *refusal) {
    out[0] = '\0';
    const char *list[] = {"rev-list", "--first-parent", "--reverse", run->branch, NULL};
    naula_run_result result;
    naula_status status = cli_git(run->repository, list, &result, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    char **commits = naula_memory_allocate(LINE_LIMIT * sizeof *commits);
    size_t count = 0;
    for (char *line = commits == NULL ? NULL : strtok(result.output, "\r\n"); line != NULL && count < LINE_LIMIT;
         line = strtok(NULL, "\r\n")) {
        commits[count++] = line;
    }
    status = commits == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    const naula_history *history = history_reader(run);
    size_t low = 0;
    size_t high = count;
    while (status == NAULA_OK && low < high) {
        size_t middle = low + (high - low) / 2;
        int contained = 0;
        status = history->contains(history->context, other, other_branch, commits[middle], &contained, refusal);
        if (status == NAULA_REFUSED && strcmp(refusal->code, "missing_commit_or_ref") == 0) {
            contained = 0;
            status = NAULA_OK;
        }
        if (contained) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (status == NAULA_OK && low > 0 && low < count) {
        snprintf(out, 65, "%s", commits[low]);
    }
    naula_memory_release(commits);
    naula_run_result_release(&result);
    return status;
}

/**
 * Another repo of the primary's account, or the repo the primary was forked from, whose id is ours: adopt proposes
 * the fork exception and decides nothing, since which of the two takes it is a person's call.
 */
static naula_status refuse_a_shared_root(adoption *run, naula_refusal *refusal) {
    const naula_home *primary = &run->file.homes[0];
    const naula_provider *provider = naula_providers_find(run->context.providers, primary->provider, refusal);
    if (provider == NULL) {
        return NAULA_REFUSED;
    }
    const char *token = cli_credentials_token(run->context.credentials, primary->provider, primary->host);
    naula_repository *repositories = NULL;
    size_t count = 0;
    naula_status status =
        provider->list_repos(provider->context, primary->host, primary->account, token, &repositories, &count, refusal);
    char own[80];
    snprintf(own, sizeof own, "git:%s", run->root);
    for (size_t index = 0; status == NAULA_OK && index < count; index++) {
        const naula_repository *other = &repositories[index];
        if (other->number == run->primary_number || other->archived) {
            continue;
        }
        naula_place place;
        memset(&place, 0, sizeof place);
        place.size = sizeof place;
        place.kind = NAULA_HOME_PROVIDER;
        place.provider = primary->provider;
        place.host = primary->host;
        place.number = other->number;
        char *id = NULL;
        naula_refusal ignored = cli_fresh_refusal();
        naula_status derived = cli_derive_id(&run->context, &place, &id, &ignored);
        if (derived == NAULA_OUT_OF_MEMORY) {
            status = derived;
        } else if (derived == NAULA_TIMED_OUT || derived == NAULA_UNREACHABLE) {
            *refusal = ignored;
            status = derived;
        }
        if (status == NAULA_OK && derived == NAULA_OK && strcmp(id, own) == 0) {
            naula_id parsed;
            char *branch = NULL;
            status = cli_history_id(run->context.history, other->clone_url, other->default_branch, NULL, &parsed,
                                    &branch, refusal);
            char proposal[65] = "";
            if (status == NAULA_OK) {
                naula_id_release(&parsed);
                status = first_own_commit(run, &place, branch, proposal, refusal);
            }
            naula_memory_release(branch);
            if (status == NAULA_OK) {
                if (proposal[0] != '\0') {
                    naula_refuse(refusal, "root-shared",
                                 "agree which of the two takes its own identity; in that repo, the one created later, "
                                 "run naula adopt --fork-id <its first own commit>",
                                 "%s/%s (number %llu) has the same first-parent root %s; proposed: naula adopt "
                                 "--fork-id %s",
                                 other->account, other->name, (unsigned long long)other->number, run->root, proposal);
                } else {
                    naula_refuse(refusal, "root-shared",
                                 "run naula adopt --fork-id <its first own commit> in the other repo, whose history "
                                 "holds all of this one",
                                 "%s/%s (number %llu) has the same first-parent root %s and holds every commit of "
                                 "this repo's line",
                                 other->account, other->name, (unsigned long long)other->number, run->root);
                }
                status = NAULA_REFUSED;
            }
        }
        naula_memory_release(id);
    }
    naula_repository_list_release(repositories, count);
    return status;
}

static naula_status take_fork_id(adoption *run, naula_refusal *refusal) {
    char *stated = cli_format("git:%s", run->arguments->fork_id);
    if (stated == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    naula_id id;
    naula_status status = naula_git_repository_id(run->repository, run->branch, stated, &id, refusal);
    if (status == NAULA_OK) {
        naula_id_release(&id);
        naula_memory_release(run->file.id);
        run->file.id = stated;
        return NAULA_OK;
    }
    naula_memory_release(stated);
    return status;
}

static naula_status convert(adoption *run, naula_refusal *refusal) {
    naula_status status = cli_context_open(run->arguments, &run->context, refusal);
    run->context_open = status == NAULA_OK;
    if (status == NAULA_OK) {
        status = take_homes_and_name(run, refusal);
    }
    for (size_t index = 0; status == NAULA_OK && index < run->file.dependency_count; index++) {
        status = convert_dependency(run, &run->file.dependencies[index], refusal);
    }
    if (status == NAULA_OK) {
        status = name_the_org(run);
    }
    if (status == NAULA_OK && run->arguments->fork_id != NULL) {
        status = take_fork_id(run, refusal);
    } else if (status == NAULA_OK) {
        status = naula_git_first_parent_root(run->repository, run->branch, run->root, refusal);
        if (status == NAULA_OK) {
            status = refuse_a_shared_root(run, refusal);
        }
    }
    run->file.format = 2;
    return status;
}

static naula_status read_existing(adoption *run, int *adopted, naula_refusal *refusal) {
    char *path = cli_join(run->repository, "repo.json");
    uint8_t *bytes = NULL;
    size_t length = 0;
    *adopted = 0;
    naula_status status = path == NULL ? NAULA_OUT_OF_MEMORY : cli_read_file(path, &bytes, &length, refusal);
    naula_memory_release(path);
    memset(&run->file, 0, sizeof run->file);
    run->file.size = sizeof run->file;
    if (status == NAULA_NOT_FOUND) {
        naula_refuse(refusal, "no-repo-file", "write a repo.json naming the repo's importName first",
                     "the repo has no repo.json, so its importName is unknown");
        return NAULA_REFUSED;
    }
    if (status == NAULA_OK) {
        naula_refusal format_two = cli_fresh_refusal();
        status = naula_repo_file_parse(bytes, length, &run->file, &format_two);
        *adopted = status == NAULA_OK;
        if (status == NAULA_REFUSED && strcmp(format_two.code, "repo-file-format-one") == 0) {
            status = naula_repo_file_parse_format_one(bytes, length, &run->file, refusal);
        } else if (status != NAULA_OK) {
            *refusal = format_two;
        }
    }
    naula_memory_release(bytes);
    return status;
}

/**
 * `naula adopt`: writes repo.json format 2 in the working tree and commits nothing. A file already in format 2 is
 * only written again in its canonical form, so a second run changes no byte.
 */
int cli_command_adopt(int count, char **values) {
    cli_arguments arguments;
    if (!cli_parse_arguments("adopt", count, values, CLI_ALLOW_REPO | CLI_ALLOW_FORK_ID, 0, 0, &arguments)) {
        return CLI_USAGE;
    }
    if (arguments.fork_id != NULL && !repo_file_is_commit(arguments.fork_id)) {
        return cli_usage_error("adopt", "--fork-id takes a commit's full hex, not %s", arguments.fork_id);
    }
    adoption run;
    memset(&run, 0, sizeof run);
    run.arguments = &arguments;
    run.repository = cli_repository(&arguments);
    naula_refusal refusal = cli_fresh_refusal();
    naula_status status = run.repository == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    if (status == NAULA_OK) {
        status = cli_working_branch(run.repository, &run.branch, &refusal);
    }
    int adopted = 0;
    if (status == NAULA_OK) {
        status = read_existing(&run, &adopted, &refusal);
    }
    if (status == NAULA_OK && adopted && arguments.fork_id != NULL) {
        status = take_fork_id(&run, &refusal);
    } else if (status == NAULA_OK && !adopted) {
        status = convert(&run, &refusal);
    }
    int changed = 0;
    if (status == NAULA_OK) {
        status = cli_write_repo_file(run.repository, &run.file, &changed, &refusal);
    }
    if (status == NAULA_OK) {
        printf("%s repo.json\n", changed ? "wrote" : "kept");
    }
    if (run.context_open) {
        cli_print_questions(&run.context);
        cli_context_close(&run.context);
    }
    naula_repo_file_release(&run.file);
    naula_memory_release(run.branch);
    naula_memory_release(run.repository);
    return status == NAULA_OK ? CLI_DONE : cli_refused(status, &refusal);
}
