#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "naula/git.h"
#include "naula/memory.h"
#include "repo_file_internal.h"
#include "sync_internal.h"

static const char *const note_codes[] = {"unadopted", "not-held", "path-outside-root", "suborg-declares-itself"};

void sync_rules_files_release(sync_request *request) {
    for (size_t index = 0; index < request->rules_file_count; index++) {
        naula_memory_release(request->rules_files[index]);
    }
    naula_memory_release(request->rules_files);
    request->rules_files = NULL;
    request->rules_file_count = 0;
}

void sync_request_release(sync_request *request) {
    sync_rules_files_release(request);
    naula_memory_release(request->root);
    naula_root_file_release(&request->root_file);
    memset(request, 0, sizeof *request);
}

static int is_rules_file(const naula_layout_step *step) {
    size_t length = step->path == NULL ? 0 : strlen(step->path);
    return step->kind == NAULA_WRITE_FILE && length >= 10 && strcmp(step->path + length - 10, "/CLAUDE.md") == 0;
}

static naula_status collect_rules_files(sync_request *request, const naula_layout_plan *plan) {
    sync_rules_files_release(request);
    request->rules_files = naula_memory_allocate((plan->step_count + 1) * sizeof *request->rules_files);
    if (request->rules_files == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < plan->step_count; index++) {
        if (is_rules_file(&plan->steps[index])) {
            char *copy = naula_memory_duplicate_text(plan->steps[index].path);
            if (copy == NULL) {
                return NAULA_OUT_OF_MEMORY;
            }
            request->rules_files[request->rules_file_count++] = copy;
        }
    }
    return NAULA_OK;
}

