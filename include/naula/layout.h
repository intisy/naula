#ifndef NAULA_LAYOUT_H
#define NAULA_LAYOUT_H

/**
 * The layout of a root, derived and never written: which folder every org and every repo becomes, as a plan of steps
 * that nothing here carries out. Every struct is read only up to its `size`; the caller sets it. An array of structs
 * is read at the stride of this header's `sizeof`, whatever `size` its elements carry.
 *
 * Every path is absolute (`/`, or a drive letter and a separator) with no empty, `.` or `..` segment and no `:`
 * past the drive, or the input is refused as `layout-input-invalid`; the root is a folder, never `/` or a whole drive.
 * A checkout's `current_path` must name the real folder, never a link to it or a path through one: the plan cannot
 * see the disk, so it leaves alone every checkout at or below an `existing_links` path. A link without
 * `made_by_naula` at or above any path the plan would write, make, move to, remove or import through makes that step
 * `path-occupied`: it is skipped with everything below it, and the plan is refused when the link holds an org or
 * `repos` folder. The applier checks every component of both a step's `path` and its `target` below the root and
 * refuses a step that reaches through a link, except the link a MAKE_LINK replaces or a REMOVE_LINK removes.
 *
 * The trust model rests on `naula_layout_repo.primary`: an org holds an account because a repo is homed there, so
 * `primary` must be the home the provider verified, the account the host answered for that very repository, never
 * the `homes` text of its repo.json, which anyone can write. Task 15's reader fills it so.
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
 * One repo the root may hold, as its host and its checkout show it. `primary` is the provider-verified home (see the
 * top of this header); with `kind` 0, or a `local:` id, it is a repo with no home. `current_path` is where its checkout is now, NULL when it is not cloned; a held repo whose
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
 * org. `existing_links` are the links found under the root, each `path` pointing at `target`. Only one marked
 * `made_by_naula` is ever removed, and only when the plan does not make it again; one that is not marked is never
 * written through, as the top of this header says. Links are removed before any folder is made, so one naula made
 * where an org, checkout or project folder now goes is gone first; a marked one the plan cannot remove counts as
 * unmarked for every folder the plan makes at or below it. Task 14 must record every link it makes, in its journal or
 * root file, and pass that record back as the flag. First shape ends at `existing_link_count`.
 *
 * An org holds an account it names when any repo with its declaring repo's id is homed there (for a sub-org entry, the
 * repo that lists it), or when a repo homed there names that declaring repo in `member_of`; any other account it names
 * is reported `account-not-owned` and ignored. A `member_of` naming no repo that declares a held org is reported
 * `member-of-unknown`, one whose org does not name its account `member-of-unlisted`, and one naming an org left out
 * `org-name-taken`; none confirms anything.
 * When two orgs claim one account, a declaring repo homed there beats a confirmation, which is reported
 * `member-of-contested`; two confirmations hold neither, both reported; a sub-org entry naming the account of the repo
 * that declares it gives way, reported `suborg-lists-parent-account`; two declaring repos refuse the plan. An org nests
 * under its `parent` only when its declaring repo is homed in an account the parent's own declaring repo is homed in
 * (a confirmation never counts here), or the parent lists its name and that repo is homed in an account the entry
 * names; otherwise it is laid out under the root and reported `parent-not-listing`, and the parent's entry stays. An
 * org at the root by default or refused by its parent never takes the folder of an entry or an accepted org, a project
 * of its parent or a folder naula keeps: it is left out and reported `org-name-taken`; two orgs of equal standing
 * refuse the plan as `name-collides`. An org folder always takes a repo's link path, whatever the org's standing, since
 * a repo's name never decides the tree: the repo gets no link, reported `name-collides`. Two `orgs` from one declaring repo with
 * different declarations refuse the input as `layout-input-invalid`; an identical one is kept once. A listed sub-org
 * whose declaration the entry accepts is nested by the entry and takes its members from its own declaration, reported
 * `suborg-declares-itself`.
 */
/**
 * A link found under the root. `made_by_naula` is set only from naula's own record of the links it made (task 14's
 * journal or root file), never inferred from where the link sits or what it points at: a link without it is never
 * removed, so a first sync with no record removes nothing. First shape ends at `made_by_naula`.
 */
typedef struct naula_layout_link {
    uint32_t size;
    char *path;
    char *target;
    int made_by_naula;
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
 * - REMOVE_LINK removes the link at `path` only while it is a link to `target`, never what it points at. Only a link
 *   naula's record says it made is removed, and of those only one at an org folder's top or one folder below it
 *   (`archive`, or any folder that is neither reserved nor a laid-out org, since a renamed project or a released
 *   org's projects are named nowhere),
 *   pointing at a checkout folder (twelve lowercase hex, or `local-...`) directly in that org folder's `repos`, and
 *   outside every checkout, never at or above a `current_path`. Task 14 removes it only after verifying that `path`
 *   is a reparse point or symlink whose target is exactly the recorded one, and never falls back to removing a folder.
 * - REMOVE_CLONE removes the archived checkout at `path`, which is a real folder and never a link: the applier refuses
 *   a link at `path` and never follows a link or other reparse point inside it, removing the link itself instead.
 * - WRITE_FILE gives the file at `path` exactly `content`.
 * - MAKE_LINK makes `path` a link to `target`, replacing only a link.
 * - REPORT names what the layout left alone: `code`, the repo or org at `path`, another one involved at `target` or
 *   NULL, and `content` saying why. `unadopted`, `not-held`, `path-outside-root` and `suborg-declares-itself` are
 *   notes; every other code is a refusal, `under-existing-link` among them.
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
 * Derives the plan for `input`, its steps in the order they are to run: link removals, org and `repos` folders, moves,
 * clone removals, project and archive folders, the org `CLAUDE.md` files, links, then reports. A repo it cannot place
 * safely is reported and left where it is; the whole plan is refused only when the org tree itself is undefined:
 * `account-held-twice`, `parent-cycle`, `name-collides` or `name-invalid` for an org or project folder, and
 * `path-occupied` for a checkout, or a link the plan does not remove first, at or above an org or `repos` folder. A
 * struct smaller than its first shape is refused as `layout-input-invalid`. On anything but NAULA_OK the plan is left
 * empty.
 */
naula_status naula_layout_plan_make(const naula_layout_input *input, naula_layout_plan *plan, naula_refusal *refusal);

void naula_layout_plan_release(naula_layout_plan *plan);

#ifdef __cplusplus
}
#endif

#endif
