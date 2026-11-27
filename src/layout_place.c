#include <stddef.h>
#include <string.h>

#include "layout_internal.h"
#include "naula/memory.h"
#include "repo_file_internal.h"

static const char LOCAL_PREFIX[] = "local-";
static const char REPO_FIX[] = "set each repo's size to its sizeof and its id from naula_id_parse";

int layout_repo_is_adopted(const naula_layout_repo *repo) {
    return repo->size >= offsetof(naula_layout_repo, adopted) + sizeof repo->adopted ? repo->adopted : 1;
}

int layout_placement_moves(const layout_placement *placement) {
    const char *current = placement->repo->current_path;
    return !placement->stays && !placement->removes && placement->checkout != NULL && current != NULL &&
           !layout_path_equal(current, placement->checkout);
}

static int is_stationary(const layout_placement *placement) {
    return placement->repo->current_path != NULL && !layout_placement_moves(placement);
}

static int is_homeless(const naula_layout_repo *repo) {
    return repo->primary.kind == 0 || repo->id.kind == NAULA_ID_LOCAL;
}

const char *layout_placement_subject(const layout_placement *placement) {
    const char *current = placement->repo->current_path;
    if (placement->checkout == NULL || ((placement->stays || placement->removes) && current != NULL)) {
        return current;
    }
    return placement->checkout;
}

const char *layout_placement_name(const layout_placement *placement) {
    return placement->repo->name == NULL ? "(unnamed)" : placement->repo->name;
}

static size_t extra_org(const naula_layout_input *input, const layout_tree *tree, const naula_layout_repo *repo) {
    for (size_t index = 0; index < input->extra_count; index++) {
        if (!layout_reference_names(&input->extra[index], &repo->id, &repo->primary)) {
            continue;
        }
        const char *name = input->extra_orgs == NULL ? NULL : input->extra_orgs[index];
        int ambiguous = 0;
        return name == NULL ? 0 : layout_tree_named(tree, name, &ambiguous);
    }
    return LAYOUT_NONE;
}

static size_t org_of(const naula_layout_input *input, const layout_tree *tree, const naula_layout_repo *repo) {
    if (is_homeless(repo)) {
        int is_in_root = repo->current_path != NULL && layout_path_encloses(tree->root, repo->current_path);
        return is_in_root ? layout_tree_enclosing(tree, repo->current_path) : LAYOUT_NONE;
    }
    size_t owner = layout_tree_account_owner(tree, &repo->primary);
    return owner != LAYOUT_NONE ? owner : extra_org(input, tree, repo);
}

static const char *project_of(const layout_org *org, const naula_layout_repo *repo, int *matches) {
    const char *project = NULL;
    *matches = 0;
    for (size_t index = 0; index < org->declaration->project_count; index++) {
        const char *candidate = org->declaration->project_names[index];
        for (size_t topic = 0; topic < repo->topic_count; topic++) {
            if (repo->topics[topic] != NULL && layout_same_name(repo->topics[topic], candidate)) {
                project = candidate;
                (*matches)++;
                break;
            }
        }
    }
    return project;
}

static int has_valid_name(const naula_layout_repo *repo) {
    return repo->name != NULL && repo_file_is_repo_name(repo->name);
}

static naula_status regular_link(const layout_org *org, const naula_layout_repo *repo, char **link, unsigned *notes) {
    int matches = 0;
    const char *project = project_of(org, repo, &matches);
    *link = NULL;
    *notes |= has_valid_name(repo) ? 0u : (unsigned)LAYOUT_NOTE_NAME_INVALID;
    *notes |= matches > 1 ? (unsigned)LAYOUT_NOTE_TWO_PROJECT_TOPICS : 0u;
    if (*notes != 0) {
        return NAULA_OK;
    }
    char *parent = matches == 1 ? layout_join(org->folder, project) : naula_memory_duplicate_text(org->folder);
    *link = parent == NULL ? NULL : layout_join(parent, repo->name);
    naula_memory_release(parent);
    return *link == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}

