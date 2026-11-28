#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "disk.h"
#include "naula/git.h"
#include "naula/home.h"
#include "naula/memory.h"
#include "sync_internal.h"

#define ACCOUNT_LIMIT 256
#define SCAN_DEPTH 8

typedef struct account_list {
    const naula_home *items[ACCOUNT_LIMIT];
    size_t count;
} account_list;

static int same_text(const char *left, const char *right) {
    if (left == NULL || right == NULL) {
        return left == right;
    }
    for (; *left != '\0' && *right != '\0'; left++, right++) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return 0;
        }
    }
    return *left == *right;
}

static int same_place(const naula_place *left, const naula_place *right) {
    return left->kind == NAULA_HOME_PROVIDER && right->kind == NAULA_HOME_PROVIDER &&
           strcmp(left->provider, right->provider) == 0 && same_text(left->host, right->host) &&
           left->number == right->number;
}

static char *copy_text(const char *text) {
    return text == NULL ? NULL : naula_memory_duplicate_text(text);
}

static naula_status set_primary(naula_home *home, const char *provider, const char *host, const char *account) {
    memset(home, 0, sizeof *home);
    home->size = sizeof *home;
    home->kind = NAULA_HOME_PROVIDER;
    home->provider = copy_text(provider);
    home->host = copy_text(host);
    home->account = copy_text(account);
    if (home->provider == NULL || home->account == NULL || (host != NULL && home->host == NULL)) {
        naula_home_release(home);
        return NAULA_OUT_OF_MEMORY;
    }
    return NAULA_OK;
}

static naula_status copy_place(const naula_place *from, naula_place *to) {
    memset(to, 0, sizeof *to);
    to->size = sizeof *to;
    to->kind = from->kind;
    to->number = from->number;
    to->provider = copy_text(from->provider);
    to->host = copy_text(from->host);
    to->domain = copy_text(from->domain);
    to->url = copy_text(from->url);
    if ((from->provider != NULL && to->provider == NULL) || (from->host != NULL && to->host == NULL) ||
        (from->domain != NULL && to->domain == NULL) || (from->url != NULL && to->url == NULL)) {
        naula_place_release(to);
        return NAULA_OUT_OF_MEMORY;
    }
    return NAULA_OK;
}

