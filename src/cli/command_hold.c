#include <stdio.h>
#include <string.h>

#include "disk.h"
#include "naula/memory.h"
#include "sync_internal.h"

naula_status sync_org_name(cli_context *context, const naula_reference *reference, char **name,
                           naula_refusal *refusal) {
    naula_resolution resolution;
    *name = NULL;
    naula_status status = cli_resolve(context, reference, &resolution, refusal);
    if (status == NAULA_OK && (resolution.file.org == NULL || resolution.file.org->name == NULL)) {
        naula_refuse(refusal, "not-an-org", "hold the repo that declares the org in its repo.json",
                     "%s declares no org", reference->id == NULL ? reference->url : reference->id);
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK) {
        *name = naula_memory_duplicate_text(resolution.file.org->name);
        status = *name == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    } else {
        cli_print_verdicts(&resolution);
    }
    naula_resolution_release(&resolution);
    return status;
}

static int same_id(const naula_reference *left, const naula_reference *right) {
    return left->id != NULL && right->id != NULL && strcmp(left->id, right->id) == 0;
}

static int is_held(const naula_root_file *file, const naula_reference *reference) {
    if (same_id(&file->top, reference)) {
        return 1;
    }
    for (size_t index = 0; index < file->added_count; index++) {
        if (same_id(&file->added[index], reference)) {
            return 1;
        }
    }
    return 0;
}

