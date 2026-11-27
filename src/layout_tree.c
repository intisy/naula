#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "layout_internal.h"
#include "naula/memory.h"
#include "repo_file_internal.h"

#define LAYOUT_ORG_LIMIT 4096u

static const char INPUT_FIX[] = "set each struct's size to its sizeof and give the input a root and a top org";
static const char NAME_FIX[] = "name each org, project and rules path with letters, digits, dot, dash and "
                               "underscore, never ., .., .git, a trailing dot or a Windows device name";
static const char COLLISION_FIX[] = "rename the org or project so no two folders of one org folder share a name, "
                                    "ignoring case, and none is repos, archive or CLAUDE.md";
static const char ACCOUNT_FIX[] = "list each host account in the org member of one org only";
static const char CYCLE_FIX[] = "point one of these orgs' parent at an org above them";

static int holds_account(const layout_tree *tree, const layout_org *org, const naula_home *home);
static const naula_home *primary_of(const naula_layout_input *input, const naula_id *id);

static int declaration_holds_first_shape(const naula_org_declaration *org) {
    return org->size >= offsetof(naula_org_declaration, hold_count) + sizeof org->hold_count;
}

static const char *name_of(const layout_org *org) {
    return org->declaration->name == NULL ? "(unnamed)" : org->declaration->name;
}

