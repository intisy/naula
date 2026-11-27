#ifndef NAULA_LAYOUT_H
#define NAULA_LAYOUT_H

/**
 * The layout of a root, derived and never written: which folder every org and every repo becomes, as a plan of steps
 * that nothing here carries out. Every struct is read only up to its `size`; the caller sets it. An array of structs
 * is read at the stride of this header's `sizeof`, whatever `size` its elements carry.
 *
 * Every path is absolute (`/`, or a drive letter and a separator) with no empty, `.` or `..` segment and no `:`
 * past the drive, or the input is refused as `layout-input-invalid`. A checkout's `current_path` must name the real
 * folder, never a link to it: the plan cannot see the disk, so the applier is what refuses a link.
 */

#include <stddef.h>
#include <stdint.h>

#include "naula/home.h"
#include "naula/id.h"
#include "naula/repo_file.h"
#include "naula/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One repo the root may hold, as its host and its checkout show it. `primary` with `kind` 0, or a `local:` id, is a
 * repo with no home. `current_path` is where its checkout is now, NULL when it is not cloned; a held repo whose
 * checkout is outside the root is left alone and reported `path-outside-root`. `clean_and_pushed` says
 * every commit of the checkout is on a home and nothing is uncommitted; only then may an archived clone be removed.
 * `member_of` is the `org.memberOf` of its repo.json, NULL when it states none: it confirms the account of `primary`,
 * and no other, for the org the referenced repo declares. First shape ends at `current_path`; a struct without
 * `adopted` is read as adopted, and one without `member_of` confirms nothing.
 */
typedef struct naula_layout_repo {
    uint32_t size;
    naula_id id;
    char *name;
    char **topics;
    size_t topic_count;
    naula_home primary;
    int archived;
    int clean_and_pushed;
    char *current_path;
    int adopted;
    const naula_reference *member_of;
} naula_layout_repo;

/**
 * `top` is the root's top org and `orgs` the other orgs it holds; `dropped` names orgs, a sub-org included, that the
 * root leaves out (the top org is never dropped). `top_repository` and `org_repositories[i]` are the ids of the repos
 * declaring `top` and `orgs[i]`, which is how an org's `parent` and its rules file are found. `extra` are single repos
 * kept from accounts no held org owns, `extra_orgs[i]` naming the org `extra[i]` is kept in, NULL meaning the top
 * org. `existing_links` are the links found under the root, each `path` pointing at `target`; one whose target sits
 * in a `repos` folder and that the plan does not make again is removed. First shape ends at `existing_link_count`.
 *
 * An org holds an account it names when its declaring repo's primary home sits under it (for a sub-org entry, the repo
 * that lists it), or when a repo whose primary home sits under it names that declaring repo in `member_of`; any other
 * account it names is reported `account-not-owned` and ignored. A `member_of` naming no repo that declares a held org
 * is reported `member-of-unknown`, and one whose org does not name its account `member-of-unlisted`; neither confirms
 * anything. Two orgs holding one account refuse the plan. An org nests under its `parent` only when the parent lists it
 * in `suborgs` or its declaring repo sits under an account the parent holds; otherwise it is laid out under the root
 * and reported `parent-not-listing`. A listed sub-org that also declares itself is nested by the entry and takes its
 * members from its own declaration, reported `suborg-declares-itself`.
 */
/** First shape ends at `target`. */
typedef struct naula_layout_link {
    uint32_t size;
    char *path;
    char *target;
} naula_layout_link;

typedef struct naula_layout_input {
    uint32_t size;
    const char *root;
    const naula_org_declaration *top;
    const naula_org_declaration *orgs;
    size_t org_count;
    const char *const *dropped;
    size_t dropped_count;
    const naula_layout_repo *repos;
    size_t repo_count;
    const naula_reference *extra;
    size_t extra_count;
    const naula_id *top_repository;
    const naula_id *org_repositories;
    const char *const *extra_orgs;
    const naula_layout_link *existing_links;
    size_t existing_link_count;
} naula_layout_input;

/** Enums append only. */
typedef enum naula_layout_step_kind {
    NAULA_MAKE_FOLDER = 1,
    NAULA_WRITE_FILE = 2,
    NAULA_MAKE_LINK = 3,
    NAULA_MOVE_CHECKOUT = 4,
    NAULA_REMOVE_LINK = 5,
    NAULA_REMOVE_CLONE = 6,
    NAULA_REPORT = 7
} naula_layout_step_kind;

/**
 * One step, each meant to be compared with the disk before it changes anything, so a plan from a laid-out root
 * changes nothing:
 * - MAKE_FOLDER `path` makes the folder if it is missing.
 * - MOVE_CHECKOUT moves the checkout at `path` to `target`; the applier refuses when `path` is a link.
 * - REMOVE_LINK removes the link at `path` only while it is a link to `target`, never what it points at.
 * - REMOVE_CLONE removes the archived checkout at `path`, which is a real folder and never a link: the applier refuses
 *   a link at `path` and never follows a link or other reparse point inside it, removing the link itself instead.
 * - WRITE_FILE gives the file at `path` exactly `content`.
 * - MAKE_LINK makes `path` a link to `target`, replacing only a link.
 * - REPORT names what the layout left alone: `code`, the repo or org at `path`, another one involved at `target` or
 *   NULL, and `content` saying why. `unadopted`, `not-held`, `path-outside-root` and `suborg-declares-itself` are
 *   notes; every other code is a refusal.
 * First shape ends at `code`.
 */
typedef struct naula_layout_step {
    uint32_t size;
    uint32_t kind;
    char *path;
    char *target;
    char *content;
    char code[64];
} naula_layout_step;

/** First shape ends at `step_count`. */
typedef struct naula_layout_plan {
    uint32_t size;
    naula_layout_step *steps;
    size_t step_count;
} naula_layout_plan;

/**
 * Derives the plan for `input`, its steps in the order they are to run: org and `repos` folders, moves, link and
 * clone removals, project and archive folders, the org `CLAUDE.md` files, links, then reports. A repo it cannot place
 * safely is reported and left where it is; the whole plan is refused only when the org tree itself is undefined:
 * `account-held-twice`, `parent-cycle`, `name-collides` or `name-invalid` for an org or project folder, and
 * `path-occupied` for a checkout holding an org or `repos` folder. A struct smaller than its first shape is refused
 * as `layout-input-invalid`. On anything but NAULA_OK the plan is left empty.
 */
naula_status naula_layout_plan_make(const naula_layout_input *input, naula_layout_plan *plan, naula_refusal *refusal);

void naula_layout_plan_release(naula_layout_plan *plan);

#ifdef __cplusplus
}
#endif

#endif
