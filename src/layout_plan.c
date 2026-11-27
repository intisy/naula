#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout_internal.h"
#include "naula/memory.h"

static const char INPUT_FIX[] = "set each struct's size to its sizeof and give the input a root and a top org";
static const char PATH_FIX[] = "pass every path absolute and resolved, as the folder it names on disk";
static const char OCCUPIED_FIX[] = "move that checkout out of the org folder's path, then lay the root out again";
static const char LINKED_FIX[] = "replace that link with a real folder, then lay the root out again";

static char *copy_or_null(const char *text, int *failed) {
    if (text == NULL) {
        return NULL;
    }
    char *copy = naula_memory_duplicate_text(text);
    *failed |= copy == NULL;
    return copy;
}

static void release_step(naula_layout_step *step) {
    naula_memory_release(step->path);
    naula_memory_release(step->target);
    naula_memory_release(step->content);
}

static naula_status grow(layout_builder *builder) {
    if (builder->count < builder->capacity) {
        return NAULA_OK;
    }
    size_t capacity = builder->capacity == 0 ? 32 : builder->capacity * 2;
    layout_entry *grown = naula_memory_reallocate(builder->entries, capacity * sizeof *grown);
    if (grown == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    builder->entries = grown;
    builder->capacity = capacity;
    return NAULA_OK;
}

static naula_status add_step(layout_builder *builder, int phase, size_t order, naula_layout_step_kind kind,
                             const char *code, const char *path, const char *target, const char *content) {
    if (grow(builder) != NAULA_OK) {
        return NAULA_OUT_OF_MEMORY;
    }
    layout_entry *entry = &builder->entries[builder->count];
    memset(entry, 0, sizeof *entry);
    entry->phase = phase;
    entry->order = order;
    entry->step.size = sizeof entry->step;
    entry->step.kind = kind;
    snprintf(entry->step.code, sizeof entry->step.code, "%s", code == NULL ? "" : code);
    int failed = 0;
    entry->step.path = copy_or_null(path, &failed);
    entry->step.target = copy_or_null(target, &failed);
    entry->step.content = copy_or_null(content, &failed);
    if (failed) {
        release_step(&entry->step);
        return NAULA_OUT_OF_MEMORY;
    }
    builder->count++;
    return NAULA_OK;
}

naula_status layout_add(layout_builder *builder, int phase, size_t order, naula_layout_step_kind kind,
                        const char *path, const char *target, const char *content) {
    return add_step(builder, phase, order, kind, NULL, path, target, content);
}

naula_status layout_report(layout_builder *builder, const char *code, const char *path, const char *target,
                           const char *format, ...) {
    char detail[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(detail, sizeof detail, format, arguments);
    va_end(arguments);
    return add_step(builder, LAYOUT_PHASE_REPORTS, 0, NAULA_REPORT, code, path, target, detail);
}

void layout_builder_release(layout_builder *builder) {
    for (size_t index = 0; index < builder->count; index++) {
        release_step(&builder->entries[index].step);
    }
    naula_memory_release(builder->entries);
    memset(builder, 0, sizeof *builder);
}

static int compare_text(const char *left, const char *right) {
    if (left == NULL || right == NULL) {
        return (left != NULL) - (right != NULL);
    }
    return strcmp(left, right);
}

static int compare_entries(const void *left_pointer, const void *right_pointer) {
    const layout_entry *left = left_pointer;
    const layout_entry *right = right_pointer;
    if (left->phase != right->phase) {
        return left->phase < right->phase ? -1 : 1;
    }
    if (left->order != right->order) {
        return left->order < right->order ? -1 : 1;
    }
    int compared = compare_text(left->step.path, right->step.path);
    if (compared == 0 && left->step.kind != right->step.kind) {
        compared = left->step.kind < right->step.kind ? -1 : 1;
    }
    if (compared == 0) {
        compared = strcmp(left->step.code, right->step.code);
    }
    if (compared == 0) {
        compared = compare_text(left->step.target, right->step.target);
    }
    return compared == 0 ? compare_text(left->step.content, right->step.content) : compared;
}

static naula_status emit_tree_folders(const layout_tree *tree, layout_builder *builder) {
    naula_status status = NAULA_OK;
    for (size_t index = 0; index < tree->count && status == NAULA_OK; index++) {
        status = layout_add(builder, LAYOUT_PHASE_TREE_FOLDERS, 0, NAULA_MAKE_FOLDER, tree->orgs[index].folder, NULL,
                            NULL);
        if (status == NAULA_OK) {
            status = layout_add(builder, LAYOUT_PHASE_TREE_FOLDERS, 0, NAULA_MAKE_FOLDER,
                                tree->orgs[index].repos_folder, NULL, NULL);
        }
    }
    return status;
}

/** The org or `repos` folder at or below `path`, NULL when there is none. */
static const char *held_tree_folder(const layout_tree *tree, const char *path) {
    for (size_t index = 0; index < tree->count; index++) {
        const layout_org *org = &tree->orgs[index];
        if (layout_path_encloses(path, org->folder)) {
            return org->folder;
        }
        if (layout_path_encloses(path, org->repos_folder)) {
            return org->repos_folder;
        }
    }
    return NULL;
}

/**
 * A checkout holding an org or `repos` folder would take the whole tree along when it moves, and a link naula has no
 * record of making there would put the tree wherever it points.
 */
static naula_status refuse_occupied_tree(const naula_layout_input *input, const layout_tree *tree,
                                         naula_refusal *refusal) {
    for (size_t repo = 0; repo < input->repo_count; repo++) {
        const char *current = input->repos[repo].current_path;
        const char *held = current == NULL ? NULL : held_tree_folder(tree, current);
        if (held != NULL) {
            naula_refuse(refusal, "path-occupied", OCCUPIED_FIX, "the checkout at %s holds the folder %s", current,
                         held);
            return NAULA_REFUSED;
        }
    }
    for (size_t index = 0; index < input->existing_link_count; index++) {
        const naula_layout_link *link = &input->existing_links[index];
        const char *held = link->made_by_naula ? NULL : held_tree_folder(tree, link->path);
        if (held != NULL) {
            naula_refuse(refusal, "path-occupied", LINKED_FIX,
                         "the link at %s, which naula has no record of making, holds the folder %s", link->path,
                         held);
            return NAULA_REFUSED;
        }
    }
    return NAULA_OK;
}

static int links_equal(const layout_placement *left, const layout_placement *right) {
    return layout_path_encloses(left->link, right->link) && layout_path_encloses(right->link, left->link);
}

static int has_link(const layout_placement *placement) {
    return placement->link != NULL && !placement->stays && !placement->removes;
}

/** Why `placement` may not take its link, and who else is involved; NULL when it may. */
static const char *link_clash(const layout_tree *tree, const layout_placements *placements,
                              const layout_placement *placement, const char **code, const char **other) {
    *code = "name-collides";
    *other = NULL;
    if (layout_is_reserved(layout_basename(placement->link))) {
        return "a name naula keeps for itself";
    }
    if (layout_tree_is_folder(tree, placement->link)) {
        return "an org or project folder";
    }
    for (size_t index = 0; index < placements->count; index++) {
        const layout_placement *peer = &placements->items[index];
        const char *peer_current = peer->repo->current_path;
        if (peer == placement) {
            continue;
        }
        if (has_link(peer) && links_equal(peer, placement)) {
            *other = layout_placement_subject(peer);
            return "the link of another repo";
        }
        if (peer_current != NULL && !peer->removes && !layout_placement_moves(peer) &&
            layout_path_encloses(peer_current, placement->link)) {
            *code = "path-occupied";
            *other = peer_current;
            return "a checkout that stays where it is";
        }
    }
    return NULL;
}

static naula_status settle_links(const layout_tree *tree, layout_placements *placements, layout_builder *builder) {
    int *refused = placements->count == 0 ? NULL : naula_memory_allocate(placements->count * sizeof *refused);
    if (placements->count > 0 && refused == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    naula_status status = NAULA_OK;
    for (size_t index = 0; index < placements->count && status == NAULA_OK; index++) {
        layout_placement *placement = &placements->items[index];
        const char *code = NULL;
        const char *other = NULL;
        const char *why = has_link(placement) ? link_clash(tree, placements, placement, &code, &other) : NULL;
        refused[index] = why != NULL;
        if (why != NULL) {
            status = layout_report(builder, code, layout_placement_subject(placement), other,
                                   "%s gets no link: %s collides with %s", layout_placement_name(placement),
                                   placement->link, why);
        }
    }
    for (size_t index = 0; index < placements->count; index++) {
        if (refused[index]) {
            naula_memory_release(placements->items[index].link);
            placements->items[index].link = NULL;
        }
    }
    naula_memory_release(refused);
    return status;
}

static const layout_placement *rules_holder(const layout_placements *placements, const naula_id *repository) {
    for (size_t index = 0; index < placements->count && repository != NULL; index++) {
        const layout_placement *placement = &placements->items[index];
        if (!placement->stays && !placement->removes && !placement->homeless && placement->checkout != NULL &&
            layout_same_repository(&placement->repo->id, repository)) {
            return placement;
        }
    }
    return NULL;
}

/** One import line, through `reached`: the rules repo's link, or its checkout when the link cannot be trusted. */
static char *import_line(const layout_tree *tree, const layout_org *org, const char *reached) {
    char *relative =
        layout_relative(layout_below_root(tree->root, org->folder), layout_below_root(tree->root, reached));
    char *through = relative == NULL ? NULL : layout_join(relative, org->declaration->rules);
    naula_memory_release(relative);
    char *line = through == NULL ? NULL : naula_memory_allocate(strlen(through) + 3);
    if (line != NULL) {
        snprintf(line, strlen(through) + 3, "@%s\n", through);
    }
    naula_memory_release(through);
    return line;
}

static const char *occupant(const layout_placements *placements, const char *path) {
    for (size_t index = 0; index < placements->count; index++) {
        const layout_placement *placement = &placements->items[index];
        const char *current = placement->repo->current_path;
        if (current != NULL && !placement->removes && !layout_placement_moves(placement) &&
            layout_path_encloses(current, path)) {
            return current;
        }
    }
    return NULL;
}

/**
 * The rules are imported through the link only when naula will make it there itself; a link naula has no record of,
 * at that path or above it, or a checkout at that path, could point anywhere, and the import would load whatever
 * rules it points at. A rules repo that is not cloned has no checkout to fall back on, so then nothing is imported.
 */
static const char *rules_path(const naula_layout_input *input, const layout_placements *placements,
                              const layout_placement *holder) {
    int link_is_naulas = holder->link != NULL && layout_unrecorded_link_over(input, holder->link) == NULL &&
                         occupant(placements, holder->link) == NULL;
    if (link_is_naulas) {
        return holder->link;
    }
    return holder->repo->current_path == NULL ? NULL : holder->checkout;
}

static naula_status emit_rules_file(const naula_layout_input *input, const layout_tree *tree,
                                    const layout_placements *placements, const layout_org *org,
                                    layout_builder *builder) {
    char *path = layout_join(org->folder, "CLAUDE.md");
    if (path == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    const layout_placement *holder = rules_holder(placements, org->rules_repository);
    const char *reached = holder == NULL ? NULL : rules_path(input, placements, holder);
    const char *in_the_way = occupant(placements, path);
    const naula_layout_link *linked = layout_unrecorded_link_over(input, path);
    const char *name = org->declaration->name == NULL ? "the top org" : org->declaration->name;
    naula_status status;
    if (reached == NULL) {
        status = layout_report(builder, "rules-repo-missing", org->folder, NULL,
                               "the repo holding the rules of %s is not laid out in this root, so its CLAUDE.md is "
                               "not written",
                               name);
    } else if (in_the_way != NULL) {
        status = layout_report(builder, "path-occupied", path, in_the_way, "the CLAUDE.md of %s sits in a checkout",
                               name);
    } else if (linked != NULL) {
        status = layout_report(builder, "path-occupied", path, linked->path,
                               "the CLAUDE.md of %s is a link naula has no record of making, so it is not written",
                               name);
    } else {
        char *line = import_line(tree, org, reached);
        status = line == NULL ? NAULA_OUT_OF_MEMORY
                              : layout_add(builder, LAYOUT_PHASE_FILES, 0, NAULA_WRITE_FILE, path, NULL, line);
        naula_memory_release(line);
    }
    naula_memory_release(path);
    return status;
}

static naula_status emit_rules_files(const naula_layout_input *input, const layout_tree *tree,
                                     const layout_placements *placements, layout_builder *builder) {
    naula_status status = NAULA_OK;
    for (size_t index = 0; index < tree->count && status == NAULA_OK; index++) {
        if (tree->orgs[index].declaration->rules != NULL) {
            status = emit_rules_file(input, tree, placements, &tree->orgs[index], builder);
        }
    }
    return status;
}

static int is_recorded_link_at(const naula_layout_input *input, const char *path) {
    for (size_t index = 0; index < input->existing_link_count; index++) {
        const naula_layout_link *link = &input->existing_links[index];
        if (link->made_by_naula && layout_path_equal(link->path, path)) {
            return 1;
        }
    }
    return 0;
}

static naula_status emit_link_removal(const naula_layout_input *input, const char *path, const char *target,
                                      layout_builder *builder) {
    const naula_layout_link *foreign = layout_unrecorded_link_over(input, path);
    if (foreign != NULL) {
        return layout_report(builder, "path-occupied", path, foreign->path,
                             "the link naula made here is reached through a link naula has no record of making, so "
                             "it is not removed");
    }
    return layout_add(builder, LAYOUT_PHASE_LINK_REMOVALS, 0, NAULA_REMOVE_LINK, path, target, NULL);
}

static naula_status emit_link(const naula_layout_input *input, const layout_tree *tree,
                              const layout_placement *placement, layout_builder *builder) {
    const naula_layout_link *foreign = layout_unrecorded_link_over(input, placement->link);
    if (foreign != NULL) {
        return layout_report(builder, "path-occupied", placement->link, foreign->path,
                             "%s gets no link: a link naula has no record of making sits where its link goes or "
                             "above it",
                             layout_placement_name(placement));
    }
    char *parent = naula_memory_duplicate_text(placement->link);
    if (parent == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    parent[layout_basename(parent) - parent - 1] = '\0';
    naula_status status = NAULA_OK;
    if (!layout_path_equal(parent, tree->orgs[placement->org].folder)) {
        status = layout_add(builder, LAYOUT_PHASE_LINK_FOLDERS, 0, NAULA_MAKE_FOLDER, parent, NULL, NULL);
    }
    naula_memory_release(parent);
    return status == NAULA_OK ? layout_add(builder, LAYOUT_PHASE_LINKS, 0, NAULA_MAKE_LINK, placement->link,
                                           placement->checkout, NULL)
                              : status;
}

static naula_status emit_removals(const naula_layout_input *input, const layout_placement *placement,
                                   layout_builder *builder) {
    const char *current = placement->repo->current_path;
    naula_status status = NAULA_OK;
    for (size_t index = 0; index < 2 && status == NAULA_OK; index++) {
        if (placement->stale_links[index] != NULL && is_recorded_link_at(input, placement->stale_links[index])) {
            status = emit_link_removal(input, placement->stale_links[index], current, builder);
        }
    }
    if (status == NAULA_OK && placement->removes) {
        status = layout_add(builder, LAYOUT_PHASE_CLONE_REMOVALS, 0, NAULA_REMOVE_CLONE, current, NULL, NULL);
    }
    return status;
}

static naula_status emit_placement(const naula_layout_input *input, const layout_tree *tree,
                                   const layout_placement *placement, layout_builder *builder) {
    if (placement->stays || placement->org == LAYOUT_NONE) {
        return NAULA_OK;
    }
    naula_status status = NAULA_OK;
    if (layout_placement_moves(placement)) {
        status = layout_add(builder, LAYOUT_PHASE_MOVES, placement->order, NAULA_MOVE_CHECKOUT,
                            placement->repo->current_path, placement->checkout, NULL);
    }
    if (status == NAULA_OK && placement->repo->current_path != NULL) {
        status = emit_removals(input, placement, builder);
    }
    if (status == NAULA_OK && placement->link != NULL && !placement->removes) {
        status = emit_link(input, tree, placement, builder);
    }
    return status;
}

static naula_status report_notes(const layout_placement *placement, layout_builder *builder) {
    const naula_layout_repo *repo = placement->repo;
    const char *name = layout_placement_name(placement);
    const char *at = layout_placement_subject(placement);
    naula_status status = NAULA_OK;
    if (placement->notes & LAYOUT_NOTE_ORG_UNKNOWN) {
        return layout_report(builder, "org-unknown", layout_placement_reported_at(placement), NULL,
                             "%s is kept in an org this root does not hold once by that name, so it is left alone",
                             name);
    }
    if (placement->org == LAYOUT_NONE) {
        return repo->current_path == NULL
                   ? NAULA_OK
                   : layout_report(builder, "not-held", repo->current_path, NULL,
                                   "%s belongs to no org this root holds, so its checkout is left alone", name);
    }
    if (placement->notes & LAYOUT_NOTE_OUTSIDE_ROOT) {
        return layout_report(builder, "path-outside-root", repo->current_path, NULL,
                             "the checkout of %s is outside the root, so it is left alone", name);
    }
    if (at == NULL) {
        return NAULA_OK;
    }
    if (placement->notes & LAYOUT_NOTE_NAME_INVALID) {
        status = layout_report(builder, "name-invalid", at, NULL, "the name \"%s\" cannot be a folder, so %s",
                               repo->name == NULL ? "" : repo->name, "the repo gets no link");
    }
    if (status == NAULA_OK && (placement->notes & LAYOUT_NOTE_TWO_PROJECT_TOPICS)) {
        status = layout_report(builder, "two-project-topics", at, NULL,
                               "%s carries more than one project topic of its org, so it gets no link", name);
    }
    if (status == NAULA_OK && repo->archived && !repo->clean_and_pushed && repo->current_path != NULL) {
        status = layout_report(builder, "archived-kept-dirty", at, NULL,
                               "%s is archived but holds changes or commits no home has, so it is kept", name);
    }
    if (status == NAULA_OK && !layout_repo_is_adopted(repo)) {
        status = layout_report(builder, "unadopted", at, NULL,
                               "%s has no repo.json and is placed from its host alone", name);
    }
    return status;
}

static int is_made(const layout_placements *placements, const naula_layout_link *link) {
    for (size_t index = 0; index < placements->count; index++) {
        const layout_placement *placement = &placements->items[index];
        if (placement->link != NULL && !placement->stays && !placement->removes &&
            layout_path_equal(placement->link, link->path) && layout_path_equal(placement->checkout, link->target)) {
            return 1;
        }
    }
    return 0;
}

/** A copy with `/` for every separator and no trailing separator. */
static char *normalized_copy(const char *path) {
    char *copy = naula_memory_duplicate_text(path);
    if (copy == NULL) {
        return NULL;
    }
    size_t length = strlen(copy);
    for (size_t index = 0; index < length; index++) {
        copy[index] = copy[index] == '\\' ? '/' : copy[index];
    }
    while (length > 0 && copy[length - 1] == '/') {
        copy[--length] = '\0';
    }
    return copy;
}

/** Cuts the last segment off `path` in place and answers it, NULL when there is no separator left. */
static char *cut_last_segment(char *path) {
    char *separator = strrchr(path, '/');
    if (separator == NULL) {
        return NULL;
    }
    *separator = '\0';
    return separator + 1;
}

static int has_repos_segment(const char *path) {
    for (const char *segment = path; *segment != '\0';) {
        size_t length = strcspn(segment, "/");
        char name[6] = "";
        if (length == sizeof name - 1) {
            memcpy(name, segment, length);
            if (layout_same_name(name, "repos")) {
                return 1;
            }
        }
        segment += length + (segment[length] == '/');
    }
    return 0;
}

/** A checkout folder name naula makes: twelve lowercase hex digits, or `local-` and a folder for a homeless repo. */
static int is_checkout_name(const char *name) {
    if (strncmp(name, "local-", 6) == 0) {
        return name[6] != '\0';
    }
    size_t length = strlen(name);
    return length == 12 && strspn(name, "0123456789abcdef") == length;
}

static int is_laid_out_folder(const layout_tree *tree, const char *path) {
    for (size_t index = 0; index < tree->count; index++) {
        if (layout_path_equal(tree->orgs[index].folder, path)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Whether `segment` under `folder` may hold naula's links: `archive`, or any folder that is not reserved and is not a
 * laid-out org, since a project folder that was renamed away, or belongs to an org no longer held, is named nowhere.
 */
static int may_hold_links(const layout_tree *tree, const char *folder, const char *segment, int *failed) {
    if (layout_same_name(segment, "archive")) {
        return 1;
    }
    if (layout_is_reserved(segment)) {
        return 0;
    }
    char *joined = layout_join(folder, segment);
    if (joined == NULL) {
        *failed = 1;
        return 0;
    }
    int laid_out = is_laid_out_folder(tree, joined);
    naula_memory_release(joined);
    return !laid_out;
}

static int is_inside_a_checkout(const naula_layout_input *input, const char *path) {
    for (size_t index = 0; index < input->repo_count; index++) {
        const char *current = input->repos[index].current_path;
        if (current != NULL && layout_path_strictly_encloses(current, path)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Whether naula could have made `link`: its target is a checkout folder directly in some org folder's `repos`, and the
 * link sits where that org folder keeps links, at its top or one folder down, outside every checkout.
 */
static int is_naula_link_place(const naula_layout_input *input, const layout_tree *tree, char *folder,
                               const char *path) {
    const char *checkout = cut_last_segment(folder);
    if (checkout == NULL || !is_checkout_name(checkout)) {
        return 0;
    }
    const char *repos = cut_last_segment(folder);
    if (repos == NULL || !layout_same_name(repos, "repos") || !layout_path_encloses(tree->root, folder) ||
        has_repos_segment(layout_below_root(tree->root, folder)) || !layout_path_strictly_encloses(folder, path) ||
        is_inside_a_checkout(input, path)) {
        return 0;
    }
    char *rest = naula_memory_duplicate_text(path + strlen(folder) + 1);
    if (rest == NULL) {
        return -1;
    }
    const char *name = cut_last_segment(rest);
    int failed = 0;
    int answer = name == NULL                ? !layout_is_reserved(rest)
                 : strchr(rest, '/') != NULL ? 0
                                             : may_hold_links(tree, folder, rest, &failed);
    naula_memory_release(rest);
    return failed ? -1 : answer;
}

static naula_status is_stale_link(const naula_layout_input *input, const layout_tree *tree,
                                  const naula_layout_link *link, int *stale) {
    char *folder = normalized_copy(link->target);
    char *path = normalized_copy(link->path);
    int answer = folder == NULL || path == NULL ? -1 : is_naula_link_place(input, tree, folder, path);
    naula_memory_release(folder);
    naula_memory_release(path);
    *stale = answer == 1;
    return answer < 0 ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}

static naula_status emit_stale_links(const naula_layout_input *input, const layout_tree *tree,
                                     const layout_placements *placements, layout_builder *builder) {
    naula_status status = NAULA_OK;
    for (size_t index = 0; index < input->existing_link_count && status == NAULA_OK; index++) {
        const naula_layout_link *link = &input->existing_links[index];
        int stale = 0;
        status = !link->made_by_naula || is_made(placements, link) ? NAULA_OK
                                                                   : is_stale_link(input, tree, link, &stale);
        if (status == NAULA_OK && stale) {
            status = emit_link_removal(input, link->path, link->target, builder);
        }
    }
    return status;
}

static naula_status report_confirmation(const layout_tree *tree, const layout_placement *placement,
                                        layout_builder *builder) {
    const char *problem = layout_tree_confirmation_problem(tree, placement->repo);
    if (problem == NULL) {
        return NAULA_OK;
    }
    const char *why = strcmp(problem, "member-of-unknown") == 0 ? "names no repo declaring an org this root holds"
                      : strcmp(problem, "org-name-taken") == 0
                          ? "names an org left out because something accepted owns its folder"
                          : "names an org that does not list the account of its primary home";
    return layout_report(builder, problem, layout_placement_reported_at(placement), NULL,
                         "the memberOf of %s %s, so it confirms nothing", layout_placement_name(placement), why);
}

static naula_status emit_placements(const naula_layout_input *input, const layout_tree *tree,
                                    const layout_placements *placements, layout_builder *builder) {
    naula_status status = NAULA_OK;
    for (size_t index = 0; index < placements->count && status == NAULA_OK; index++) {
        status = emit_placement(input, tree, &placements->items[index], builder);
        if (status == NAULA_OK) {
            status = report_notes(&placements->items[index], builder);
        }
        if (status == NAULA_OK) {
            status = report_confirmation(tree, &placements->items[index], builder);
        }
    }
    return status;
}

static int same_entry(const layout_entry *left, const layout_entry *right) {
    return compare_entries(left, right) == 0;
}

static naula_status finish(layout_builder *builder, naula_layout_plan *plan) {
    if (builder->count > 0) {
        qsort(builder->entries, builder->count, sizeof *builder->entries, compare_entries);
    }
    size_t kept = 0;
    for (size_t index = 0; index < builder->count; index++) {
        if (kept > 0 && same_entry(&builder->entries[kept - 1], &builder->entries[index])) {
            release_step(&builder->entries[index].step);
            continue;
        }
        builder->entries[kept++] = builder->entries[index];
    }
    builder->count = kept;
    plan->steps = kept == 0 ? NULL : naula_memory_allocate(kept * sizeof *plan->steps);
    if (kept > 0 && plan->steps == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < kept; index++) {
        plan->steps[index] = builder->entries[index].step;
    }
    plan->step_count = kept;
    naula_memory_release(builder->entries);
    memset(builder, 0, sizeof *builder);
    return NAULA_OK;
}

static int input_holds_first_shape(const naula_layout_input *input) {
    return input->size >= offsetof(naula_layout_input, existing_link_count) + sizeof input->existing_link_count;
}

static int plan_holds_first_shape(const naula_layout_plan *plan) {
    return plan->size >= offsetof(naula_layout_plan, step_count) + sizeof plan->step_count;
}

static naula_status refuse_path(naula_refusal *refusal, const char *what, size_t index, const char *path) {
    naula_refuse(refusal, "layout-input-invalid", PATH_FIX,
                 "%s[%zu] \"%s\" is not an absolute path free of empty, . and .. segments", what, index,
                 path == NULL ? "" : path);
    return NAULA_REFUSED;
}

static naula_status check_paths(const naula_layout_input *input, naula_refusal *refusal) {
    for (size_t index = 0; index < input->repo_count; index++) {
        const naula_layout_repo *repo = &input->repos[index];
        int holds_path = repo->size >= offsetof(naula_layout_repo, current_path) + sizeof repo->current_path;
        if (holds_path && repo->current_path != NULL && !layout_is_clean_absolute(repo->current_path)) {
            return refuse_path(refusal, "repos", index, repo->current_path);
        }
        const naula_reference *member_of = layout_repo_member_of(repo);
        if (member_of != NULL && member_of->size < offsetof(naula_reference, at_count) + sizeof member_of->at_count) {
            naula_refuse(refusal, "layout-input-invalid", INPUT_FIX,
                         "repos[%zu].member_of is smaller than its first shape", index);
            return NAULA_REFUSED;
        }
    }
    for (size_t index = 0; index < input->existing_link_count; index++) {
        const naula_layout_link *link = &input->existing_links[index];
        if (link->size < offsetof(naula_layout_link, made_by_naula) + sizeof link->made_by_naula) {
            naula_refuse(refusal, "layout-input-invalid", INPUT_FIX,
                         "existing_links[%zu] is smaller than its first shape", index);
            return NAULA_REFUSED;
        }
        if (link->path == NULL || !layout_is_clean_absolute(link->path)) {
            return refuse_path(refusal, "existing_links path", index, link->path);
        }
        if (link->target == NULL || !layout_is_clean_absolute(link->target)) {
            return refuse_path(refusal, "existing_links target", index, link->target);
        }
    }
    return NAULA_OK;
}

/** A whole drive or `/` as the root would make every folder on it the layout's to move and remove. */
static int is_filesystem_root(const char *root) {
    size_t length = strlen(root);
    while (length > 0 && (root[length - 1] == '/' || root[length - 1] == '\\')) {
        length--;
    }
    return length < layout_filesystem_root_length(root);
}

static naula_status check_input(const naula_layout_input *input, naula_refusal *refusal) {
    const char *missing = !input_holds_first_shape(input) ? "the input is smaller than its first shape"
                          : input->root == NULL           ? "the input names no root"
                          : input->top == NULL            ? "the input names no top org"
                          : (input->org_count > 0 && input->orgs == NULL) ||
                                  (input->repo_count > 0 && input->repos == NULL) ||
                                  (input->extra_count > 0 && input->extra == NULL) ||
                                  (input->dropped_count > 0 && input->dropped == NULL) ||
                                  (input->existing_link_count > 0 && input->existing_links == NULL)
                              ? "a count is set for a list that is NULL"
                          : !layout_is_clean_absolute(input->root)
                              ? "the root is not an absolute path free of empty, . and .. segments"
                          : is_filesystem_root(input->root) ? "the root is a whole filesystem, not a folder on one"
                                                            : NULL;
    if (missing != NULL) {
        naula_refuse(refusal, "layout-input-invalid", INPUT_FIX, "%s", missing);
        return NAULA_REFUSED;
    }
    for (size_t index = 0; index < input->extra_count; index++) {
        if (input->extra[index].size < offsetof(naula_reference, at_count) + sizeof input->extra[index].at_count) {
            naula_refuse(refusal, "layout-input-invalid", INPUT_FIX, "extra[%zu] is smaller than its first shape",
                         index);
            return NAULA_REFUSED;
        }
    }
    return check_paths(input, refusal);
}

static naula_status plan_layout(const naula_layout_input *input, layout_tree *tree, layout_placements *placements,
                                layout_builder *builder, naula_refusal *refusal) {
    naula_status status = layout_tree_build(input, tree, builder, refusal);
    if (status == NAULA_OK) {
        status = refuse_occupied_tree(input, tree, refusal);
    }
    if (status == NAULA_OK) {
        status = layout_place(input, tree, placements, refusal);
    }
    if (status == NAULA_OK) {
        status = layout_refuse_collisions(placements, builder);
    }
    if (status == NAULA_OK) {
        status = layout_guard_naula_paths(input, tree, placements, builder);
    }
    if (status == NAULA_OK) {
        status = layout_order_moves(placements, builder);
    }
    if (status == NAULA_OK) {
        status = settle_links(tree, placements, builder);
    }
    if (status == NAULA_OK) {
        status = emit_tree_folders(tree, builder);
    }
    if (status == NAULA_OK) {
        status = emit_placements(input, tree, placements, builder);
    }
    if (status == NAULA_OK) {
        status = emit_rules_files(input, tree, placements, builder);
    }
    if (status == NAULA_OK) {
        status = emit_stale_links(input, tree, placements, builder);
    }
    return status;
}

naula_status naula_layout_plan_make(const naula_layout_input *input, naula_layout_plan *plan, naula_refusal *refusal) {
    if (plan == NULL || !plan_holds_first_shape(plan)) {
        naula_refuse(refusal, "layout-input-invalid", INPUT_FIX, "the plan is NULL or smaller than its first shape");
        return NAULA_REFUSED;
    }
    plan->steps = NULL;
    plan->step_count = 0;
    if (input == NULL) {
        naula_refuse(refusal, "layout-input-invalid", INPUT_FIX, "the input is NULL");
        return NAULA_REFUSED;
    }
    naula_status status = check_input(input, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    layout_tree tree;
    layout_placements placements = {NULL, 0};
    layout_builder builder = {NULL, 0, 0};
    status = plan_layout(input, &tree, &placements, &builder, refusal);
    if (status == NAULA_OK) {
        status = finish(&builder, plan);
    }
    layout_builder_release(&builder);
    layout_placements_release(&placements);
    layout_tree_release(&tree);
    if (status != NAULA_OK) {
        naula_layout_plan_release(plan);
    }
    return status;
}

void naula_layout_plan_release(naula_layout_plan *plan) {
    if (plan == NULL) {
        return;
    }
    for (size_t index = 0; index < plan->step_count; index++) {
        release_step(&plan->steps[index]);
    }
    naula_memory_release(plan->steps);
    plan->steps = NULL;
    plan->step_count = 0;
}
