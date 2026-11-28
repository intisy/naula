#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "naula/memory.h"

/** One dependency to check out: where, from which url, at which commit, and whether it is already there. */
typedef struct sibling {
    const naula_dependency *dependency;
    char *relative;
    char *path;
    char *url;
    size_t depth;
    int kept;
} sibling;

typedef struct siblings_run {
    char *repository;
    naula_repo_file file;
    sibling *items;
    size_t count;
} siblings_run;

static size_t depth_of(const char *path) {
    size_t depth = 0;
    for (const char *at = path; *at != '\0'; at++) {
        depth += *at == '/';
    }
    return depth;
}

/** Paths compared ignoring ASCII case on every system, since a repo checked out on Windows or macOS folds them. */
static int compare_paths(const char *left, const char *right) {
    for (;; left++, right++) {
        int first = tolower((unsigned char)*left);
        int second = tolower((unsigned char)*right);
        if (first != second || first == '\0') {
            return first - second;
        }
    }
}

static int by_depth(const void *left, const void *right) {
    const sibling *first = left;
    const sibling *second = right;
    if (first->depth != second->depth) {
        return first->depth < second->depth ? -1 : 1;
    }
    return compare_paths(first->relative, second->relative);
}

static naula_status read_file(siblings_run *run, int *adopted, naula_refusal *refusal) {
    char *path = cli_join(run->repository, "repo.json");
    if (path == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    uint8_t *bytes = NULL;
    size_t length = 0;
    naula_status status = cli_read_file(path, &bytes, &length, refusal);
    naula_memory_release(path);
    *adopted = status != NAULA_NOT_FOUND;
    if (status == NAULA_OK) {
        run->file.size = sizeof run->file;
        status = naula_repo_file_parse(bytes, length, &run->file, refusal);
    }
    naula_memory_release(bytes);
    return status == NAULA_NOT_FOUND ? NAULA_OK : status;
}

/** HEAD of the checkout at `path`, compared with `commit`; anything else there is refused. */
static naula_status judge_existing(sibling *item, naula_refusal *refusal) {
    if (!cli_exists(item->path)) {
        return NAULA_OK;
    }
    const char *arguments[] = {"rev-parse", "--verify", "--quiet", "HEAD", NULL};
    naula_run_result result;
    naula_refusal ignored = cli_fresh_refusal();
    if (cli_is_folder(item->path) && cli_git(item->path, arguments, &result, &ignored) == NAULA_OK) {
        item->kept = strncmp(result.output, item->dependency->commit, strlen(item->dependency->commit)) == 0;
        naula_run_result_release(&result);
    }
    if (!item->kept) {
        naula_refuse(refusal, "sibling-occupied", "remove what is there, or check the repo out afresh",
                     "%s already holds something other than %s at %s", item->relative,
                     item->dependency->import_name, item->dependency->commit);
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

static naula_status locate(cli_context *context, sibling *item, naula_refusal *refusal) {
    const naula_dependency *dependency = item->dependency;
    if (dependency->reference.url != NULL) {
        if (!cli_clone_url_is_allowed(dependency->reference.url)) {
            naula_refuse(refusal, "clone-url-refused", "name the dependency by its id and at",
                         "%s is named by %s, which naula does not hand to git", dependency->import_name,
                         dependency->reference.url);
            return NAULA_REFUSED;
        }
        naula_status checked = cli_history_check_commit(context->history, dependency->reference.url,
                                                        dependency->commit, NULL, refusal);
        if (checked != NAULA_OK) {
            return checked;
        }
        fprintf(stdout, "warning url-dependency: %s is checked out by url, unconfirmed\n", dependency->import_name);
        item->url = naula_memory_duplicate_text(dependency->reference.url);
        return item->url == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    }
    naula_resolution resolution;
    naula_status status = cli_resolve(context, &dependency->reference, &resolution, refusal);
    if (status == NAULA_OK && (resolution.state != NAULA_CONFIRMED ||
                               !cli_clone_url_is_allowed(resolution.primary_repository.clone_url))) {
        naula_refuse(refusal, "primary-unreadable", "run again once the primary answers",
                     "%s has no primary naula can clone from in this run", dependency->import_name);
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK) {
        status = cli_history_check_commit(context->history, resolution.primary_repository.clone_url,
                                          dependency->commit, dependency->reference.id, refusal);
    }
    if (status == NAULA_OK) {
        item->url = naula_memory_duplicate_text(resolution.primary_repository.clone_url);
        status = item->url == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    }
    if (status != NAULA_OK && status != NAULA_OUT_OF_MEMORY) {
        cli_print_verdicts(&resolution);
    }
    naula_resolution_release(&resolution);
    return status;
}

static naula_status prepare(siblings_run *run, naula_refusal *refusal) {
    run->items = naula_memory_allocate((run->file.dependency_count + 1) * sizeof *run->items);
    if (run->items == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < run->file.dependency_count; index++) {
        sibling *item = &run->items[run->count++];
        memset(item, 0, sizeof *item);
        item->dependency = &run->file.dependencies[index];
        item->relative = item->dependency->path != NULL ? naula_memory_duplicate_text(item->dependency->path)
                                                        : cli_format("siblings/%s", item->dependency->import_name);
        item->path = item->relative == NULL ? NULL : cli_join(run->repository, item->relative);
        if (item->path == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        item->depth = depth_of(item->relative);
    }
    qsort(run->items, run->count, sizeof *run->items, by_depth);
    for (size_t index = 1; index < run->count; index++) {
        if (compare_paths(run->items[index].relative, run->items[index - 1].relative) == 0) {
            naula_refuse(refusal, "sibling-path-repeated", "give each dependency a path of its own",
                         "two dependencies check out into %s", run->items[index].relative);
            return NAULA_REFUSED;
        }
    }
    return NAULA_OK;
}

static naula_status locate_all(siblings_run *run, cli_context *context, naula_refusal *refusal) {
    for (size_t index = 0; index < run->count; index++) {
        naula_status status = cli_refuse_links_below(run->repository, run->items[index].relative, refusal);
        if (status == NAULA_OK) {
            status = judge_existing(&run->items[index], refusal);
        }
        if (status == NAULA_OK && !run->items[index].kept) {
            status = locate(context, &run->items[index], refusal);
        }
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

static naula_status check_out(const char *repository, const sibling *item, naula_refusal *refusal) {
    const char *clone[] = {"clone", "--quiet", "--no-checkout", "--", item->url, item->path, NULL};
    const char *checkout[] = {"checkout", "--quiet", "--detach", item->dependency->commit, NULL};
    naula_run_result result;
    naula_status status = cli_refuse_links_below(repository, item->relative, refusal);
    if (status == NAULA_OK) {
        status = cli_git(NULL, clone, &result, refusal);
    }
    if (status == NAULA_OK) {
        naula_run_result_release(&result);
        status = cli_git(item->path, checkout, &result, refusal);
    }
    if (status == NAULA_OK) {
        naula_run_result_release(&result);
        printf("clone %s %s\n", item->relative, item->dependency->commit);
    }
    return status;
}

static void release_run(siblings_run *run) {
    for (size_t index = 0; index < run->count; index++) {
        naula_memory_release(run->items[index].relative);
        naula_memory_release(run->items[index].path);
        naula_memory_release(run->items[index].url);
    }
    naula_memory_release(run->items);
    naula_repo_file_release(&run->file);
    naula_memory_release(run->repository);
}

/** Every dependency is resolved and its commit checked before the first clone, so a refusal writes nothing. */
static naula_status run_siblings(siblings_run *run, const cli_arguments *arguments, naula_refusal *refusal) {
    int adopted = 0;
    naula_status status = read_file(run, &adopted, refusal);
    if (status != NAULA_OK || !adopted || run->file.dependency_count == 0) {
        if (status == NAULA_OK) {
            printf("note: %s\n", adopted ? "the repo has no dependencies" : "the repo has no repo.json, so nothing to check out");
        }
        return status;
    }
    status = prepare(run, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    cli_context context;
    status = cli_context_open(arguments, &context, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    status = locate_all(run, &context, refusal);
    if (status != NAULA_OK) {
        cli_print_questions(&context);
    }
    cli_context_close(&context);
    for (size_t index = 0; status == NAULA_OK && index < run->count; index++) {
        if (run->items[index].kept) {
            printf("kept %s %s\n", run->items[index].relative, run->items[index].dependency->commit);
        } else {
            status = check_out(run->repository, &run->items[index], refusal);
        }
    }
    return status;
}

int cli_command_siblings(int count, char **values) {
    cli_arguments arguments;
    if (!cli_parse_arguments("siblings", count, values, CLI_ALLOW_REPO | CLI_ALLOW_OFFLINE | CLI_ALLOW_ACCEPT, 0, 0,
                             &arguments)) {
        return CLI_USAGE;
    }
    siblings_run run;
    memset(&run, 0, sizeof run);
    naula_refusal refusal = cli_fresh_refusal();
    run.repository = cli_absolute_path(arguments.repo == NULL ? "." : arguments.repo);
    naula_status status = run.repository == NULL ? NAULA_OUT_OF_MEMORY : run_siblings(&run, &arguments, &refusal);
    release_run(&run);
    return status == NAULA_OK ? CLI_DONE : cli_refused(status, &refusal);
}
