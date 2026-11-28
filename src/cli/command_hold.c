#include <string.h>

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
    status = release_org(&context, &request, arguments.positional[0], &refusal);
    if (status == NAULA_OK) {
        status = sync_run(&context, &request, &refusal);
    }
    sync_request_release(&request);
    cli_context_close(&context);
    return status == NAULA_OK ? CLI_DONE : cli_refused(status, &refusal);
}
