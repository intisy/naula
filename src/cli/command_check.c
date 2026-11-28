#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "check_internal.h"
#include "cli.h"
#include "naula/git.h"
#include "naula/home.h"
#include "naula/memory.h"

#define ACCOUNT_LIMIT 64

void check_add(check_findings *findings, check_level level, const char *code, const char *fix, const char *format,
               ...) {
    if (findings->overflowed) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    char text[1200];
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    if (findings->count == CHECK_FINDING_LIMIT - 1) {
        findings->overflowed = 1;
        level = CHECK_ERROR;
        code = "too-many-findings";
        fix = "fix the findings listed, then run the check again to see the rest";
        snprintf(text, sizeof text, "more than %d findings: the rest were not recorded, so the gate fails",
                 CHECK_FINDING_LIMIT - 1);
    }
    check_finding *finding = &findings->items[findings->count];
    memset(finding, 0, sizeof *finding);
    finding->level = level;
    snprintf(finding->code, sizeof finding->code, "%s", code);
    finding->fix = fix;
    finding->detail = naula_memory_duplicate_text(text);
    findings->out_of_memory = findings->out_of_memory || finding->detail == NULL;
    findings->count++;
}

void check_attach_line(check_findings *findings, char *line) {
    if (findings->count == 0 || line == NULL || findings->overflowed) {
        findings->out_of_memory = findings->out_of_memory || line == NULL;
        naula_memory_release(line);
        return;
    }
    check_finding *finding = &findings->items[findings->count - 1];
    naula_memory_release(finding->line);
    finding->line = line;
}

void check_findings_release(check_findings *findings) {
    for (size_t index = 0; index < findings->count; index++) {
        naula_memory_release(findings->items[index].detail);
        naula_memory_release(findings->items[index].line);
    }
    findings->count = 0;
}

static void add_refusal(check_findings *findings, naula_status status, const naula_refusal *refusal) {
    if (status == NAULA_OUT_OF_MEMORY) {
        findings->out_of_memory = 1;
        return;
    }
    check_add(findings, CHECK_ERROR, refusal->code[0] == '\0' ? "refused" : refusal->code, refusal->fix, "%s",
              refusal->detail);
}

typedef struct check_run {
    const cli_arguments *arguments;
    char *repository;
    naula_repo_file file;
    int has_file;
    char own_id[80];
    check_findings findings;
} check_run;

typedef enum repo_match { MATCH_UNHEARD = 0, MATCH_THIS_REPO = 1, MATCH_OTHER_REPO = 2 } repo_match;

static int is_unheard(naula_status status) {
    return status == NAULA_TIMED_OUT || status == NAULA_UNREACHABLE;
}

/** CI's `before` for a branch that did not exist yet: forty or sixty-four zeros, meaning there is no previous tip. */
static int names_no_commit(const char *commit) {
    size_t length = strlen(commit);
    return (length == 40 || length == 64) && strspn(commit, "0") == length;
}

/**
 * A shallow checkout's oldest commit has its parents cut off, so git reads it as the root and every id check would
 * judge the shallow boundary instead of the repo; actions/checkout makes one unless told `fetch-depth: 0`.
 */
static int is_shallow(check_run *run) {
    const char *arguments[] = {"rev-parse", "--is-shallow-repository", NULL};
    naula_run_result result;
    naula_refusal refusal = cli_fresh_refusal();
    if (cli_git(run->repository, arguments, &result, &refusal) != NAULA_OK) {
        return 0;
    }
    int shallow = strncmp(result.output, "true", 4) == 0;
    naula_run_result_release(&result);
    if (shallow) {
        check_add(&run->findings, CHECK_ERROR, "shallow-checkout",
                  "check the repo out with its whole history (actions/checkout: fetch-depth: 0)",
                  "the checkout is shallow, so its oldest commit would be read as the root and the id is unknown");
    }
    return shallow;
}