naula_status sync_find_root(const cli_arguments *arguments, char **root, naula_refusal *refusal) {
    *root = cli_absolute_path(arguments->root == NULL ? "." : arguments->root);
    if (*root == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    for (;;) {
        char *file = cli_join(*root, ".naula.json");
        int found = file != NULL && cli_exists(file);
        naula_memory_release(file);
        if (file == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        if (found) {
            return NAULA_OK;
        }
        char *slash = strrchr(*root, '/');
        if (arguments->root != NULL || slash == NULL || slash[1] == '\0') {
            break;
        }
        *(slash == strchr(*root, '/') ? slash + 1 : slash) = '\0';
    }
    naula_refuse(refusal, "not-a-root", "run naula init <folder> <top org home> first, or pass --root",
                 "no .naula.json was found at %s or above it", *root);
    naula_memory_release(*root);
    *root = NULL;
    return NAULA_REFUSED;
}

static naula_status grow(void **list, size_t count, size_t element) {
    void *grown = naula_memory_reallocate(*list, (count + 1) * element);
    if (grown == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    *list = grown;
    return NAULA_OK;
}

naula_status sync_root_add_held(naula_root_file *file, const naula_reference *reference) {
    naula_status status = grow((void **)&file->added, file->added_count, sizeof *file->added);
    if (status == NAULA_OK) {
        status = cli_reference_copy(reference, &file->added[file->added_count]);
    }
    file->added_count += status == NAULA_OK;
    return status;
}

naula_status sync_root_add_extra(naula_root_file *file, const naula_reference *reference) {
    naula_status status = grow((void **)&file->extra, file->extra_count, sizeof *file->extra);
    if (status == NAULA_OK) {
        status = grow((void **)&file->extra_orgs, file->extra_count, sizeof *file->extra_orgs);
    }
    if (status == NAULA_OK) {
        status = cli_reference_copy(reference, &file->extra[file->extra_count]);
    }
    if (status == NAULA_OK) {
        file->extra_orgs[file->extra_count++] = NULL;
    }
    return status;
}

naula_status sync_root_drop(naula_root_file *file, const char *name) {
    for (size_t index = 0; index < file->dropped_count; index++) {
        if (strcmp(file->dropped[index], name) == 0) {
            return NAULA_OK;
        }
    }
    char *copy = naula_memory_duplicate_text(name);
    naula_status status = copy == NULL ? NAULA_OUT_OF_MEMORY
                                       : grow((void **)&file->dropped, file->dropped_count, sizeof *file->dropped);
    if (status != NAULA_OK) {
        naula_memory_release(copy);
        return status;
    }
    file->dropped[file->dropped_count++] = copy;
    return NAULA_OK;
}

void sync_root_undrop(naula_root_file *file, const char *name) {
    for (size_t index = 0; index < file->dropped_count; index++) {
        if (strcmp(file->dropped[index], name) == 0) {
            naula_memory_release(file->dropped[index]);
            memmove(&file->dropped[index], &file->dropped[index + 1],
                    (file->dropped_count - index - 1) * sizeof *file->dropped);
            file->dropped_count--;
            return;
        }
    }
}

void sync_root_remove_added(naula_root_file *file, size_t index) {
    cli_reference_release(&file->added[index]);
    memmove(&file->added[index], &file->added[index + 1], (file->added_count - index - 1) * sizeof *file->added);
    file->added_count--;
}

static int is_note(const char *code) {
    for (size_t index = 0; index < sizeof note_codes / sizeof note_codes[0]; index++) {
        if (strcmp(code, note_codes[index]) == 0) {
            return 1;
        }
    }
    return 0;
}

static void print_reports(const naula_layout_plan *plan) {
    for (size_t index = 0; index < plan->step_count; index++) {
        const naula_layout_step *step = &plan->steps[index];
        if (step->kind == NAULA_REPORT) {
            printf("%s %s: %s%s%s: %s\n", is_note(step->code) ? "note" : "left-alone", step->code,
                   step->path == NULL ? "" : step->path, step->target == NULL ? "" : " and ",
                   step->target == NULL ? "" : step->target, step->content == NULL ? "" : step->content);
        }
    }
}

static int ends_with_checkout(const char *path, const char *ending) {
    size_t length = path == NULL ? 0 : strlen(path);
    size_t ending_length = strlen(ending);
    return length > ending_length && strcmp(path + length - ending_length, ending) == 0;
}

/** Where the plan puts the repo's checkout, `<org folder>/repos/<12 hex>`, as any of its steps names it. */
static char *planned_checkout(const naula_layout_plan *plan, const naula_id *id) {
    char folder[13];
    naula_id_folder(id, folder);
    char ending[24];
    snprintf(ending, sizeof ending, "/repos/%s", folder);
    for (size_t index = 0; folder[0] != '\0' && index < plan->step_count; index++) {
        const naula_layout_step *step = &plan->steps[index];
        if (ends_with_checkout(step->target, ending)) {
            return naula_memory_duplicate_text(step->target);
        }
        if (ends_with_checkout(step->path, ending)) {
            return naula_memory_duplicate_text(step->path);
        }
    }
    return NULL;
}

static int same_name(const char *left, const char *right) {
    for (; *left != '\0' && *right != '\0'; left++, right++) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return 0;
        }
    }
    return *left == *right;
}

/** Whether the checkout lies in the folder of the org `sync <org>` names: its org folder ends with that name. */
static int is_in_filtered_org(const sync_request *request, const naula_org_declaration *top, const char *checkout) {
    if (request->org_filter == NULL) {
        return 1;
    }
    const char *repos = strstr(checkout + strlen(request->root), "/repos/");
    if (repos == NULL) {
        return 0;
    }
    size_t folder_length = (size_t)(repos - checkout);
    if (folder_length == strlen(request->root)) {
        return top->name != NULL && same_name(top->name, request->org_filter);
    }
    const char *name = repos;
    while (name > checkout && *(name - 1) != '/') {
        name--;
    }
    return (size_t)(repos - name) == strlen(request->org_filter) &&
           strncmp(name, request->org_filter, (size_t)(repos - name)) == 0;
}

static char *parent_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash == NULL ? NULL : cli_format("%.*s", (int)(slash - path), path);
}