static naula_status copy_topics(naula_layout_repo *layout, char *const *topics, size_t count) {
    layout->topics = count == 0 ? NULL : naula_memory_allocate(count * sizeof *layout->topics);
    if (count > 0 && layout->topics == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < count; index++) {
        layout->topics[index] = copy_text(topics[index]);
        if (layout->topics[index] == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        layout->topic_count++;
    }
    return NAULA_OK;
}

static void release_repo(sync_repo *repo) {
    naula_id_release(&repo->layout.id);
    naula_memory_release(repo->layout.name);
    for (size_t index = 0; index < repo->layout.topic_count; index++) {
        naula_memory_release(repo->layout.topics[index]);
    }
    naula_memory_release(repo->layout.topics);
    naula_home_release(&repo->layout.primary);
    naula_memory_release(repo->layout.current_path);
    naula_place_release(&repo->place);
    naula_memory_release(repo->clone_url);
    naula_memory_release(repo->working_branch);
    naula_resolution_release(&repo->resolution);
    memset(repo, 0, sizeof *repo);
}

void sync_model_release(sync_model *model) {
    for (size_t index = 0; index < model->repo_count; index++) {
        release_repo(&model->repos[index]);
    }
    naula_memory_release(model->repos);
    for (size_t index = 0; index < model->org_count; index++) {
        if (model->orgs[index].owns_resolution) {
            naula_resolution_release(&model->orgs[index].resolution);
        }
        naula_id_release(&model->orgs[index].id);
    }
    naula_memory_release(model->orgs);
    for (size_t index = 0; index < model->checkout_count; index++) {
        naula_memory_release(model->checkouts[index].path);
        naula_id_release(&model->checkouts[index].id);
    }
    naula_memory_release(model->checkouts);
    naula_layout_links_release(&model->links);
    naula_memory_release(model->declarations);
    naula_memory_release(model->org_ids);
    naula_memory_release(model->layout_repos);
    memset(model, 0, sizeof *model);
}

static int is_dropped(const sync_request *request, const char *name) {
    for (size_t index = 0; name != NULL && index < request->root_file.dropped_count; index++) {
        if (same_text(request->root_file.dropped[index], name)) {
            return 1;
        }
    }
    return 0;
}

static int holds_org(const sync_model *model, const naula_id *id) {
    for (size_t index = 0; index < model->org_count; index++) {
        if (naula_id_compare(&model->orgs[index].id, id) == 0) {
            return 1;
        }
    }
    return 0;
}

static sync_org *new_org(sync_model *model) {
    sync_org *grown = naula_memory_reallocate(model->orgs, (model->org_count + 1) * sizeof *grown);
    if (grown == NULL) {
        return NULL;
    }
    model->orgs = grown;
    sync_org *org = &model->orgs[model->org_count++];
    memset(org, 0, sizeof *org);
    org->id.size = sizeof org->id;
    org->resolution.size = sizeof org->resolution;
    return org;
}

static naula_status parse_id(const char *text, naula_id *id, naula_refusal *refusal) {
    memset(id, 0, sizeof *id);
    id->size = sizeof *id;
    return naula_id_parse(text, strlen(text), id, refusal);
}

/** Resolves a held org's declaring repo; one whose primary declares no org is refused, since a root holds orgs. */
static naula_status hold_org(cli_context *context, sync_model *model, const naula_reference *reference,
                             naula_refusal *refusal) {
    naula_id id;
    if (reference->id == NULL) {
        naula_refuse(refusal, "org-by-url", "hold the org by its rules repo's id and at",
                     "the root holds an org by the url %s, which naula cannot confirm", reference->url);
        return NAULA_REFUSED;
    }
    naula_status status = parse_id(reference->id, &id, refusal);
    if (status != NAULA_OK || holds_org(model, &id)) {
        naula_id_release(&id);
        return status;
    }
    sync_org *org = new_org(model);
    if (org == NULL) {
        naula_id_release(&id);
        return NAULA_OUT_OF_MEMORY;
    }
    org->id = id;
    org->owns_resolution = 1;
    status = cli_resolve(context, reference, &org->resolution, refusal);
    if (status != NAULA_OK) {
        cli_print_verdicts(&org->resolution);
        return status;
    }
    if (org->resolution.file.org == NULL) {
        naula_refuse(refusal, "not-an-org", "hold the repo that declares the org in its repo.json",
                     "%s declares no org", reference->id);
        return NAULA_REFUSED;
    }
    org->declaration = org->resolution.file.org;
    return NAULA_OK;
}

static int same_account(const naula_home *left, const naula_home *right) {
    return strcmp(left->provider, right->provider) == 0 && same_text(left->host, right->host) &&
           same_text(left->account, right->account);
}

static void add_account(account_list *accounts, const naula_home *home) {
    if (home->kind != NAULA_HOME_PROVIDER) {
        return;
    }
    for (size_t index = 0; index < accounts->count; index++) {
        if (same_account(accounts->items[index], home)) {
            return;
        }
    }
    if (accounts->count < ACCOUNT_LIMIT) {
        accounts->items[accounts->count++] = home;
    }
}

static void add_declared_accounts(account_list *accounts, const naula_org_declaration *declaration) {
    for (size_t index = 0; index < declaration->account_count; index++) {
        add_account(accounts, &declaration->accounts[index]);
    }
    for (size_t suborg = 0; suborg < declaration->suborg_count; suborg++) {
        add_declared_accounts(accounts, &declaration->suborgs[suborg]);
    }
}

static sync_repo *new_repo(sync_model *model) {
    if (model->repo_count == model->repo_capacity) {
        size_t capacity = model->repo_capacity == 0 ? 16 : model->repo_capacity * 2;
        sync_repo *grown = naula_memory_reallocate(model->repos, capacity * sizeof *grown);
        if (grown == NULL) {
            return NULL;
        }
        model->repos = grown;
        model->repo_capacity = capacity;
    }
    sync_repo *repo = &model->repos[model->repo_count++];
    memset(repo, 0, sizeof *repo);
    repo->layout.size = sizeof repo->layout;
    repo->layout.id.size = sizeof repo->layout.id;
    repo->layout.primary.size = sizeof repo->layout.primary;
    repo->resolution.size = sizeof repo->resolution;
    return repo;
}

static void drop_last_repo(sync_model *model) {
    release_repo(&model->repos[--model->repo_count]);
}

static int is_listed_elsewhere(const sync_model *model, const naula_place *place) {
    for (size_t index = 0; index < model->repo_count; index++) {
        const sync_repo *repo = &model->repos[index];
        for (size_t home = 0; repo->resolved && home < repo->resolution.home_count; home++) {
            if (same_place(&repo->resolution.homes[home], place)) {
                return 1;
            }
        }
    }
    return 0;
}

static int cannot_be_heard(naula_status status) {
    return status == NAULA_TIMED_OUT || status == NAULA_UNREACHABLE;
}

/** Names a repo that got no definite answer; the gather goes on, so every such repo is named, then refuses. */
static void record_unread(sync_model *model, const char *code, const char *what, const naula_refusal *refusal) {
    fprintf(stderr, "naula: unread %s: %s: %s: %s\n", what, code, refusal->code, refusal->detail);
    if (model->unread_count++ == 0) {
        naula_refuse(&model->first_unread, "repos-unread",
                     "run again once every repo answers; a repo refused for its own content (its branch, its repo.json, "
                     "its confirmation) needs that fixed first",
                     "%s: %s: %s", what, code, refusal->code);
    }
}

static void record_unread_place(sync_model *model, const char *code, const naula_place *place,
                                const naula_refusal *refusal) {
    char *text = naula_place_text(place);
    record_unread(model, code, text == NULL ? "(home)" : text, refusal);
    naula_memory_release(text);
}

/** The repo.json the listed home holds on its working branch; `*file` stays empty for an unadopted repo. */
static naula_status read_listed_file(cli_context *context, sync_model *model, sync_repo *repo,
                                     naula_repo_file *file, int *adopted, int *skipped, naula_refusal *refusal) {
    uint8_t *bytes = NULL;
    size_t length = 0;
    *adopted = 0;
    naula_status status = cli_read_home_file(context, &repo->place, repo->working_branch, &bytes, &length, refusal);
    if (status == NAULA_NOT_FOUND) {
        return NAULA_OK;
    }
    if (status == NAULA_REFUSED) {
        record_unread_place(model, "repo-file-unreadable", &repo->place, refusal);
        *skipped = 1;
        return NAULA_OK;
    }
    if (status != NAULA_OK) {
        return status;
    }
    naula_refusal parsed = cli_fresh_refusal();
    status = naula_repo_file_parse(bytes, length, file, &parsed);
    naula_memory_release(bytes);
    if (status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    if (status != NAULA_OK) {
        record_unread_place(model, "repo-file-invalid", &repo->place, &parsed);
        naula_repo_file_release(file);
        *skipped = 1;
        return NAULA_OK;
    }
    *adopted = 1;
    return NAULA_OK;
}

static naula_status primary_from_resolution(sync_repo *repo) {
    const naula_place *primary = &repo->resolution.homes[repo->resolution.primary];
    if (primary->kind != NAULA_HOME_PROVIDER || repo->resolution.primary_repository.account == NULL) {
        return NAULA_OK;
    }
    return set_primary(&repo->layout.primary, primary->provider, primary->host,
                       repo->resolution.primary_repository.account);
}

/** Fills the layout from the confirmed primary's file; `member_of` points into the resolution, which the repo owns. */
static naula_status fill_from_resolution(sync_repo *repo) {
    const naula_repo_file *file = &repo->resolution.file;
    repo->layout.adopted = 1;
    repo->layout.archived = repo->resolution.primary_repository.archived;
    repo->layout.member_of = file->member_of;
    repo->layout.name = copy_text(file->name);
    if (repo->layout.name == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    naula_status status = copy_topics(&repo->layout, file->metadata.topics, file->metadata.topic_count);
    return status == NAULA_OK ? primary_from_resolution(repo) : status;
}

static naula_status host_topics(cli_context *context, const naula_provider *provider, sync_repo *repo) {
    uint32_t supported = 0;
    int anonymous = 0;
    if (!naula_provider_offers_writes(provider) || provider->describe(provider->context, &supported, &anonymous) != NAULA_OK ||
        !(supported & NAULA_OP_READ_METADATA)) {
        return NAULA_OK;
    }
    naula_metadata metadata;
    memset(&metadata, 0, sizeof metadata);
    metadata.size = sizeof metadata;
    naula_refusal refusal = cli_fresh_refusal();
    naula_status status = provider->read_metadata(provider->context, repo->place.host, repo->place.number,
                                                  cli_credentials_token(context->credentials, repo->place.provider,
                                                                        repo->place.host),
                                                  &metadata, &refusal);
    if (status == NAULA_OK) {
        status = copy_topics(&repo->layout, metadata.topics, metadata.topic_count);
    } else if (status != NAULA_OUT_OF_MEMORY) {
        status = cannot_be_heard(status) ? status : NAULA_OK;
    }
    naula_metadata_release(&metadata);
    return status;
}

static naula_status fill_from_host(cli_context *context, const naula_provider *provider,
                                   const naula_repository *listed, const naula_repo_file *file, sync_repo *repo) {
    repo->layout.archived = listed->archived;
    repo->layout.adopted = file->name != NULL;
    repo->layout.name = copy_text(file->name != NULL ? file->name : listed->name);
    if (repo->layout.name == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    naula_status status = set_primary(&repo->layout.primary, repo->place.provider, repo->place.host, listed->account);
    if (status == NAULA_OK && file->name != NULL) {
        return copy_topics(&repo->layout, file->metadata.topics, file->metadata.topic_count);
    }
    return status == NAULA_OK ? host_topics(context, provider, repo) : status;
}

static naula_status add_found_org(sync_model *model, const sync_request *request, const sync_repo *repo,
                                  account_list *accounts) {
    const naula_org_declaration *declaration = repo->resolution.file.org;
    if (declaration == NULL || holds_org(model, &repo->layout.id) || is_dropped(request, declaration->name)) {
        return NAULA_OK;
    }
    naula_id id;
    char text[80];
    snprintf(text, sizeof text, "git:%s", repo->layout.id.commit);
    naula_status status = parse_id(text, &id, NULL);
    sync_org *org = status == NAULA_OK ? new_org(model) : NULL;
    if (org == NULL) {
        naula_id_release(&id);
        return status == NAULA_OK ? NAULA_OUT_OF_MEMORY : status;
    }
    org->id = id;
    org->declaration = declaration;
    add_declared_accounts(accounts, declaration);
    return NAULA_OK;
}

/** Derives the id from the listed home's history, then reads its file, taking a stated id as the fork exception. */
static naula_status derive_listed_id(cli_context *context, sync_model *model, sync_repo *repo,
                                     const naula_repository *listed, naula_repo_file *file, int *adopted, int *skipped,
                                     naula_refusal *refusal) {
    naula_status status = cli_history_id(context->history, repo->clone_url, listed->default_branch, NULL,
                                         &repo->layout.id, &repo->working_branch, refusal);
    if (status == NAULA_OK) {
        status = read_listed_file(context, model, repo, file, adopted, skipped, refusal);
    }
    if (status == NAULA_OK && !*skipped && *adopted && file->id != NULL) {
        naula_id_release(&repo->layout.id);
        naula_memory_release(repo->working_branch);
        repo->working_branch = NULL;
        status = cli_history_id(context->history, repo->clone_url, listed->default_branch, file->id,
                                &repo->layout.id, &repo->working_branch, refusal);
    }
    return status;
}

static naula_status confirm_listed(cli_context *context, sync_model *model, sync_repo *repo, naula_refusal *refusal,
                                   int *skipped) {
    char *id_text = cli_format("git:%s", repo->layout.id.commit);
    if (id_text == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    naula_reference reference;
    memset(&reference, 0, sizeof reference);
    reference.size = sizeof reference;
    reference.id = id_text;
    reference.at = &repo->place;
    reference.at_count = 1;
    naula_refusal confirmed = cli_fresh_refusal();
    naula_status status = cli_resolve(context, &reference, &repo->resolution, &confirmed);
    naula_memory_release(id_text);
    repo->resolved = status == NAULA_OK;
    if (status == NAULA_REFUSED || status == NAULA_NOT_FOUND) {
        record_unread_place(model, "unconfirmed", &repo->place, &confirmed);
        cli_print_verdicts(&repo->resolution);
        *skipped = 1;
        return NAULA_OK;
    }
    if (status != NAULA_OK && refusal != NULL) {
        *refusal = confirmed;
    }
    return status;
}

static naula_status gather_listed(cli_context *context, const sync_request *request, sync_model *model,
                                  const naula_provider *provider, const naula_home *account,
                                  const naula_repository *listed, account_list *accounts, naula_refusal *refusal) {
    naula_place place = {sizeof place, NAULA_HOME_PROVIDER, account->provider, account->host, listed->number, NULL,
                         NULL};
    if (is_listed_elsewhere(model, &place)) {
        return NAULA_OK;
    }
    sync_repo *repo = new_repo(model);
    if (repo == NULL || copy_place(&place, &repo->place) != NAULA_OK ||
        ((repo->clone_url = copy_text(listed->clone_url)) == NULL && listed->clone_url != NULL)) {
        if (repo != NULL) {
            drop_last_repo(model);
        }
        return NAULA_OUT_OF_MEMORY;
    }
    naula_repo_file file;
    memset(&file, 0, sizeof file);
    file.size = sizeof file;
    int adopted = 0;
    int skipped = 0;
    naula_refusal derived = cli_fresh_refusal();
    naula_status status = NAULA_REFUSED;
    if (cli_clone_url_is_allowed(listed->clone_url)) {
        status = derive_listed_id(context, model, repo, listed, &file, &adopted, &skipped, &derived);
    } else {
        naula_refuse(&derived, "clone-url-refused", "ask the host for an https clone url",
                     "the host answered no clone url naula hands to git");
    }
    if (status == NAULA_REFUSED) {
        record_unread_place(model, "unreadable", &repo->place, &derived);
        skipped = 1;
        status = NAULA_OK;
    } else if (status != NAULA_OK && refusal != NULL) {
        *refusal = derived;
    }
    if (status == NAULA_OK && !skipped && adopted && !listed->archived) {
        status = confirm_listed(context, model, repo, refusal, &skipped);
    }
    if (status == NAULA_OK && !skipped) {
        status = repo->resolved ? fill_from_resolution(repo) : fill_from_host(context, provider, listed, &file, repo);
    }
    if (status == NAULA_OK && !skipped && repo->resolved) {
        status = add_found_org(model, request, repo, accounts);
    }
    naula_repo_file_release(&file);
    if (status != NAULA_OK || skipped) {
        drop_last_repo(model);
    }
    return status;
}

static naula_status list_account(cli_context *context, const sync_request *request, sync_model *model,
                                 account_list *accounts, size_t which, naula_refusal *refusal) {
    const naula_home *account = accounts->items[which];
    const naula_provider *provider = naula_providers_find(context->providers, account->provider, refusal);
    if (provider == NULL) {
        return NAULA_REFUSED;
    }
    naula_repository *listed = NULL;
    size_t count = 0;
    naula_status status = provider->list_repos(provider->context, account->host, account->account,
                                               cli_credentials_token(context->credentials, account->provider,
                                                                     account->host),
                                               &listed, &count, refusal);
    for (size_t index = 0; status == NAULA_OK && index < count; index++) {
        status = gather_listed(context, request, model, provider, account, &listed[index], accounts, refusal);
    }
    naula_repository_list_release(listed, count);
    return status;
}

int sync_model_holds(const sync_model *model, const naula_id *id) {
    for (size_t index = 0; index < model->repo_count; index++) {
        if (naula_id_compare(&model->repos[index].layout.id, id) == 0) {
            return 1;
        }
    }
    return 0;
}

naula_status sync_gather_extra(cli_context *context, sync_model *model, const naula_reference *reference,
                               naula_refusal *refusal) {
    naula_id id;
    if (reference->id == NULL || parse_id(reference->id, &id, NULL) != NAULA_OK) {
        naula_refuse(refusal, "extra-by-url", "keep the repo by its id and at, as naula clone writes it",
                     "%s is kept by url, which naula cannot confirm", reference->url == NULL ? "(reference)" : reference->url);
        return NAULA_REFUSED;
    }
    if (sync_model_holds(model, &id)) {
        naula_id_release(&id);
        return NAULA_OK;
    }
    sync_repo *repo = new_repo(model);
    if (repo == NULL) {
        naula_id_release(&id);
        return NAULA_OUT_OF_MEMORY;
    }
    repo->layout.id = id;
    naula_refusal resolved = cli_fresh_refusal();
    naula_status status = cli_resolve(context, reference, &repo->resolution, &resolved);
    repo->resolved = status == NAULA_OK;
    if (status == NAULA_REFUSED || status == NAULA_NOT_FOUND) {
        naula_refuse(refusal, "extra-unconfirmed", resolved.fix == NULL ? "run again once its homes confirm it" : resolved.fix,
                     "%s: %s: %s", reference->id, resolved.code, resolved.detail);
        cli_print_verdicts(&repo->resolution);
        drop_last_repo(model);
        return NAULA_REFUSED;
    }
    if (status != NAULA_OK && refusal != NULL) {
        *refusal = resolved;
    }
    const naula_place *primary = repo->resolved ? &repo->resolution.homes[repo->resolution.primary] : NULL;
    if (status == NAULA_OK) {
        status = copy_place(primary, &repo->place);
    }
    if (status == NAULA_OK) {
        repo->clone_url = copy_text(repo->resolution.primary_repository.clone_url);
        status = fill_from_resolution(repo);
    }
    if (status != NAULA_OK) {
        cli_print_verdicts(&repo->resolution);
        drop_last_repo(model);
    }
    return status;
}

static naula_status add_checkout(sync_model *model, const char *path) {
    naula_refusal refusal = cli_fresh_refusal();
    sync_checkout found;
    memset(&found, 0, sizeof found);
    char *stated = NULL;
    char *file_path = cli_join(path, "repo.json");
    uint8_t *bytes = NULL;
    size_t length = 0;
    if (file_path != NULL && cli_read_file(file_path, &bytes, &length, &refusal) == NAULA_OK) {
        naula_repo_file file;
        memset(&file, 0, sizeof file);
        file.size = sizeof file;
        if (naula_repo_file_parse(bytes, length, &file, &refusal) == NAULA_OK && file.id != NULL) {
            stated = copy_text(file.id);
        }
        naula_repo_file_release(&file);
    }
    naula_memory_release(bytes);
    naula_memory_release(file_path);
    naula_status status = naula_git_repository_id(path, "HEAD", stated, &found.id, &refusal);
    naula_memory_release(stated);
    if (status != NAULA_OK) {
        return status == NAULA_OUT_OF_MEMORY ? status : NAULA_OK;
    }
    const char *changes[] = {"status", "--porcelain", NULL};
    const char *unpushed[] = {"rev-list", "--count", "--branches", "--not", "--remotes", NULL};
    naula_run_result result;
    int clean = 0;
    if (cli_git(path, changes, &result, &refusal) == NAULA_OK) {
        clean = result.output_length == 0;
        naula_run_result_release(&result);
    }
    if (clean && cli_git(path, unpushed, &result, &refusal) == NAULA_OK) {
        clean = strncmp(result.output, "0", 1) == 0 && (result.output[1] == '\n' || result.output[1] == '\r' ||
                                                         result.output[1] == '\0');
        naula_run_result_release(&result);
    }
    found.clean_and_pushed = clean;
    found.path = copy_text(path);
    if (found.path == NULL || model->checkout_count == model->checkout_capacity) {
        size_t capacity = model->checkout_capacity == 0 ? 16 : model->checkout_capacity * 2;
        sync_checkout *grown = found.path == NULL ? NULL
                                                  : naula_memory_reallocate(model->checkouts, capacity * sizeof *grown);
        if (grown == NULL) {
            naula_memory_release(found.path);
            naula_id_release(&found.id);
            return NAULA_OUT_OF_MEMORY;
        }
        model->checkouts = grown;
        model->checkout_capacity = capacity;
    }
    model->checkouts[model->checkout_count++] = found;
    return NAULA_OK;
}

/** Every checkout under `folder`, never entering a link, a checkout or naula's own files. */
static naula_status scan_checkouts(sync_model *model, const char *folder, unsigned depth) {
    char **names = NULL;
    size_t count = 0;
    unsigned long error = 0;
    if (disk_names(folder, &names, &count, &error) != NAULA_OK) {
        return NAULA_OK;
    }
    naula_status status = NAULA_OK;
    for (size_t index = 0; status == NAULA_OK && index < count; index++) {
        if (strcmp(names[index], ".naula-journal") == 0 || strcmp(names[index], ".git") == 0) {
            continue;
        }
        char *path = cli_join(folder, names[index]);
        char *git = path == NULL ? NULL : cli_join(path, ".git");
        if (git == NULL) {
            status = NAULA_OUT_OF_MEMORY;
        } else if (cli_is_folder(path) && cli_exists(git)) {
            status = add_checkout(model, path);
        } else if (cli_is_folder(path) && depth < SCAN_DEPTH) {
            status = scan_checkouts(model, path, depth + 1);
        }
        naula_memory_release(git);
        naula_memory_release(path);
    }
    disk_names_release(names, count);
    return status;
}

static naula_status place_checkouts(sync_model *model) {
    for (size_t index = 0; index < model->repo_count; index++) {
        sync_repo *repo = &model->repos[index];
        for (size_t checkout = 0; checkout < model->checkout_count; checkout++) {
            if (naula_id_compare(&model->checkouts[checkout].id, &repo->layout.id) == 0) {
                repo->layout.current_path = copy_text(model->checkouts[checkout].path);
                repo->layout.clean_and_pushed = model->checkouts[checkout].clean_and_pushed;
                if (repo->layout.current_path == NULL) {
                    return NAULA_OUT_OF_MEMORY;
                }
                break;
            }
        }
    }
    return NAULA_OK;
}

static naula_status hold_orgs(cli_context *context, const sync_request *request, sync_model *model,
                              naula_refusal *refusal) {
    naula_status status = hold_org(context, model, &request->root_file.top, refusal);
    const naula_org_declaration *top = status == NAULA_OK ? model->orgs[0].declaration : NULL;
    for (size_t index = 0; status == NAULA_OK && index < top->hold_count; index++) {
        status = hold_org(context, model, &top->hold[index], refusal);
    }
    for (size_t index = 0; status == NAULA_OK && index < request->root_file.added_count; index++) {
        status = hold_org(context, model, &request->root_file.added[index], refusal);
    }
    return status;
}

static naula_status gather_accounts(cli_context *context, const sync_request *request, sync_model *model,
                                    naula_refusal *refusal) {
    account_list *accounts = naula_memory_allocate(sizeof *accounts);
    if (accounts == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    accounts->count = 0;
    for (size_t index = 0; index < model->org_count; index++) {
        if (index == 0 || !is_dropped(request, model->orgs[index].declaration->name)) {
            add_declared_accounts(accounts, model->orgs[index].declaration);
        }
    }
    naula_status status = NAULA_OK;
    for (size_t index = 0; status == NAULA_OK && index < accounts->count; index++) {
        status = list_account(context, request, model, accounts, index, refusal);
    }
    naula_memory_release(accounts);
    return status;
}

naula_status sync_gather(cli_context *context, const sync_request *request, sync_model *model,
                         naula_refusal *refusal) {
    memset(model, 0, sizeof *model);
    model->links.size = sizeof model->links;
    naula_status status = hold_orgs(context, request, model, refusal);
    if (status == NAULA_OK) {
        status = gather_accounts(context, request, model, refusal);
    }
    for (size_t index = 0; status == NAULA_OK && index < request->root_file.extra_count; index++) {
        const naula_reference *extra = &request->root_file.extra[index];
        naula_refusal kept = cli_fresh_refusal();
        status = sync_gather_extra(context, model, extra, &kept);
        if (status == NAULA_REFUSED) {
            record_unread(model, "extra", extra->id == NULL ? "(kept repo)" : extra->id, &kept);
            status = NAULA_OK;
        } else if (status != NAULA_OK && refusal != NULL) {
            *refusal = kept;
        }
    }
    if (status == NAULA_OK && model->unread_count > 0) {
        if (refusal != NULL) {
            naula_refuse(refusal, "repos-unread", model->first_unread.fix,
                         "%zu repos of this root got no definite answer, so nothing was changed; the first: %s",
                         model->unread_count, model->first_unread.detail);
        }
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK && cli_is_folder(request->root)) {
        status = scan_checkouts(model, request->root, 0);
        if (status == NAULA_OK) {
            status = naula_layout_links_scan(request->root, &model->links, refusal);
        }
    }
    return status == NAULA_OK ? place_checkouts(model) : status;
}

naula_status sync_input(const sync_request *request, sync_model *model, naula_layout_input *input) {
    memset(input, 0, sizeof *input);
    input->size = sizeof *input;
    size_t others = model->org_count - 1;
    naula_memory_release(model->declarations);
    naula_memory_release(model->org_ids);
    naula_memory_release(model->layout_repos);
    model->declarations = naula_memory_allocate((others + 1) * sizeof *model->declarations);
    model->org_ids = naula_memory_allocate((others + 1) * sizeof *model->org_ids);
    model->layout_repos = naula_memory_allocate((model->repo_count + 1) * sizeof *model->layout_repos);
    if (model->declarations == NULL || model->org_ids == NULL || model->layout_repos == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < others; index++) {
        model->declarations[index] = *model->orgs[index + 1].declaration;
        model->org_ids[index] = model->orgs[index + 1].id;
    }
    for (size_t index = 0; index < model->repo_count; index++) {
        model->layout_repos[index] = model->repos[index].layout;
    }
    input->root = request->root;
    input->top = model->orgs[0].declaration;
    input->top_repository = &model->orgs[0].id;
    input->orgs = model->declarations;
    input->org_repositories = model->org_ids;
    input->org_count = others;
    input->dropped = (const char *const *)request->root_file.dropped;
    input->dropped_count = request->root_file.dropped_count;
    input->repos = model->layout_repos;
    input->repo_count = model->repo_count;
    input->extra = request->root_file.extra;
    input->extra_count = request->root_file.extra_count;
    input->extra_orgs = (const char *const *)request->root_file.extra_orgs;
    input->existing_links = model->links.links;
    input->existing_link_count = model->links.count;
    return NAULA_OK;
}