static naula_status read_repo_file(check_run *run) {
    char *path = cli_join(run->repository, "repo.json");
    if (path == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    uint8_t *bytes = NULL;
    size_t length = 0;
    naula_refusal refusal = cli_fresh_refusal();
    naula_status status = cli_read_file(path, &bytes, &length, &refusal);
    naula_memory_release(path);
    if (status == NAULA_NOT_FOUND) {
        check_add(&run->findings, CHECK_NOTE, "unadopted", "run naula adopt in the repo when it is ready",
                  "the repo has no repo.json, so it is placed from its host alone");
        return NAULA_OK;
    }
    if (status == NAULA_OK) {
        run->file.size = sizeof run->file;
        status = naula_repo_file_parse(bytes, length, &run->file, &refusal);
        run->has_file = status == NAULA_OK;
    }
    naula_memory_release(bytes);
    if (status != NAULA_OK) {
        add_refusal(&run->findings, status, &refusal);
    }
    return status == NAULA_OUT_OF_MEMORY ? status : NAULA_OK;
}

/** The stated id at `commit`, NULL when its repo.json states none or it has no format 2 repo.json there. */
static char *stated_id_at(const char *repository, const char *commit) {
    char *object = cli_format("%s:repo.json", commit);
    if (object == NULL) {
        return NULL;
    }
    const char *arguments[] = {"cat-file", "blob", object, NULL};
    naula_run_result result;
    naula_refusal refusal = cli_fresh_refusal();
    naula_status status = naula_git_run(repository, arguments, NULL, 0, &result, &refusal);
    naula_memory_release(object);
    if (status != NAULA_OK) {
        return NULL;
    }
    char *stated = NULL;
    if (result.exit_code == 0) {
        naula_repo_file file;
        memset(&file, 0, sizeof file);
        file.size = sizeof file;
        if (naula_repo_file_parse((const uint8_t *)result.output, result.output_length, &file, &refusal) == NAULA_OK &&
            file.id != NULL) {
            stated = naula_memory_duplicate_text(file.id);
        }
        naula_repo_file_release(&file);
    }
    naula_run_result_release(&result);
    return stated;
}

/** The stated id must be a first-parent commit that is not the root, and the id must be the one at `--previous`. */
static void check_identity(check_run *run) {
    naula_id id;
    naula_refusal refusal = cli_fresh_refusal();
    naula_status status = naula_git_repository_id(run->repository, "HEAD", run->file.id, &id, &refusal);
    if (status != NAULA_OK) {
        add_refusal(&run->findings, status, &refusal);
        return;
    }
    if (id.kind == NAULA_ID_GIT) {
        snprintf(run->own_id, sizeof run->own_id, "git:%s", id.commit);
    }
    naula_id_release(&id);
    if (run->arguments->previous == NULL || names_no_commit(run->arguments->previous)) {
        return;
    }
    char *previous_stated = stated_id_at(run->repository, run->arguments->previous);
    refusal = cli_fresh_refusal();
    status = naula_git_check_id_kept(run->repository, run->arguments->previous, previous_stated, "HEAD",
                                     run->file.id, &refusal);
    naula_memory_release(previous_stated);
    if (status != NAULA_OK) {
        add_refusal(&run->findings, status, &refusal);
    }
}

static void check_url_forms(check_run *run) {
    for (size_t index = 0; index < run->file.home_count; index++) {
        if (run->file.homes[index].kind == NAULA_HOME_URL) {
            check_add(&run->findings, CHECK_WARNING, "url-home", "name the home by provider and account",
                      "homes[%zu] is the url %s, which a rename cannot follow", index, run->file.homes[index].url);
        }
    }
    for (size_t index = 0; index < run->file.dependency_count; index++) {
        const naula_dependency *dependency = &run->file.dependencies[index];
        if (dependency->reference.url != NULL) {
            check_add(&run->findings, CHECK_WARNING, "url-dependency", "name the dependency by its id and at",
                      "dependencies[%zu] (%s) names the url %s, which a rename cannot follow", index,
                      dependency->import_name, dependency->reference.url);
        }
    }
}

static size_t collect_accounts(const naula_repo_file *file, const char **accounts) {
    size_t count = 0;
    for (size_t index = 0; index < file->home_count && count < ACCOUNT_LIMIT; index++) {
        if (file->homes[index].kind == NAULA_HOME_PROVIDER) {
            accounts[count++] = file->homes[index].account;
        }
    }
    for (size_t index = 0; file->org != NULL && index < file->org->account_count && count < ACCOUNT_LIMIT; index++) {
        if (file->org->accounts[index].kind == NAULA_HOME_PROVIDER) {
            accounts[count++] = file->org->accounts[index].account;
        }
    }
    return count;
}

static int at_names(const naula_reference *reference, const char *text) {
    for (size_t index = 0; index < reference->at_count; index++) {
        char *each = naula_place_text(&reference->at[index]);
        int same = each != NULL && strcmp(each, text) == 0;
        naula_memory_release(each);
        if (same) {
            return 1;
        }
    }
    return 0;
}

/** `"at": [...]` naming every confirmed home in the resolution's order, the line the gate asks to commit. */
static char *at_line(const naula_resolution *resolution) {
    char *line = naula_memory_duplicate_text("\"at\": [");
    for (size_t index = 0; line != NULL && index < resolution->home_count; index++) {
        char *text = naula_place_text(&resolution->homes[index]);
        char *longer = text == NULL ? NULL : cli_format("%s%s\"%s\"", line, index == 0 ? "" : ", ", text);
        naula_memory_release(text);
        naula_memory_release(line);
        line = longer;
    }
    char *closed = line == NULL ? NULL : cli_format("%s]", line);
    naula_memory_release(line);
    return closed;
}

static void weigh_resolution(check_run *run, size_t index, const naula_resolution *resolution) {
    const naula_dependency *dependency = &run->file.dependencies[index];
    if (resolution->state == NAULA_STALE) {
        check_add(&run->findings, CHECK_WARNING, "dependency-stale", "run the check again once its homes answer",
                  "dependencies[%zu] (%s) answered only from the cache", index, dependency->import_name);
        return;
    }
    size_t named = 0;
    for (size_t home = 0; home < resolution->home_count; home++) {
        char *text = naula_place_text(&resolution->homes[home]);
        named += text != NULL && at_names(&dependency->reference, text);
        naula_memory_release(text);
    }
    if (named < resolution->home_count) {
        check_add(&run->findings, CHECK_WARNING, "at-narrow", "commit the line below in that dependency",
                  "dependencies[%zu] (%s) names %zu of its %zu confirmed homes in at", index, dependency->import_name,
                  named, resolution->home_count);
        check_attach_line(&run->findings, at_line(resolution));
    }
}

static void check_dependencies(check_run *run, cli_context *context) {
    for (size_t index = 0; index < run->file.dependency_count; index++) {
        const naula_dependency *dependency = &run->file.dependencies[index];
        if (dependency->reference.id == NULL) {
            continue;
        }
        naula_resolution resolution;
        naula_refusal refusal = cli_fresh_refusal();
        naula_status status = cli_resolve(context, &dependency->reference, &resolution, &refusal);
        if (status == NAULA_OK) {
            weigh_resolution(run, index, &resolution);
        } else if (status == NAULA_TIMED_OUT || status == NAULA_UNREACHABLE) {
            check_add(&run->findings, CHECK_WARNING, "dependency-unverifiable", "run the check again once its homes answer",
                      "dependencies[%zu] (%s) could not be heard: %s: %s", index, dependency->import_name, refusal.code,
                      refusal.detail);
        } else if (status == NAULA_OUT_OF_MEMORY) {
            run->findings.out_of_memory = 1;
        } else {
            check_add(&run->findings, CHECK_ERROR, "dependency-unresolved", refusal.fix,
                      "dependencies[%zu] (%s): %s: %s", index, dependency->import_name, refusal.code, refusal.detail);
        }
        naula_resolution_release(&resolution);
    }
}

/**
 * For a host repo that has no format 2 repo.json yet, as the adoption change of an existing repo meets it: the id its
 * own history gives, with our stated id taken as the fork exception, compared with ours.
 */
static repo_match compare_unadopted(check_run *run, cli_context *context, const naula_place *place) {
    naula_repository repository;
    naula_refusal refusal = cli_fresh_refusal();
    naula_status status = cli_history_lookup(context->history, place, &repository, &refusal);
    if (status != NAULA_OK) {
        return is_unheard(status) ? MATCH_UNHEARD : MATCH_OTHER_REPO;
    }
    naula_id id;
    char *branch = NULL;
    status = cli_history_id(context->history, repository.clone_url, repository.default_branch, NULL, &id, &branch,
                            &refusal);
    uint8_t *bytes = NULL;
    size_t length = 0;
    int adopted = 0;
    if (status == NAULA_OK) {
        naula_status read = cli_read_home_file(context, place, branch, &bytes, &length, &refusal);
        naula_repo_file file;
        memset(&file, 0, sizeof file);
        file.size = sizeof file;
        naula_refusal parsed = cli_fresh_refusal();
        adopted = read == NAULA_OK && naula_repo_file_parse(bytes, length, &file, &parsed) == NAULA_OK;
        naula_repo_file_release(&file);
        status = read == NAULA_OK || read == NAULA_NOT_FOUND || read == NAULA_REFUSED ? NAULA_OK : read;
    }
    naula_memory_release(bytes);
    if (status == NAULA_OK && !adopted && run->file.id != NULL) {
        naula_id_release(&id);
        naula_memory_release(branch);
        branch = NULL;
        status = cli_history_id(context->history, repository.clone_url, repository.default_branch, run->file.id, &id,
                                &branch, &refusal);
    }
    repo_match match = is_unheard(status) || status == NAULA_NOT_FOUND ? MATCH_UNHEARD : MATCH_OTHER_REPO;
    if (status == NAULA_OK && !adopted) {
        char text[80];
        snprintf(text, sizeof text, "git:%s", id.commit);
        match = id.kind == NAULA_ID_GIT && strcmp(text, run->own_id) == 0 ? MATCH_THIS_REPO : MATCH_OTHER_REPO;
    }
    if (status == NAULA_OK) {
        naula_id_release(&id);
    }
    naula_memory_release(branch);
    naula_repository_release(&repository);
    return match;
}

/** Whether the repo the host holds under the name is this one: its id must confirm that home. */
static repo_match is_this_repo(check_run *run, cli_context *context, const naula_home *home, uint64_t number) {
    if (run->own_id[0] == '\0') {
        return MATCH_UNHEARD;
    }
    naula_place place;
    memset(&place, 0, sizeof place);
    place.size = sizeof place;
    place.kind = NAULA_HOME_PROVIDER;
    place.provider = home->provider;
    place.host = home->host;
    place.number = number;
    naula_reference reference;
    memset(&reference, 0, sizeof reference);
    reference.size = sizeof reference;
    reference.id = run->own_id;
    reference.at = &place;
    reference.at_count = 1;
    naula_resolution resolution;
    naula_refusal refusal = cli_fresh_refusal();
    naula_status status = cli_resolve(context, &reference, &resolution, &refusal);
    int ours = 0;
    for (size_t index = 0; status == NAULA_OK && index < resolution.home_count; index++) {
        ours = ours || resolution.homes[index].number == number;
    }
    naula_resolution_release(&resolution);
    if (ours) {
        return MATCH_THIS_REPO;
    }
    return is_unheard(status) ? MATCH_UNHEARD : compare_unadopted(run, context, &place);
}

static void check_name_free(check_run *run, cli_context *context) {
    for (size_t index = 0; index < run->file.home_count; index++) {
        const naula_home *home = &run->file.homes[index];
        if (home->kind != NAULA_HOME_PROVIDER) {
            continue;
        }
        naula_refusal refusal = cli_fresh_refusal();
        const naula_provider *provider = naula_providers_find(context->providers, home->provider, &refusal);
        uint64_t number = 0;
        naula_status status = provider == NULL ? NAULA_REFUSED
                                               : provider->find(provider->context, home->host, home->account,
                                                                run->file.name,
                                                                cli_credentials_token(context->credentials,
                                                                                      home->provider, home->host),
                                                                &number, &refusal);
        repo_match match = status == NAULA_OK && number != 0 ? is_this_repo(run, context, home, number) : MATCH_THIS_REPO;
        if (status != NAULA_OK) {
            check_add(&run->findings, CHECK_WARNING, "name-unverifiable", "run the check again once the home answers",
                      "whether homes[%zu] already holds the name %s is unknown: %s: %s", index, run->file.name,
                      refusal.code, refusal.detail);
        } else if (match == MATCH_UNHEARD) {
            check_add(&run->findings, CHECK_WARNING, "name-unverifiable", "run the check again once the home answers",
                      "%s account %s holds a repository named %s (number %llu), and whether it is this repo is unknown",
                      home->provider, home->account, run->file.name, (unsigned long long)number);
        } else if (match == MATCH_OTHER_REPO) {
            check_add(&run->findings, CHECK_ERROR, "name-taken", "choose another name, or retire the repo holding it",
                      "%s account %s already holds a repository named %s (number %llu) that is not this repo",
                      home->provider, home->account, run->file.name, (unsigned long long)number);
        }
    }
}

static void check_homes(check_run *run) {
    cli_context context;
    naula_refusal refusal = cli_fresh_refusal();
    naula_status status = cli_context_open(run->arguments, &context, &refusal);
    if (status != NAULA_OK) {
        add_refusal(&run->findings, status, &refusal);
        return;
    }
    check_dependencies(run, &context);
    check_name_free(run, &context);
    for (size_t index = 0; index < context.question_count; index++) {
        check_add(&run->findings, CHECK_WARNING, "needs-acceptance", NULL, "%s", context.questions[index]);
    }
    cli_context_close(&context);
}

static const char *level_name(check_level level) {
    return level == CHECK_ERROR ? "error" : level == CHECK_WARNING ? "warning" : "note";
}

static size_t error_count(const check_findings *findings) {
    size_t errors = 0;
    for (size_t index = 0; index < findings->count; index++) {
        errors += findings->items[index].level == CHECK_ERROR;
    }
    return errors;
}

static void print_text(const check_findings *findings) {
    for (size_t index = 0; index < findings->count; index++) {
        const check_finding *finding = &findings->items[index];
        FILE *stream = finding->level == CHECK_ERROR ? stderr : stdout;
        fprintf(stream, "%s %s: %s\n", level_name(finding->level), finding->code, finding->detail);
        if (finding->line != NULL) {
            fprintf(stream, "  %s\n", finding->line);
        }
        if (finding->level == CHECK_ERROR && finding->fix != NULL) {
            fprintf(stream, "  fix: %s\n", finding->fix);
        }
    }
}

static int print_json(const check_findings *findings) {
    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_CreateArray();
    int made = root != NULL && list != NULL && cJSON_AddItemToObject(root, "findings", list);
    if (!made) {
        cJSON_Delete(list);
    }
    for (size_t index = 0; made && index < findings->count; index++) {
        const check_finding *finding = &findings->items[index];
        cJSON *item = cJSON_CreateObject();
        if (item == NULL || !cJSON_AddItemToArray(list, item)) {
            cJSON_Delete(item);
            made = 0;
            break;
        }
        made = cJSON_AddStringToObject(item, "level", level_name(finding->level)) != NULL &&
               cJSON_AddStringToObject(item, "code", finding->code) != NULL &&
               cJSON_AddStringToObject(item, "detail", finding->detail) != NULL &&
               (finding->fix == NULL || cJSON_AddStringToObject(item, "fix", finding->fix) != NULL) &&
               (finding->line == NULL || cJSON_AddStringToObject(item, "line", finding->line) != NULL);
    }
    made = made && cJSON_AddBoolToObject(root, "passed", error_count(findings) == 0) != NULL;
    char *printed = made ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (printed == NULL) {
        return 0;
    }
    printf("%s\n", printed);
    cJSON_free(printed);
    return 1;
}

static void run_checks(check_run *run) {
    if (read_repo_file(run) != NAULA_OK || !run->has_file) {
        return;
    }
    if (!is_shallow(run)) {
        check_identity(run);
    }
    check_url_forms(run);
    const char *accounts[ACCOUNT_LIMIT];
    size_t account_count = collect_accounts(&run->file, accounts);
    naula_refusal refusal = cli_fresh_refusal();
    naula_status status = check_tracked_files(run->repository, accounts, account_count, &run->findings, &refusal);
    if (status != NAULA_OK) {
        add_refusal(&run->findings, status, &refusal);
    }
    if (run->arguments->offline) {
        check_add(&run->findings, CHECK_NOTE, "offline", NULL,
                  "offline: no dependency was resolved and no home was asked whether it holds the name");
    } else {
        check_homes(run);
    }
}

int cli_command_check(int count, char **values) {
    cli_arguments arguments;
    if (!cli_parse_arguments("check", count, values,
                             CLI_ALLOW_REPO | CLI_ALLOW_PREVIOUS | CLI_ALLOW_JSON | CLI_ALLOW_OFFLINE | CLI_ALLOW_ACCEPT, 0,
                             0, &arguments)) {
        return CLI_USAGE;
    }
    check_run run;
    memset(&run, 0, sizeof run);
    run.arguments = &arguments;
    run.repository = cli_absolute_path(arguments.repo == NULL ? "." : arguments.repo);
    if (run.repository == NULL) {
        naula_refusal refusal = cli_fresh_refusal();
        return cli_refused(NAULA_OUT_OF_MEMORY, &refusal);
    }
    run_checks(&run);
    int exit_code = CLI_DONE;
    if (run.findings.out_of_memory) {
        naula_refusal refusal = cli_fresh_refusal();
        exit_code = cli_refused(NAULA_OUT_OF_MEMORY, &refusal);
    } else {
        int printed = 1;
        if (arguments.json) {
            printed = print_json(&run.findings);
        } else {
            print_text(&run.findings);
        }
        exit_code = !printed || error_count(&run.findings) > 0 ? CLI_REFUSED : CLI_DONE;
    }
    check_findings_release(&run.findings);
    naula_repo_file_release(&run.file);
    naula_memory_release(run.repository);
    return exit_code;
}