static naula_status clone_one(sync_repo *repo, const char *checkout, naula_refusal *refusal) {
    const char *url = repo->resolved && repo->resolution.primary_repository.clone_url != NULL
                          ? repo->resolution.primary_repository.clone_url
                          : repo->clone_url;
    if (!cli_clone_url_is_allowed(url)) {
        naula_refuse(refusal, "clone-url-refused", "ask the host for an https clone url", "%s has no clone url naula uses",
                     repo->layout.name);
        return NAULA_REFUSED;
    }
    char *parent = parent_of(checkout);
    char *remote = cli_format("naula::git:%s", repo->layout.id.commit);
    naula_status status = parent == NULL || remote == NULL ? NAULA_OUT_OF_MEMORY : cli_make_folders(parent, refusal);
    int development = repo->working_branch != NULL && strcmp(repo->working_branch, "development") == 0;
    const char *with_branch[] = {"clone", "--quiet", "--branch", "development", "--", url, checkout, NULL};
    const char *plain[] = {"clone", "--quiet", "--", url, checkout, NULL};
    const char *rename_remote[] = {"remote", "set-url", "origin", remote, NULL};
    naula_run_result result;
    if (status == NAULA_OK) {
        status = cli_git(NULL, development ? with_branch : plain, &result, refusal);
    }
    if (status == NAULA_OK) {
        naula_run_result_release(&result);
        status = cli_git(checkout, rename_remote, &result, refusal);
    }
    if (status == NAULA_OK) {
        naula_run_result_release(&result);
        printf("clone %s %s\n", repo->layout.name, checkout);
    }
    naula_memory_release(parent);
    naula_memory_release(remote);
    return status;
}

/**
 * Clones every placed repo that has no checkout yet straight into the checkout the plan gives it, so the plan made
 * afterwards moves nothing. The plan already refused every link at or above those folders.
 */