static naula_status place_homeless(const layout_org *org, layout_placement *placement) {
    const char *current = placement->repo->current_path;
    const char *folder = layout_basename(current);
    char *parent_of_current = naula_memory_duplicate_text(current);
    if (parent_of_current == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    parent_of_current[folder - current] = '\0';
    int placed = strncmp(folder, LOCAL_PREFIX, sizeof LOCAL_PREFIX - 1) == 0 &&
                 layout_path_equal(parent_of_current, org->repos_folder);
    naula_memory_release(parent_of_current);
    if (placed) {
        placement->checkout = naula_memory_duplicate_text(current);
        return placement->checkout == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    }
    if (!repo_file_is_folder_name(folder)) {
        placement->stays = 1;
        placement->notes |= LAYOUT_NOTE_NAME_INVALID;
        return NAULA_OK;
    }
    char *local_name = layout_concatenate(LOCAL_PREFIX, folder);
    if (local_name == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    placement->checkout = layout_join(org->repos_folder, local_name);
    naula_memory_release(local_name);
    return placement->checkout == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}

static naula_status place_checkout(const layout_org *org, layout_placement *placement) {
    char folder[13];
    naula_id_folder(&placement->repo->id, folder);
    placement->checkout = layout_join(org->repos_folder, folder);
    return placement->checkout == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}

static naula_status place_archived(const layout_org *org, layout_placement *placement) {
    const naula_layout_repo *repo = placement->repo;
    unsigned ignored = 0;
    naula_status status = regular_link(org, repo, &placement->stale_links[0], &ignored);
    placement->notes |= has_valid_name(repo) ? 0u : (unsigned)LAYOUT_NOTE_NAME_INVALID;
    if (status != NAULA_OK || !has_valid_name(repo)) {
        return status;
    }
    char *archive = layout_join(org->folder, "archive");
    char *archive_link = archive == NULL ? NULL : layout_join(archive, repo->name);
    naula_memory_release(archive);
    if (archive_link == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    if (placement->removes) {
        placement->stale_links[1] = archive_link;
    } else {
        placement->link = archive_link;
    }
    return NAULA_OK;
}

static naula_status place_held(const layout_org *org, layout_placement *placement) {
    const naula_layout_repo *repo = placement->repo;
    if (placement->homeless) {
        return place_homeless(org, placement);
    }
    if (repo->archived) {
        placement->removes = repo->clean_and_pushed;
        naula_status status = placement->removes ? NAULA_OK : place_checkout(org, placement);
        return status == NAULA_OK ? place_archived(org, placement) : status;
    }
    naula_status status = place_checkout(org, placement);
    return status == NAULA_OK ? regular_link(org, repo, &placement->link, &placement->notes) : status;
}

static int repo_holds_first_shape(const naula_layout_repo *repo) {
    return repo->size >= offsetof(naula_layout_repo, current_path) + sizeof repo->current_path &&
           repo->id.size >= offsetof(naula_id, segment_count) + sizeof repo->id.segment_count &&
           repo->primary.size >= offsetof(naula_home, url) + sizeof repo->primary.url;
}

/** A checkout folder is named from the id's hex, so anything but a whole git commit could name a path. */
static int holds_an_id(const naula_layout_repo *repo) {
    return (repo->id.kind == NAULA_ID_GIT && repo_file_is_commit(repo->id.commit)) || repo->id.kind == NAULA_ID_LOCAL;
}

static naula_status place_one(const naula_layout_input *input, const layout_tree *tree, layout_placement *placement,
                              naula_refusal *refusal) {
    const naula_layout_repo *repo = placement->repo;
    if (!repo_holds_first_shape(repo) || !holds_an_id(repo)) {
        naula_refuse(refusal, "layout-input-invalid", REPO_FIX,
                     "repos[%zu] is smaller than its first shape or holds no id", (size_t)(repo - input->repos));
        return NAULA_REFUSED;
    }
    placement->homeless = is_homeless(repo);
    placement->org = org_of(input, tree, repo);
    if (placement->org == LAYOUT_NONE || (repo->archived && repo->current_path == NULL) ||
        (placement->homeless && repo->current_path == NULL)) {
        placement->stays = 1;
        return NAULA_OK;
    }
    return place_held(&tree->orgs[placement->org], placement);
}

naula_status layout_place(const naula_layout_input *input, const layout_tree *tree, layout_placements *placements,
                          naula_refusal *refusal) {
    placements->count = 0;
    if (input->repo_count == 0) {
        placements->items = NULL;
        return NAULA_OK;
    }
    size_t bytes = input->repo_count * sizeof *placements->items;
    placements->items = naula_memory_allocate(bytes);
    if (placements->items == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    memset(placements->items, 0, bytes);
    placements->count = input->repo_count;
    for (size_t index = 0; index < input->repo_count; index++) {
        placements->items[index].repo = &input->repos[index];
        placements->items[index].org = LAYOUT_NONE;
        naula_status status = place_one(input, tree, &placements->items[index], refusal);
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

static naula_status report_pair_side(layout_builder *builder, const char *code, const layout_placement *reported,
                                     const layout_placement *other, const char *why) {
    return layout_report(builder, code, layout_placement_subject(reported), layout_placement_subject(other),
                         "%s (%s) and %s (%s) %s; neither is moved, linked or removed", layout_placement_name(reported),
                         reported->repo->id.commit, layout_placement_name(other), other->repo->id.commit, why);
}

static naula_status refuse_pair(layout_builder *builder, const char *code, layout_placement *first,
                                layout_placement *second, const char *why) {
    first->stays = 1;
    second->stays = 1;
    first->removes = 0;
    second->removes = 0;
    naula_status status = report_pair_side(builder, code, first, second, why);
    return status == NAULA_OK ? report_pair_side(builder, code, second, first, why) : status;
}

static int has_checkout(const layout_placement *placement) {
    return placement->org != LAYOUT_NONE && placement->checkout != NULL;
}

static const char *id_clash(const layout_placement *left, const layout_placement *right, const char **code) {
    if (left->homeless || right->homeless) {
        *code = "name-collides";
        return layout_path_encloses(left->checkout, right->checkout) &&
                       layout_path_encloses(right->checkout, left->checkout)
                   ? "want one local checkout folder"
                   : NULL;
    }
    if (strcmp(left->repo->id.commit, right->repo->id.commit) == 0) {
        *code = "id-collision";
        return "share one id";
    }
    *code = "twelve-hex-collision";
    return strncmp(left->repo->id.commit, right->repo->id.commit, 12) == 0
               ? "share the first twelve hex digits that name a checkout folder"
               : NULL;
}

naula_status layout_refuse_collisions(layout_placements *placements, layout_builder *builder) {
    for (size_t index = 0; index < placements->count; index++) {
        layout_placement *left = &placements->items[index];
        for (size_t other = index + 1; other < placements->count; other++) {
            layout_placement *right = &placements->items[other];
            if (!has_checkout(left) || !has_checkout(right) || left->homeless != right->homeless) {
                continue;
            }
            const char *code = NULL;
            const char *why = id_clash(left, right, &code);
            if (why != NULL) {
                naula_status status = refuse_pair(builder, code, left, right, why);
                if (status != NAULA_OK) {
                    return status;
                }
            }
        }
    }
    return NAULA_OK;
}

static naula_status block(layout_builder *builder, layout_placement *placement, const char *in_the_way,
                          const char *why) {
    placement->stays = 1;
    placement->removes = 0;
    return layout_report(builder, "path-occupied", placement->repo->current_path, in_the_way,
                         "%s is left where it is: %s %s", layout_placement_name(placement), why,
                         in_the_way == NULL ? "" : in_the_way);
}

/** The first stationary checkout a move or a removal of `placement` would land in or carry along. */
static const char *obstacle_of(const layout_placements *placements, const layout_placement *placement,
                               const char **why) {
    const char *current = placement->repo->current_path;
    for (size_t index = 0; index < placements->count; index++) {
        const layout_placement *other = &placements->items[index];
        const char *other_current = other->repo->current_path;
        if (other == placement || other_current == NULL) {
            continue;
        }
        if (placement->removes && layout_path_encloses(current, other_current)) {
            *why = "removing it would remove the checkout at";
            return other_current;
        }
        if (layout_path_encloses(current, other_current) && layout_path_encloses(other_current, current)) {
            *why = "another repo claims the same checkout at";
            return other_current;
        }
        if (layout_placement_moves(placement) && is_stationary(other) &&
            layout_path_encloses(other_current, placement->checkout)) {
            *why = "its checkout folder is taken by the checkout at";
            return other_current;
        }
        if (layout_placement_moves(placement) && is_stationary(other) &&
            layout_path_strictly_encloses(current, other_current)) {
            *why = "moving it would carry along the checkout at";
            return other_current;
        }
    }
    return NULL;
}

static naula_status block_obstructed(layout_placements *placements, layout_builder *builder, int *changed) {
    *changed = 0;
    for (size_t index = 0; index < placements->count; index++) {
        layout_placement *placement = &placements->items[index];
        if (!layout_placement_moves(placement) && !(placement->removes && !placement->stays)) {
            continue;
        }
        const char *why = NULL;
        const char *in_the_way = obstacle_of(placements, placement, &why);
        if (in_the_way != NULL) {
            *changed = 1;
            naula_status status = block(builder, placement, in_the_way, why);
            if (status != NAULA_OK) {
                return status;
            }
        }
    }
    return NAULA_OK;
}

/** Whether `first` has to move before `second`: it sits where `second` goes, or inside `second`. */
static int must_precede(const layout_placement *first, const layout_placement *second) {
    const char *current = first->repo->current_path;
    return layout_path_encloses(second->checkout, current) ||
           layout_path_strictly_encloses(second->repo->current_path, current);
}

static int is_ready(const layout_placements *placements, const layout_placement *candidate, const int *ordered) {
    for (size_t index = 0; index < placements->count; index++) {
        const layout_placement *other = &placements->items[index];
        if (other != candidate && !ordered[index] && layout_placement_moves(other) && must_precede(other, candidate)) {
            return 0;
        }
    }
    return 1;
}

/** Deepest current path first among the ready ones, so the plan does not depend on the input's order. */
static size_t next_ready(const layout_placements *placements, const int *ordered) {
    size_t best = LAYOUT_NONE;
    for (size_t index = 0; index < placements->count; index++) {
        const layout_placement *candidate = &placements->items[index];
        if (ordered[index] || !layout_placement_moves(candidate) || !is_ready(placements, candidate, ordered)) {
            continue;
        }
        if (best == LAYOUT_NONE ||
            strcmp(candidate->repo->current_path, placements->items[best].repo->current_path) > 0) {
            best = index;
        }
    }
    return best;
}

static naula_status block_unordered(layout_placements *placements, layout_builder *builder, const int *ordered) {
    for (size_t index = 0; index < placements->count; index++) {
        layout_placement *placement = &placements->items[index];
        if (!ordered[index] && layout_placement_moves(placement)) {
            naula_status status = block(builder, placement, placement->checkout,
                                        "it and other checkouts each sit where another must go, at");
            if (status != NAULA_OK) {
                return status;
            }
        }
    }
    return NAULA_OK;
}

static naula_status order_once(layout_placements *placements, layout_builder *builder, int *ordered, int *cycled) {
    size_t sequence = 0;
    *cycled = 0;
    for (size_t index = 0; index < placements->count; index++) {
        ordered[index] = !layout_placement_moves(&placements->items[index]);
    }
    for (size_t ready = next_ready(placements, ordered); ready != LAYOUT_NONE;
         ready = next_ready(placements, ordered)) {
        ordered[ready] = 1;
        placements->items[ready].order = ++sequence;
    }
    for (size_t index = 0; index < placements->count; index++) {
        *cycled |= !ordered[index];
    }
    return *cycled ? block_unordered(placements, builder, ordered) : NAULA_OK;
}

naula_status layout_order_moves(layout_placements *placements, layout_builder *builder) {
    int *ordered = placements->count == 0 ? NULL : naula_memory_allocate(placements->count * sizeof *ordered);
    if (placements->count > 0 && ordered == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    naula_status status = NAULA_OK;
    int unsettled = 1;
    while (status == NAULA_OK && unsettled) {
        int changed = 0;
        status = block_obstructed(placements, builder, &changed);
        if (status == NAULA_OK && !changed) {
            status = order_once(placements, builder, ordered, &unsettled);
        }
    }
    naula_memory_release(ordered);
    return status;
}

void layout_placements_release(layout_placements *placements) {
    for (size_t index = 0; index < placements->count; index++) {
        layout_placement *placement = &placements->items[index];
        naula_memory_release(placement->checkout);
        naula_memory_release(placement->link);
        naula_memory_release(placement->stale_links[0]);
        naula_memory_release(placement->stale_links[1]);
    }
    naula_memory_release(placements->items);
    placements->items = NULL;
    placements->count = 0;
}
