#ifndef NAULA_CLI_SYNC_INTERNAL_H
#define NAULA_CLI_SYNC_INTERNAL_H

#include <stddef.h>

#include "cli.h"
#include "naula/layout.h"
#include "naula/layout_apply.h"

/**
 * What one layout command asks of a root: the root, what `.naula.json` holds once the command is done, whether the
 * command changed that, and the org whose missing repos are cloned (NULL for all).
 */
typedef struct sync_request {
    char *root;
    naula_root_file root_file;
    int write_root_file;
    const char *org_filter;
    const naula_reference *clone;
} sync_request;

/** One repo of a held account, as its host and its checkout show it, owning every text it points at. */
typedef struct sync_repo {
    naula_layout_repo layout;
    naula_place place;
    char *clone_url;
    char *working_branch;
    naula_resolution resolution;
    int resolved;
} sync_repo;

/** A checkout found under the root, by the id its history gives. */
typedef struct sync_checkout {
    char *path;
    naula_id id;
    int clean_and_pushed;
} sync_checkout;

/**
 * An org the root holds: one it holds by reference owns its resolution, one found declared in a held account points
 * at the declaration its repo's resolution holds.
 */
typedef struct sync_org {
    naula_resolution resolution;
    int owns_resolution;
    const naula_org_declaration *declaration;
    naula_id id;
} sync_org;

typedef struct sync_model {
    sync_org *orgs;
    size_t org_count;
    sync_repo *repos;
    size_t repo_count;
    size_t repo_capacity;
    sync_checkout *checkouts;
    size_t checkout_count;
    size_t checkout_capacity;
    naula_layout_links links;
    naula_org_declaration *declarations;
    naula_id *org_ids;
    naula_layout_repo *layout_repos;
} sync_model;

/**
 * Reads everything the plan needs: the orgs the root holds, every repo of their accounts, the extra repos, the
 * checkouts and the links under the root. Any home that cannot be heard refuses the whole read, since a plan made from
 * part of a root would remove the links of what it could not see.
 */
naula_status sync_gather(cli_context *context, const sync_request *request, sync_model *model,
                         naula_refusal *refusal);

/** Adds one kept repo to the model from its reference; one its homes refuse is left out and named. */
naula_status sync_gather_extra(cli_context *context, sync_model *model, const naula_reference *reference,
                               naula_refusal *refusal);

/** Whether the model holds a repo with this id. */
int sync_model_holds(const sync_model *model, const naula_id *id);

/** Edits of what `.naula.json` will hold, each copying what it adds. */
naula_status sync_root_add_held(naula_root_file *file, const naula_reference *reference);
naula_status sync_root_add_extra(naula_root_file *file, const naula_reference *reference);
naula_status sync_root_drop(naula_root_file *file, const char *name);
void sync_root_remove_added(naula_root_file *file, size_t index);
void sync_root_undrop(naula_root_file *file, const char *name);

/** The plan's input over the model; valid while the model is. */
naula_status sync_input(const sync_request *request, sync_model *model, naula_layout_input *input);

void sync_model_release(sync_model *model);

/** Plans, clones what is missing, and applies, writing nothing when the plan is refused. */
naula_status sync_run(cli_context *context, sync_request *request, naula_refusal *refusal);

/** The root a command acts on: `--root`, or the nearest folder above the working one holding `.naula.json`. */
naula_status sync_find_root(const cli_arguments *arguments, char **root, naula_refusal *refusal);

void sync_request_release(sync_request *request);

/** Finds the root, reads its `.naula.json` and opens the context, for every command acting on a root. */
naula_status sync_open_root(const cli_arguments *arguments, cli_context *context, sync_request *request,
                            naula_refusal *refusal);

/** The name of the org the repo `reference` names declares, refused `not-an-org` for a repo that declares none. */
naula_status sync_org_name(cli_context *context, const naula_reference *reference, char **name,
                           naula_refusal *refusal);

#endif