static naula_status clone_missing(const sync_request *request, sync_model *model, const naula_layout_plan *plan,
                                  int *cloned, naula_refusal *refusal) {
    for (size_t index = 0; index < model->repo_count; index++) {
        sync_repo *repo = &model->repos[index];
        if (repo->layout.current_path != NULL || repo->layout.archived || repo->layout.id.kind != NAULA_ID_GIT) {
            continue;
        }
        char *checkout = planned_checkout(plan, &repo->layout.id);
        naula_status status = NAULA_OK;
        if (checkout != NULL && is_in_filtered_org(request, model->orgs[0].declaration, checkout)) {
            status = clone_one(repo, checkout, refusal);
            if (status == NAULA_OK) {
                repo->layout.current_path = checkout;
                repo->layout.clean_and_pushed = 1;
                checkout = NULL;
                *cloned = 1;
            }
        }
        naula_memory_release(checkout);
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

static void report_applied(void *context, const naula_layout_step *step, naula_status status) {
    (void)context;
    if (status != NAULA_OK) {
        printf("stopped at %s\n", step->path == NULL ? "(step)" : step->path);
    } else if (step->kind == NAULA_MOVE_CHECKOUT) {
        printf("move %s -> %s\n", step->path, step->target);
    } else if (step->kind == NAULA_REMOVE_LINK) {
        printf("unlink %s\n", step->path);
    } else if (step->kind == NAULA_REMOVE_CLONE) {
        printf("remove %s\n", step->path);
    }
}

/** Claude Code's memory banks: `NAULA_MEMORY_BANKS`, else `~/.claude/projects` where it exists, else none. */
static char *memory_banks(void) {
    char *named = cli_environment("NAULA_MEMORY_BANKS");
    if (named != NULL) {
        return named;
    }
#if defined(_WIN32)
    char *home = cli_environment("USERPROFILE");
#else
    char *home = cli_environment("HOME");
#endif
    char *banks = home == NULL ? NULL : cli_join(home, ".claude/projects");
    naula_memory_release(home);
    if (banks != NULL && !cli_is_folder(banks)) {
        naula_memory_release(banks);
        return NULL;
    }
    return banks;
}

static naula_status apply(const sync_request *request, const naula_layout_plan *plan, naula_refusal *refusal) {
    char *banks = memory_banks();
    naula_apply_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.root = request->root;
    options.memory_banks = banks;
    options.report = report_applied;
    naula_status status = naula_layout_apply(plan, &options, refusal);
    naula_memory_release(banks);
    return status;
}

static naula_status resume_unfinished(const sync_request *request, naula_refusal *refusal) {
    if (!cli_is_folder(request->root)) {
        return NAULA_OK;
    }
    char *journal = NULL;
    naula_status status = naula_journal_unfinished(request->root, &journal, refusal);
    if (status == NAULA_NOT_FOUND) {
        return NAULA_OK;
    }
    if (status == NAULA_OK) {
        printf("resume %s\n", journal);
        char *banks = memory_banks();
        naula_apply_options options;
        memset(&options, 0, sizeof options);
        options.size = sizeof options;
        options.root = request->root;
        options.memory_banks = banks;
        options.report = report_applied;
        status = naula_journal_resume(journal, &options, refusal);
        naula_memory_release(banks);
    }
    naula_memory_release(journal);
    return status;
}

static naula_status make_plan(const sync_request *request, sync_model *model, naula_layout_plan *plan,
                              naula_refusal *refusal) {
    naula_layout_input input;
    memset(plan, 0, sizeof *plan);
    plan->size = sizeof *plan;
    naula_status status = sync_input(request, model, &input);
    return status == NAULA_OK ? naula_layout_plan_make(&input, plan, refusal) : status;
}

/** A repo `naula clone` names is kept in the top org unless an account the root already holds has it. */
static naula_status keep_cloned(cli_context *context, sync_request *request, sync_model *model,
                                naula_refusal *refusal) {
    naula_id id;
    memset(&id, 0, sizeof id);
    id.size = sizeof id;
    naula_status status = naula_id_parse(request->clone->id, strlen(request->clone->id), &id, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    int held = sync_model_holds(model, &id);
    naula_id_release(&id);
    if (held) {
        return NAULA_OK;
    }
    status = sync_gather_extra(context, model, request->clone, refusal);
    if (status == NAULA_OK) {
        status = sync_root_add_extra(&request->root_file, request->clone);
        request->write_root_file = request->write_root_file || status == NAULA_OK;
    }
    return status;
}

static int declares_name(const naula_org_declaration *declaration, const char *name) {
    if (declaration->name != NULL && same_name(declaration->name, name)) {
        return 1;
    }
    for (size_t index = 0; index < declaration->suborg_count; index++) {
        if (declares_name(&declaration->suborgs[index], name)) {
            return 1;
        }
    }
    return 0;
}

static int holds_org_named(const sync_model *model, const char *name) {
    for (size_t index = 0; index < model->org_count; index++) {
        if (declares_name(model->orgs[index].declaration, name)) {
            return 1;
        }
    }
    return 0;
}

static naula_status lay_out(cli_context *context, sync_request *request, sync_model *model, int *held,
                            naula_refusal *refusal) {
    naula_status status = sync_gather(context, request, model, refusal);
    if (status == NAULA_OK && request->org_filter != NULL && !holds_org_named(model, request->org_filter)) {
        naula_refuse(refusal, "org-not-held", "name an org this root holds, or hold it first",
                     "this root holds no org named %s", request->org_filter);
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK && request->clone != NULL) {
        status = keep_cloned(context, request, model, refusal);
    }
    naula_layout_plan plan;
    if (status == NAULA_OK) {
        status = make_plan(request, model, &plan, refusal);
    }
    if (status != NAULA_OK) {
        return status;
    }
    if (request->plan_only) {
        status = collect_rules_files(request, &plan);
        naula_layout_plan_release(&plan);
        return status;
    }
    if (!*held) {
        status = cli_make_folders(request->root, refusal);
        if (status == NAULA_OK) {
            status = naula_root_hold(request->root, refusal);
        }
        *held = status == NAULA_OK;
    }
    if (status == NAULA_OK && request->write_root_file) {
        status = naula_root_file_write(request->root, &request->root_file, refusal);
    }
    int cloned = 0;
    if (status == NAULA_OK) {
        status = clone_missing(request, model, &plan, &cloned, refusal);
    }
    if (status == NAULA_OK && cloned) {
        naula_layout_plan_release(&plan);
        status = make_plan(request, model, &plan, refusal);
    }
    if (status == NAULA_OK) {
        print_reports(&plan);
        status = apply(request, &plan, refusal);
    }
    if (status == NAULA_OK) {
        status = collect_rules_files(request, &plan);
    }
    naula_layout_plan_release(&plan);
    return status;
}

naula_status sync_run(cli_context *context, sync_request *request, naula_refusal *refusal) {
    int held = 0;
    naula_status status = NAULA_OK;
    if (cli_is_folder(request->root)) {
        status = naula_root_hold(request->root, refusal);
        held = status == NAULA_OK;
    }
    if (status == NAULA_OK && !request->plan_only) {
        status = resume_unfinished(request, refusal);
    }
    sync_model model;
    memset(&model, 0, sizeof model);
    if (status == NAULA_OK) {
        status = lay_out(context, request, &model, &held, refusal);
    }
    if (status != NAULA_OK) {
        cli_print_questions(context);
    }
    sync_model_release(&model);
    if (held) {
        naula_root_unhold();
    }
    return status;
}