static naula_status add_org(layout_tree *tree, const naula_org_declaration *declaration, const naula_id *repository,
                            size_t parent, naula_refusal *refusal) {
    if (tree->count == LAYOUT_ORG_LIMIT) {
        naula_refuse(refusal, "layout-input-invalid", INPUT_FIX, "more than %u orgs", LAYOUT_ORG_LIMIT);
        return NAULA_REFUSED;
    }
    if (tree->count == tree->capacity) {
        size_t capacity = tree->capacity == 0 ? 8 : tree->capacity * 2;
        layout_org *grown = naula_memory_reallocate(tree->orgs, capacity * sizeof *grown);
        if (grown == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        tree->orgs = grown;
        tree->capacity = capacity;
    }
    layout_org *org = &tree->orgs[tree->count++];
    memset(org, 0, sizeof *org);
    org->declaration = declaration;
    org->repository = repository;
    org->rules_repository = repository;
    org->parent = parent;
    return NAULA_OK;
}

static int is_dropped(const naula_layout_input *input, const char *name) {
    for (size_t index = 0; index < input->dropped_count && name != NULL; index++) {
        if (input->dropped[index] != NULL && layout_same_name(input->dropped[index], name)) {
            return 1;
        }
    }
    return 0;
}

static int is_declared_twice(const layout_tree *tree, const naula_id *repository) {
    for (size_t index = 0; index < tree->count && repository != NULL; index++) {
        if (tree->orgs[index].repository != NULL && layout_same_repository(tree->orgs[index].repository, repository)) {
            return 1;
        }
    }
    return 0;
}

static naula_status collect_held(const naula_layout_input *input, layout_tree *tree, naula_refusal *refusal) {
    naula_status status = add_org(tree, input->top, input->top_repository, LAYOUT_NONE, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    tree->orgs[0].is_top = 1;
    tree->orgs[0].declarer = input->top_repository == NULL ? NULL : primary_of(input, input->top_repository);
    for (size_t index = 0; index < input->org_count; index++) {
        const naula_org_declaration *org = &input->orgs[index];
        const naula_id *repository = input->org_repositories == NULL ? NULL : &input->org_repositories[index];
        if (!declaration_holds_first_shape(org)) {
            naula_refuse(refusal, "layout-input-invalid", INPUT_FIX, "orgs[%zu] is smaller than its first shape",
                         index);
            return NAULA_REFUSED;
        }
        if (is_dropped(input, org->name) || is_declared_twice(tree, repository)) {
            continue;
        }
        status = add_org(tree, org, repository, 0, refusal);
        if (status != NAULA_OK) {
            return status;
        }
        tree->orgs[tree->count - 1].declarer = repository == NULL ? NULL : primary_of(input, repository);
    }
    return NAULA_OK;
}

static const naula_home *primary_of(const naula_layout_input *input, const naula_id *id) {
    for (size_t index = 0; index < input->repo_count; index++) {
        if (layout_same_repository(&input->repos[index].id, id)) {
            return &input->repos[index].primary;
        }
    }
    return NULL;
}

static size_t org_declared_by(const naula_layout_input *input, const layout_tree *tree,
                              const naula_reference *reference) {
    for (size_t index = 0; index < tree->count; index++) {
        const naula_id *repository = tree->orgs[index].repository;
        if (repository != NULL &&
            layout_reference_names(reference, repository, primary_of(input, repository))) {
            return index;
        }
    }
    return LAYOUT_NONE;
}

static naula_status collect_confirmations(const naula_layout_input *input, layout_tree *tree) {
    size_t count = 0;
    for (size_t index = 0; index < input->repo_count; index++) {
        count += layout_repo_member_of(&input->repos[index]) != NULL;
    }
    if (count == 0) {
        return NAULA_OK;
    }
    tree->confirmations = naula_memory_allocate(count * sizeof *tree->confirmations);
    if (tree->confirmations == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < input->repo_count; index++) {
        const naula_reference *member_of = layout_repo_member_of(&input->repos[index]);
        if (member_of == NULL) {
            continue;
        }
        size_t declared = org_declared_by(input, tree, member_of);
        layout_confirmation *confirmation = &tree->confirmations[tree->confirmation_count++];
        confirmation->repo = &input->repos[index];
        confirmation->declaring = declared == LAYOUT_NONE ? NULL : tree->orgs[declared].repository;
    }
    return NAULA_OK;
}

static int lists_suborg(const layout_org *parent, const char *name) {
    for (size_t index = 0; index < parent->declaration->suborg_count && name != NULL; index++) {
        const char *listed = parent->declaration->suborgs[index].name;
        if (listed != NULL && layout_same_name(listed, name)) {
            return 1;
        }
    }
    return 0;
}

/** Naming a parent proves nothing, since anyone can; the parent's own list or account has to agree. */
static int parent_accepts(const layout_tree *tree, const layout_org *parent, const layout_org *org) {
    return lists_suborg(parent, org->declaration->name) ||
           (org->declarer != NULL && holds_account(tree, parent, org->declarer));
}

/** A parent this root does not hold leaves the org under the root, which is the top org's folder. */
static void resolve_parents(const naula_layout_input *input, layout_tree *tree) {
    for (size_t index = 1; index < tree->count; index++) {
        layout_org *org = &tree->orgs[index];
        size_t found = org->declaration->parent == NULL ? LAYOUT_NONE
                                                         : org_declared_by(input, tree, org->declaration->parent);
        if (found != LAYOUT_NONE && found != 0 && found != index && !parent_accepts(tree, &tree->orgs[found], org)) {
            org->not_listing = 1;
            found = LAYOUT_NONE;
        }
        org->parent = found == LAYOUT_NONE ? 0 : found;
    }
}

static naula_status refuse_cycle_through(const layout_tree *tree, size_t start, naula_refusal *refusal) {
    char chain[512] = "";
    size_t used = 0;
    size_t current = start;
    for (size_t step = 0; step < tree->count && used < sizeof chain; step++) {
        int written = snprintf(chain + used, sizeof chain - used, "%s%s", step == 0 ? "" : " -> ",
                               name_of(&tree->orgs[current]));
        used += written < 0 ? 0 : (size_t)written;
        current = tree->orgs[current].parent;
        if (current == start) {
            break;
        }
    }
    naula_refuse(refusal, "parent-cycle", CYCLE_FIX, "the orgs %s name each other as parents", chain);
    return NAULA_REFUSED;
}

static naula_status refuse_cycles(const layout_tree *tree, naula_refusal *refusal) {
    for (size_t index = 1; index < tree->count; index++) {
        size_t current = index;
        size_t steps = 0;
        while (current != 0 && current != LAYOUT_NONE && steps <= tree->count) {
            current = tree->orgs[current].parent;
            steps++;
        }
        if (steps > tree->count) {
            size_t inside = index;
            for (size_t step = 0; step < tree->count; step++) {
                inside = tree->orgs[inside].parent;
            }
            return refuse_cycle_through(tree, inside, refusal);
        }
    }
    return NAULA_OK;
}

static int has_own_declaration(const layout_tree *tree, size_t parent, const char *name) {
    for (size_t index = 1; index < tree->count; index++) {
        const layout_org *org = &tree->orgs[index];
        if (org->repository != NULL && org->parent == parent && org->declaration->name != NULL && name != NULL &&
            layout_same_name(org->declaration->name, name)) {
            return 1;
        }
    }
    return 0;
}

static naula_status add_suborgs(const naula_layout_input *input, layout_tree *tree, naula_refusal *refusal) {
    for (size_t index = 0; index < tree->count; index++) {
        const naula_org_declaration *declaration = tree->orgs[index].declaration;
        for (size_t suborg = 0; suborg < declaration->suborg_count; suborg++) {
            const naula_org_declaration *entry = &declaration->suborgs[suborg];
            if (!declaration_holds_first_shape(entry)) {
                naula_refuse(refusal, "layout-input-invalid", INPUT_FIX,
                             "a suborg of %s is smaller than its first shape", name_of(&tree->orgs[index]));
                return NAULA_REFUSED;
            }
            if (is_dropped(input, entry->name) || has_own_declaration(tree, index, entry->name)) {
                continue;
            }
            const naula_id *rules_repository = tree->orgs[index].rules_repository;
            naula_status status = add_org(tree, entry, NULL, index, refusal);
            if (status != NAULA_OK) {
                return status;
            }
            tree->orgs[tree->count - 1].rules_repository = rules_repository;
            tree->orgs[tree->count - 1].declarer = tree->orgs[index].declarer;
        }
    }
    return NAULA_OK;
}

static naula_status set_folder(layout_tree *tree, size_t index, naula_refusal *refusal) {
    layout_org *org = &tree->orgs[index];
    if (org->folder != NULL) {
        return NAULA_OK;
    }
    if (org->is_top) {
        org->folder = naula_memory_duplicate_text(tree->root);
    } else {
        const char *name = org->declaration->name;
        if (name == NULL || !repo_file_is_folder_name(name)) {
            naula_refuse(refusal, "name-invalid", NAME_FIX, "the org name \"%s\" cannot be a folder",
                         name == NULL ? "" : name);
            return NAULA_REFUSED;
        }
        naula_status status = set_folder(tree, org->parent, refusal);
        if (status != NAULA_OK) {
            return status;
        }
        org->folder = layout_join(tree->orgs[org->parent].folder, name);
    }
    org->repos_folder = org->folder == NULL ? NULL : layout_join(org->folder, "repos");
    return org->repos_folder == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}

static naula_status set_folders(layout_tree *tree, naula_refusal *refusal) {
    for (size_t index = 0; index < tree->count; index++) {
        naula_status status = set_folder(tree, index, refusal);
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

int layout_org_has_project(const layout_org *org, const char *name) {
    for (size_t index = 0; index < org->declaration->project_count; index++) {
        if (layout_same_name(org->declaration->project_names[index], name)) {
            return 1;
        }
    }
    return 0;
}

static naula_status check_projects(const layout_org *org, naula_refusal *refusal) {
    const naula_org_declaration *declaration = org->declaration;
    for (size_t index = 0; index < declaration->project_count; index++) {
        const char *project = declaration->project_names[index];
        if (project == NULL || !repo_file_is_folder_name(project)) {
            naula_refuse(refusal, "name-invalid", NAME_FIX, "the project \"%s\" of %s cannot be a folder",
                         project == NULL ? "" : project, name_of(org));
            return NAULA_REFUSED;
        }
        int repeated = 0;
        for (size_t earlier = 0; earlier < index; earlier++) {
            repeated |= layout_same_name(declaration->project_names[earlier], project);
        }
        if (layout_is_reserved(project) || repeated) {
            naula_refuse(refusal, "name-collides", COLLISION_FIX, "the project %s of %s collides with %s", project,
                         name_of(org), repeated ? "another project" : "a folder naula keeps");
            return NAULA_REFUSED;
        }
    }
    if (declaration->rules != NULL &&
        (!repo_file_is_relative_path(declaration->rules) || !repo_file_is_one_line(declaration->rules))) {
        naula_refuse(refusal, "name-invalid", NAME_FIX, "the rules path \"%s\" of %s is not a path inside its repo",
                     declaration->rules, name_of(org));
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

static naula_status check_org_folder(const layout_tree *tree, size_t index, naula_refusal *refusal) {
    const layout_org *org = &tree->orgs[index];
    const layout_org *parent = &tree->orgs[org->parent];
    const char *name = org->declaration->name;
    const char *collides = layout_is_reserved(name) ? "a folder naula keeps"
                           : layout_org_has_project(parent, name) ? "a project of its parent"
                                                                   : NULL;
    for (size_t other = 1; other < tree->count && collides == NULL; other++) {
        if (other != index && tree->orgs[other].parent == org->parent &&
            layout_same_name(tree->orgs[other].declaration->name, name)) {
            collides = "another org";
        }
    }
    if (collides != NULL) {
        naula_refuse(refusal, "name-collides", COLLISION_FIX, "the org folder %s collides with %s", org->folder,
                     collides);
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

static naula_status check_names(const layout_tree *tree, naula_refusal *refusal) {
    for (size_t index = 0; index < tree->count; index++) {
        naula_status status = check_projects(&tree->orgs[index], refusal);
        if (status == NAULA_OK && index > 0) {
            status = check_org_folder(tree, index, refusal);
        }
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

static int same_host(const char *provider, const char *left, const char *right) {
    int is_github = layout_same_name(provider, "github");
    const char *left_host = left == NULL && is_github ? "github.com" : left;
    const char *right_host = right == NULL && is_github ? "github.com" : right;
    if (left_host == NULL || right_host == NULL) {
        return left_host == right_host;
    }
    return layout_same_name(left_host, right_host);
}

static int same_account(const naula_home *left, const naula_home *right) {
    return left->kind == NAULA_HOME_PROVIDER && right->kind == NAULA_HOME_PROVIDER && left->provider != NULL &&
           right->provider != NULL && left->account != NULL && right->account != NULL &&
           layout_same_name(left->provider, right->provider) && same_host(left->provider, left->host, right->host) &&
           layout_same_name(left->account, right->account);
}

/** Only a repo whose primary home is in `account` can confirm it, since only someone who can push there wrote it. */
static int is_confirmed(const layout_tree *tree, const layout_org *org, const naula_home *account) {
    for (size_t index = 0; index < tree->confirmation_count && org->rules_repository != NULL; index++) {
        const layout_confirmation *confirmation = &tree->confirmations[index];
        if (confirmation->declaring != NULL &&
            layout_same_repository(confirmation->declaring, org->rules_repository) &&
            same_account(&confirmation->repo->primary, account)) {
            return 1;
        }
    }
    return 0;
}

static int owns_account(const layout_tree *tree, const layout_org *org, const naula_home *account) {
    return (org->declarer != NULL && same_account(org->declarer, account)) || is_confirmed(tree, org, account);
}

static int holds_account(const layout_tree *tree, const layout_org *org, const naula_home *home) {
    for (size_t index = 0; index < org->declaration->account_count; index++) {
        const naula_home *account = &org->declaration->accounts[index];
        if (owns_account(tree, org, account) && same_account(account, home)) {
            return 1;
        }
    }
    return 0;
}

static naula_status check_accounts(const layout_tree *tree, naula_refusal *refusal) {
    for (size_t index = 0; index < tree->count; index++) {
        const naula_org_declaration *declaration = tree->orgs[index].declaration;
        for (size_t account = 0; account < declaration->account_count; account++) {
            const naula_home *home = &declaration->accounts[account];
            for (size_t other = index + 1; other < tree->count && owns_account(tree, &tree->orgs[index], home);
                 other++) {
                if (holds_account(tree, &tree->orgs[other], home)) {
                    naula_refuse(refusal, "account-held-twice", ACCOUNT_FIX,
                                 "the account %s on %s is held by both %s and %s", home->account, home->provider,
                                 name_of(&tree->orgs[index]), name_of(&tree->orgs[other]));
                    return NAULA_REFUSED;
                }
            }
        }
    }
    return NAULA_OK;
}

static naula_status report_unowned_accounts(const layout_tree *tree, const layout_org *org,
                                            layout_builder *builder) {
    naula_status status = NAULA_OK;
    for (size_t index = 0; index < org->declaration->account_count && status == NAULA_OK; index++) {
        const naula_home *account = &org->declaration->accounts[index];
        if (!owns_account(tree, org, account)) {
            status = layout_report(builder, "account-not-owned", org->folder, NULL,
                                   "%s names the account %s on %s, but the repo declaring it is not under that "
                                   "account and no repo of that account confirms it with memberOf, so the claim is "
                                   "ignored",
                                   name_of(org), account->account == NULL ? "" : account->account,
                                   account->provider == NULL ? "" : account->provider);
        }
    }
    return status;
}

static naula_status report_claims(const layout_tree *tree, layout_builder *builder) {
    naula_status status = NAULA_OK;
    for (size_t index = 0; index < tree->count && status == NAULA_OK; index++) {
        const layout_org *org = &tree->orgs[index];
        status = report_unowned_accounts(tree, org, builder);
        if (status == NAULA_OK && org->not_listing) {
            status = layout_report(builder, "parent-not-listing", org->folder, NULL,
                                   "%s names a parent that neither lists it nor holds its repo's account, so it is "
                                   "laid out under the root",
                                   name_of(org));
        }
    }
    return status;
}

static naula_status report_suborgs_declaring_themselves(const layout_tree *tree, layout_builder *builder) {
    for (size_t index = 0; index < tree->count; index++) {
        const naula_org_declaration *declaration = tree->orgs[index].declaration;
        for (size_t suborg = 0; suborg < declaration->suborg_count; suborg++) {
            const char *name = declaration->suborgs[suborg].name;
            if (name == NULL || !has_own_declaration(tree, index, name)) {
                continue;
            }
            char *folder = layout_join(tree->orgs[index].folder, name);
            naula_status status = folder == NULL
                                      ? NAULA_OUT_OF_MEMORY
                                      : layout_report(builder, "suborg-declares-itself", folder, NULL,
                                                      "%s lists the sub-org %s, which also declares itself: the entry "
                                                      "authorizes the nesting and %s's own declaration supplies its "
                                                      "members",
                                                      name_of(&tree->orgs[index]), name, name);
            naula_memory_release(folder);
            if (status != NAULA_OK) {
                return status;
            }
        }
    }
    return NAULA_OK;
}

static char *normalized_root(const char *root) {
    char *copy = naula_memory_duplicate_text(root);
    if (copy == NULL) {
        return NULL;
    }
    size_t length = strlen(copy);
    for (size_t index = 0; index < length; index++) {
        copy[index] = copy[index] == '\\' ? '/' : copy[index];
    }
    while (length > layout_filesystem_root_length(copy) && copy[length - 1] == '/') {
        copy[--length] = '\0';
    }
    return copy;
}

naula_status layout_tree_build(const naula_layout_input *input, layout_tree *tree, layout_builder *builder,
                               naula_refusal *refusal) {
    memset(tree, 0, sizeof *tree);
    if (!declaration_holds_first_shape(input->top)) {
        naula_refuse(refusal, "layout-input-invalid", INPUT_FIX, "top is smaller than its first shape");
        return NAULA_REFUSED;
    }
    tree->root = normalized_root(input->root);
    naula_status status = tree->root == NULL ? NAULA_OUT_OF_MEMORY : collect_held(input, tree, refusal);
    if (status == NAULA_OK) {
        status = collect_confirmations(input, tree);
    }
    if (status == NAULA_OK) {
        resolve_parents(input, tree);
    }
    if (status == NAULA_OK) {
        status = refuse_cycles(tree, refusal);
    }
    if (status == NAULA_OK) {
        status = add_suborgs(input, tree, refusal);
    }
    if (status == NAULA_OK) {
        status = set_folders(tree, refusal);
    }
    if (status == NAULA_OK) {
        status = check_names(tree, refusal);
    }
    if (status == NAULA_OK) {
        status = check_accounts(tree, refusal);
    }
    if (status == NAULA_OK) {
        status = report_suborgs_declaring_themselves(tree, builder);
    }
    if (status == NAULA_OK) {
        status = report_claims(tree, builder);
    }
    return status;
}

void layout_tree_release(layout_tree *tree) {
    for (size_t index = 0; index < tree->count; index++) {
        naula_memory_release(tree->orgs[index].folder);
        naula_memory_release(tree->orgs[index].repos_folder);
    }
    naula_memory_release(tree->orgs);
    naula_memory_release(tree->root);
    naula_memory_release(tree->confirmations);
    memset(tree, 0, sizeof *tree);
}

const char *layout_tree_confirmation_problem(const layout_tree *tree, const naula_layout_repo *repo) {
    for (size_t index = 0; index < tree->confirmation_count; index++) {
        const layout_confirmation *confirmation = &tree->confirmations[index];
        if (confirmation->repo != repo) {
            continue;
        }
        if (confirmation->declaring == NULL) {
            return "member-of-unknown";
        }
        for (size_t org = 0; org < tree->count; org++) {
            const layout_org *candidate = &tree->orgs[org];
            if (candidate->rules_repository != NULL &&
                layout_same_repository(candidate->rules_repository, confirmation->declaring) &&
                holds_account(tree, candidate, &repo->primary)) {
                return NULL;
            }
        }
        return "member-of-unlisted";
    }
    return NULL;
}

size_t layout_tree_account_owner(const layout_tree *tree, const naula_home *home) {
    for (size_t index = 0; index < tree->count; index++) {
        if (holds_account(tree, &tree->orgs[index], home)) {
            return index;
        }
    }
    return LAYOUT_NONE;
}

size_t layout_tree_named(const layout_tree *tree, const char *name, int *ambiguous) {
    size_t found = LAYOUT_NONE;
    *ambiguous = 0;
    for (size_t index = 0; index < tree->count; index++) {
        const char *candidate = tree->orgs[index].declaration->name;
        if (candidate != NULL && layout_same_name(candidate, name)) {
            *ambiguous |= found != LAYOUT_NONE;
            found = index;
        }
    }
    return *ambiguous ? LAYOUT_NONE : found;
}

size_t layout_tree_enclosing(const layout_tree *tree, const char *path) {
    size_t deepest = 0;
    for (size_t index = 1; index < tree->count; index++) {
        if (layout_path_encloses(tree->orgs[index].folder, path) &&
            strlen(tree->orgs[index].folder) > strlen(tree->orgs[deepest].folder)) {
            deepest = index;
        }
    }
    return deepest;
}

static int same_path_folded(const char *left, const char *right) {
    return layout_path_encloses(left, right) && layout_path_encloses(right, left);
}

static int is_project_folder(const layout_org *org, const char *path) {
    if (!layout_path_strictly_encloses(org->folder, path)) {
        return 0;
    }
    const char *rest = path + strlen(org->folder) + 1;
    return strpbrk(rest, "/\\") == NULL && layout_org_has_project(org, rest);
}

int layout_tree_is_folder(const layout_tree *tree, const char *path) {
    for (size_t index = 0; index < tree->count; index++) {
        if (same_path_folded(tree->orgs[index].folder, path) || is_project_folder(&tree->orgs[index], path)) {
            return 1;
        }
    }
    return 0;
}
