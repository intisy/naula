#ifndef NAULA_LAYOUT_INTERNAL_H
#define NAULA_LAYOUT_INTERNAL_H

#include <stddef.h>

#include "naula/layout.h"

#define LAYOUT_NONE ((size_t)-1)

typedef enum layout_phase {
    LAYOUT_PHASE_TREE_FOLDERS = 1,
    LAYOUT_PHASE_MOVES = 2,
    LAYOUT_PHASE_LINK_REMOVALS = 3,
    LAYOUT_PHASE_CLONE_REMOVALS = 4,
    LAYOUT_PHASE_LINK_FOLDERS = 5,
    LAYOUT_PHASE_FILES = 6,
    LAYOUT_PHASE_LINKS = 7,
    LAYOUT_PHASE_REPORTS = 8
} layout_phase;

typedef struct layout_entry {
    int phase;
    size_t order;
    naula_layout_step step;
} layout_entry;

typedef struct layout_builder {
    layout_entry *entries;
    size_t count;
    size_t capacity;
} layout_builder;

naula_status layout_add(layout_builder *builder, int phase, size_t order, naula_layout_step_kind kind,
                        const char *path, const char *target, const char *content);
naula_status layout_report(layout_builder *builder, const char *code, const char *path, const char *target,
                           const char *format, ...);
void layout_builder_release(layout_builder *builder);

/** ASCII case folded, since the folder names a layout accepts are ASCII and Windows and macOS fold their case. */
int layout_same_name(const char *left, const char *right);
/** Names an org folder keeps for naula and Claude Code, so no repo, project or org may take them. */
int layout_is_reserved(const char *name);
char *layout_join(const char *parent, const char *name);
char *layout_concatenate(const char *first, const char *second);
/** Whether `inner` is `outer` or below it, case folded and either separator, so a doubt counts as a clash. */
int layout_path_encloses(const char *outer, const char *inner);
int layout_path_strictly_encloses(const char *outer, const char *inner);
int layout_path_equal(const char *left, const char *right);
/** 1 for `/`, 3 for a drive letter, a colon and a separator, 0 for a path that is not absolute. */
size_t layout_filesystem_root_length(const char *path);
int layout_is_clean_absolute(const char *path);
const char *layout_basename(const char *path);
char *layout_relative(const char *from, const char *to);
const char *layout_below_root(const char *root, const char *path);
int layout_same_repository(const naula_id *left, const naula_id *right);
int layout_reference_names(const naula_reference *reference, const naula_id *id, const naula_home *primary);

/**
 * `rules_repository` is the repo that declares the org: an account any input repo with that id is homed in is held
 * without confirmation, and a `memberOf` names that repo to confirm another account. `not_listing` marks an org laid
 * out under the root because its parent does not accept it; `accepted` one that is the top org, a `suborgs` entry or
 * nested by its parent's agreement, which outranks an org at the root by default; `replaces_entry` one that takes the
 * place of its parent's
 * `suborgs` entry of the same name. `set_aside[i]`, once the tree is built, is why the claim on the declaration's
 * account `i` gave way to another org's, 0 while the claim stands.
 */
typedef struct layout_org {
    const naula_org_declaration *declaration;
    const naula_id *repository;
    const naula_id *rules_repository;
    size_t parent;
    int is_top;
    int not_listing;
    int replaces_entry;
    int accepted;
    unsigned char *set_aside;
    char *folder;
    char *repos_folder;
} layout_org;

/** A repo's `memberOf`, `declaring` the held repo it names, NULL when it names none that declares a held org. */
typedef struct layout_confirmation {
    const naula_layout_repo *repo;
    const naula_id *declaring;
} layout_confirmation;

/** An org left out because an accepted org took its folder, reported against the folder of `winner`. */
typedef struct layout_left_out {
    const naula_org_declaration *declaration;
    const naula_org_declaration *winner;
} layout_left_out;

typedef struct layout_tree {
    const naula_layout_repo *repos;
    size_t repo_count;
    char *root;
    layout_org *orgs;
    size_t count;
    size_t capacity;
    layout_confirmation *confirmations;
    size_t confirmation_count;
    layout_left_out *left_out;
    size_t left_out_count;
} layout_tree;

naula_status layout_tree_build(const naula_layout_input *input, layout_tree *tree, layout_builder *builder,
                               naula_refusal *refusal);
void layout_tree_release(layout_tree *tree);
size_t layout_tree_account_owner(const layout_tree *tree, const naula_home *home);
/** The one laid-out org called `name`; LAYOUT_NONE for none, and for two, which `ambiguous` tells apart. */
size_t layout_tree_named(const layout_tree *tree, const char *name, int *ambiguous);
/** The deepest org whose folder holds `path`, the top org when none does. */
size_t layout_tree_enclosing(const layout_tree *tree, const char *path);
int layout_tree_is_folder(const layout_tree *tree, const char *path);
/** `member-of-unknown` or `member-of-unlisted` when `repo`'s `memberOf` confirms nothing, NULL otherwise. */
const char *layout_tree_confirmation_problem(const layout_tree *tree, const naula_layout_repo *repo);
int layout_org_has_project(const layout_org *org, const char *name);

typedef enum layout_note {
    LAYOUT_NOTE_NAME_INVALID = 1u << 0,
    LAYOUT_NOTE_TWO_PROJECT_TOPICS = 1u << 1,
    LAYOUT_NOTE_OUTSIDE_ROOT = 1u << 2,
    LAYOUT_NOTE_ORG_UNKNOWN = 1u << 3
} layout_note;

/**
 * Where one repo goes. `stays` means it is left exactly where it is: not moved, linked or removed. `stale_links` are
 * links it may have had before it was archived, removed only while they still point at it.
 */
typedef struct layout_placement {
    const naula_layout_repo *repo;
    size_t org;
    int homeless;
    int stays;
    int removes;
    unsigned notes;
    char *checkout;
    char *link;
    char *stale_links[2];
    size_t order;
} layout_placement;

typedef struct layout_placements {
    layout_placement *items;
    size_t count;
} layout_placements;

int layout_repo_is_adopted(const naula_layout_repo *repo);
const naula_reference *layout_repo_member_of(const naula_layout_repo *repo);
int layout_placement_moves(const layout_placement *placement);
naula_status layout_place(const naula_layout_input *input, const layout_tree *tree, layout_placements *placements,
                          naula_refusal *refusal);
naula_status layout_refuse_collisions(layout_placements *placements, layout_builder *builder);
/** Leaves alone a checkout sitting where naula puts its own links and folders, since that path may be a link. */
naula_status layout_guard_naula_paths(const naula_layout_input *input, const layout_tree *tree,
                                      layout_placements *placements, layout_builder *builder);
naula_status layout_order_moves(layout_placements *placements, layout_builder *builder);
const char *layout_placement_name(const layout_placement *placement);
/** Where a report names a repo: where it will be once the plan has run. */
const char *layout_placement_subject(const layout_placement *placement);
/** layout_placement_subject, or the repo's name, or its id, so a report never names nothing. */
const char *layout_placement_reported_at(const layout_placement *placement);
void layout_placements_release(layout_placements *placements);

#endif