/** Holds the org, taking it back from `dropped` when an earlier release left it there. */
static naula_status add_org(cli_context *context, sync_request *request, const naula_reference *reference,
                            naula_refusal *refusal) {
    char *name = NULL;
    naula_status status = sync_org_name(context, reference, &name, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    sync_root_undrop(&request->root_file, name);
    naula_memory_release(name);
    if (!is_held(&request->root_file, reference)) {
        status = sync_root_add_held(&request->root_file, reference);
    }
    request->write_root_file = 1;
    return status;
}

int cli_command_hold(int count, char **values) {
    cli_arguments arguments;
    if (!cli_parse_arguments("hold", count, values, CLI_ALLOW_AT | CLI_ALLOW_ROOT | CLI_ALLOW_ACCEPT, 1, 1,
                             &arguments)) {
        return CLI_USAGE;
    }
    cli_context context;
    sync_request request;
    naula_refusal refusal = cli_fresh_refusal();
    naula_status status = sync_open_root(&arguments, &context, &request, &refusal);
    if (status != NAULA_OK) {
        return cli_refused(status, &refusal);
    }
    naula_reference reference;
    status = cli_reference_from_arguments(&context, arguments.positional[0], arguments.at, arguments.at_count,
                                          &reference, &refusal);
    if (status == NAULA_OK) {
        status = add_org(&context, &request, &reference, &refusal);
        cli_reference_release(&reference);
    }
    if (status == NAULA_OK) {
        status = sync_run(&context, &request, &refusal);
    }
    sync_request_release(&request);
    cli_context_close(&context);
    return status == NAULA_OK ? CLI_DONE : cli_refused(status, &refusal);
}

static int declares(const naula_org_declaration *declaration, const char *name) {
    for (size_t index = 0; index < declaration->suborg_count; index++) {
        const naula_org_declaration *suborg = &declaration->suborgs[index];
        if ((suborg->name != NULL && strcmp(suborg->name, name) == 0) || declares(suborg, name)) {
            return 1;
        }
    }
    return 0;
}

/** Whether the org named `name` is the top, a sub-org the top declares, or one of the top's own `hold`. */
static naula_status held_by_top(cli_context *context, const naula_reference *top, const char *name, int *is_top,
                                int *held, naula_refusal *refusal) {
    naula_resolution resolution;
    *is_top = 0;
    *held = 0;
    naula_status status = cli_resolve(context, top, &resolution, refusal);
    const naula_org_declaration *declaration = status == NAULA_OK ? resolution.file.org : NULL;
    if (declaration != NULL) {
        *is_top = declaration->name != NULL && strcmp(declaration->name, name) == 0;
        *held = declares(declaration, name);
    }
    for (size_t index = 0; status == NAULA_OK && declaration != NULL && !*held && index < declaration->hold_count;
         index++) {
        char *held_name = NULL;
        status = sync_org_name(context, &declaration->hold[index], &held_name, refusal);
        *held = held_name != NULL && strcmp(held_name, name) == 0;
        naula_memory_release(held_name);
    }
    naula_resolution_release(&resolution);
    return status;
}

/** Leaves the org out of the root: an added org leaves `added`, any other the root holds goes into `dropped`. */
static naula_status release_org(cli_context *context, sync_request *request, const char *name,
                                naula_refusal *refusal) {
    int is_top = 0;
    int held = 0;
    naula_status status = held_by_top(context, &request->root_file.top, name, &is_top, &held, refusal);
    if (status == NAULA_OK && is_top) {
        naula_refuse(refusal, "top-org", "make another root for another top org", "%s is this root's top org", name);
        return NAULA_REFUSED;
    }
    for (size_t index = 0; status == NAULA_OK && index < request->root_file.added_count;) {
        char *added_name = NULL;
        status = sync_org_name(context, &request->root_file.added[index], &added_name, refusal);
        int matches = added_name != NULL && strcmp(added_name, name) == 0;
        naula_memory_release(added_name);
        if (status == NAULA_OK && matches) {
            sync_root_remove_added(&request->root_file, index);
            request->write_root_file = 1;
        } else {
            index++;
        }
    }
    if (status == NAULA_OK && held) {
        status = sync_root_drop(&request->root_file, name);
        request->write_root_file = 1;
    }
    if (status == NAULA_OK && !request->write_root_file) {
        naula_refuse(refusal, "org-not-held", "name an org this root holds", "this root holds no org named %s", name);
        status = NAULA_REFUSED;
    }
    return status;
}

static int is_listed(char *const *paths, size_t count, const char *path) {
    for (size_t index = 0; index < count; index++) {
        if (strcmp(paths[index], path) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * Removes a CLAUDE.md naula wrote for an org this root no longer lays out, only while it holds exactly `content`, what
 * the plan made before the release writes there; one someone edited is left and named.
 */
static naula_status remove_rules_file(const char *root, const char *path, const char *content, naula_refusal *refusal) {
    size_t root_length = strlen(root);
    if (strncmp(path, root, root_length) != 0 || path[root_length] != '/') {
        return NAULA_OK;
    }
    int holding = 0;
    unsigned long error = 0;
    naula_status status = disk_file_remove_if_holding(root, path + root_length + 1, (const uint8_t *)content,
                                                      strlen(content), DISK_FOR_PLAN_STEP, &holding, &error);
    if (status == NAULA_OK && holding) {
        printf("remove %s\n", path);
    } else if (status == NAULA_OK) {
        printf("left-alone rules-file-edited: %s: it is not the import naula wrote, so it stays\n", path);
    } else if (status == NAULA_REFUSED) {
        char text[256];
        disk_error_text(error, text, sizeof text);
        naula_refuse(refusal, "file-not-removed", "remove it by hand", "cannot remove %s: %s", path, text);
    }
    return status == NAULA_NOT_FOUND ? NAULA_OK : status;
}

/** The rules files of the plan made before the release, kept while the release is planned and applied. */
typedef struct released_rules {
    char **paths;
    char **contents;
    size_t count;
} released_rules;

static void released_rules_release(released_rules *rules) {
    for (size_t index = 0; index < rules->count; index++) {
        naula_memory_release(rules->paths[index]);
        naula_memory_release(rules->contents[index]);
    }
    naula_memory_release(rules->paths);
    naula_memory_release(rules->contents);
}

static naula_status remove_released_rules(const sync_request *request, const released_rules *before,
                                          naula_refusal *refusal) {
    naula_status status = NAULA_OK;
    for (size_t index = 0; status == NAULA_OK && index < before->count; index++) {
        if (!is_listed(request->rules_files, request->rules_file_count, before->paths[index])) {
            status = remove_rules_file(request->root, before->paths[index], before->contents[index], refusal);
        }
    }
    return status;
}

#if defined(NAULA_TEST_HOOKS)
/** Names whether this process holds the root's lock as the rules are removed, which a test reads. */
static void report_lock_for_test(const char *root) {
    char *lock = cli_join(root, ".naula-journal/lock");
    uint8_t *bytes = NULL;
    size_t length = 0;
    unsigned long error = 0;
    char expected[32];
    snprintf(expected, sizeof expected, "%lu\n", disk_process_id());
    int held = lock != NULL && disk_file_read(lock, &bytes, &length, &error) == NAULA_OK && length == strlen(expected) &&
               memcmp(bytes, expected, length) == 0;
    fprintf(stderr, "naula-test: root lock %s as the rules are removed\n", held ? "held" : "free");
    naula_memory_release(bytes);
    naula_memory_release(lock);
}
#else
static void report_lock_for_test(const char *root) {
    (void)root;
}
#endif

/**
 * The org's links go with the plan made without it; its CLAUDE.md, which no plan writes once the org is left out, is
 * found by planning the root as it stood first and removed once the release is applied. The root's lock is held for
 * the whole command, so no other naula changes the root between the plan and the removal.
 */
static naula_status release_and_sync(cli_context *context, sync_request *request, const char *name,
                                     naula_refusal *refusal) {
    naula_status status = naula_root_hold(request->root, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    request->root_held = 1;
    request->plan_only = 1;
    status = sync_run(context, request, refusal);
    request->plan_only = 0;
    released_rules before = {request->rules_files, request->rules_contents, request->rules_file_count};
    request->rules_files = NULL;
    request->rules_contents = NULL;
    request->rules_file_count = 0;
    if (status == NAULA_OK) {
        status = release_org(context, request, name, refusal);
    }
    if (status == NAULA_OK) {
        status = sync_run(context, request, refusal);
    }
    if (status == NAULA_OK) {
        report_lock_for_test(request->root);
        status = remove_released_rules(request, &before, refusal);
    }
    released_rules_release(&before);
    request->root_held = 0;
    naula_root_unhold();
    return status;
}

int cli_command_release(int count, char **values) {
    cli_arguments arguments;
    if (!cli_parse_arguments("release", count, values, CLI_ALLOW_ROOT | CLI_ALLOW_ACCEPT, 1, 1, &arguments)) {
        return CLI_USAGE;
    }
    cli_context context;
    sync_request request;
    naula_refusal refusal = cli_fresh_refusal();
    naula_status status = sync_open_root(&arguments, &context, &request, &refusal);
    if (status != NAULA_OK) {
        return cli_refused(status, &refusal);
    }
    status = release_and_sync(&context, &request, arguments.positional[0], &refusal);
    sync_request_release(&request);
    cli_context_close(&context);
    return status == NAULA_OK ? CLI_DONE : cli_refused(status, &refusal);
}
