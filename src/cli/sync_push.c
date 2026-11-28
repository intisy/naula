#include <stdio.h>
#include <string.h>

#include "naula/memory.h"
#include "naula/reconcile.h"
#include "sync_internal.h"

#define HELD_ORG_LIMIT 64
#define HELD_ACCOUNT_LIMIT 128

/** The accounts the orgs of the root hold, pointing into the resolutions of their declaring repos. */
typedef struct held_accounts {
    naula_resolution resolutions[HELD_ORG_LIMIT];
    size_t resolution_count;
    naula_home homes[HELD_ACCOUNT_LIMIT];
    size_t count;
} held_accounts;

static void print_line(void *context, const char *line) {
    (void)context;
    printf("%s\n", line);
}

static int same_text(const char *left, const char *right) {
    if (left == NULL || right == NULL) {
        return left == right;
    }
    for (; *left != '\0' && *right != '\0'; left++, right++) {
        char one = (*left >= 'A' && *left <= 'Z') ? (char)(*left + 32) : *left;
        char other = (*right >= 'A' && *right <= 'Z') ? (char)(*right + 32) : *right;
        if (one != other) {
            return 0;
        }
    }
    return *left == *right;
}

static int same_account(const naula_home *left, const naula_home *right) {
    return left->kind == NAULA_HOME_PROVIDER && right->kind == NAULA_HOME_PROVIDER && left->provider != NULL &&
           right->provider != NULL && strcmp(left->provider, right->provider) == 0 && same_text(left->host, right->host) &&
           same_text(left->account, right->account);
}

int cli_home_is_allowed(const cli_context *context, const naula_home *home, const naula_home *held, size_t held_count) {
    for (size_t index = 0; index < held_count; index++) {
        if (same_account(&held[index], home)) {
            return 1;
        }
    }
    char text[300];
    snprintf(text, sizeof text, "%s%s%s:%s", home->provider, home->host == NULL ? "" : "@",
             home->host == NULL ? "" : home->host, home->account);
    for (size_t index = 0; index < context->arguments->accept_home_count; index++) {
        if (same_text(context->arguments->accept_home[index], text)) {
            return 1;
        }
    }
    return 0;
}

naula_status cli_reconcile(cli_context *context, const char *repository, const naula_home *held, size_t held_count,
                           naula_reconcile_report *report, naula_refusal *refusal) {
    naula_reconcile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.cache_directory = context->cache_directory;
    options.url_is_allowed = cli_clone_url_is_allowed;
    options.say = print_line;
    options.held_accounts = held;
    options.held_account_count = held_count;
    options.accepted_homes = context->arguments->accept_home;
    options.accepted_home_count = context->arguments->accept_home_count;
    memset(report, 0, sizeof *report);
    report->size = sizeof *report;
    return naula_reconcile(context->session, repository, &options, report, refusal);
}

static int is_dropped(const sync_request *request, const char *name) {
    for (size_t index = 0; name != NULL && index < request->root_file.dropped_count; index++) {
        if (same_text(request->root_file.dropped[index], name)) {
            return 1;
        }
    }
    return 0;
}

/**
 * An org holds an account only where its declaring repo's primary lives (spisor rev 270); an account it merely lists
 * is left out, so a home there is written to only once a run accepts it.
 */
static void add_owned_accounts(held_accounts *held, const naula_resolution *resolution) {
    const naula_org_declaration *org = resolution->file.org;
    if (org == NULL || resolution->home_count == 0 || resolution->primary_repository.account == NULL) {
        return;
    }
    const naula_place *primary = &resolution->homes[resolution->primary];
    naula_home owned;
    memset(&owned, 0, sizeof owned);
    owned.size = sizeof owned;
    owned.kind = NAULA_HOME_PROVIDER;
    owned.provider = primary->provider;
    owned.host = primary->host;
    owned.account = resolution->primary_repository.account;
    for (size_t index = 0; index < org->account_count && held->count < HELD_ACCOUNT_LIMIT; index++) {
        if (same_account(&org->accounts[index], &owned)) {
            held->homes[held->count++] = owned;
            return;
        }
    }
}

/** Resolves one held org's declaring repo; one that does not resolve holds nothing here and refuses nothing. */
static naula_resolution *hold(cli_context *context, held_accounts *held, const naula_reference *reference) {
    if (held->resolution_count == HELD_ORG_LIMIT) {
        return NULL;
    }
    naula_resolution *resolution = &held->resolutions[held->resolution_count];
    naula_refusal ignored = cli_fresh_refusal();
    if (cli_resolve(context, reference, resolution, &ignored) != NAULA_OK) {
        naula_resolution_release(resolution);
        return NULL;
    }
    held->resolution_count++;
    return resolution;
}

static void gather_held(cli_context *context, const sync_request *request, held_accounts *held) {
    naula_resolution *top = hold(context, held, &request->root_file.top);
    if (top != NULL) {
        add_owned_accounts(held, top);
    }
    const naula_org_declaration *declaration = top == NULL ? NULL : top->file.org;
    for (size_t index = 0; declaration != NULL && index < declaration->hold_count; index++) {
        naula_resolution *org = hold(context, held, &declaration->hold[index]);
        if (org != NULL && !(org->file.org != NULL && is_dropped(request, org->file.org->name))) {
            add_owned_accounts(held, org);
        }
    }
    for (size_t index = 0; index < request->root_file.added_count; index++) {
        naula_resolution *org = hold(context, held, &request->root_file.added[index]);
        if (org != NULL) {
            add_owned_accounts(held, org);
        }
    }
}

static void release_held(held_accounts *held) {
    for (size_t index = 0; index < held->resolution_count; index++) {
        naula_resolution_release(&held->resolutions[index]);
    }
}

static int is_separator(char character) {
    return character == '/' || character == '\\';
}

/**
 * Whether the checkout at `<org folder>/repos/<hex>` lies in the folder of the org named `filter`: the root's own
 * `repos` is the top org's.
 */
static int is_checkout_of_org(const char *root, const char *path, const char *top_name, const char *filter) {
    size_t length = strlen(path);
    for (int segment = 0; segment < 2 && length > 0; segment++) {
        while (length > 0 && !is_separator(path[length - 1])) {
            length--;
        }
        while (length > 0 && is_separator(path[length - 1])) {
            length--;
        }
    }
    size_t root_length = strlen(root);
    while (root_length > 0 && is_separator(root[root_length - 1])) {
        root_length--;
    }
    if (length == root_length && strncmp(path, root, length) == 0) {
        return top_name != NULL && same_text(top_name, filter);
    }
    size_t start = length;
    while (start > 0 && !is_separator(path[start - 1])) {
        start--;
    }
    return length - start == strlen(filter) && strncmp(path + start, filter, length - start) == 0;
}

naula_status sync_push_half(cli_context *context, const sync_request *request, int *incomplete) {
    *incomplete = 0;
    held_accounts *held = naula_memory_allocate(sizeof *held);
    if (held == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    memset(held, 0, sizeof *held);
    gather_held(context, request, held);
    const char *top_name = held->resolution_count > 0 && held->resolutions[0].file.org != NULL
                               ? held->resolutions[0].file.org->name
                               : NULL;
    sync_model model;
    memset(&model, 0, sizeof model);
    naula_status status = sync_scan_checkouts(&model, request->root);
    for (size_t index = 0; status == NAULA_OK && index < model.checkout_count; index++) {
        const char *path = model.checkouts[index].path;
        if (request->org_filter != NULL && !is_checkout_of_org(request->root, path, top_name, request->org_filter)) {
            continue;
        }
        naula_reconcile_report report;
        naula_refusal refusal = cli_fresh_refusal();
        naula_status reconciled = cli_reconcile(context, path, held->homes, held->count, &report, &refusal);
        if (reconciled == NAULA_OUT_OF_MEMORY) {
            status = reconciled;
        } else if (reconciled != NAULA_OK && strcmp(refusal.code, "unadopted") != 0) {
            *incomplete = 1;
            fprintf(stderr, "naula: %s: %s: %s\n", path, refusal.code, refusal.detail);
            if (refusal.fix != NULL) {
                fprintf(stderr, "  fix: %s\n", refusal.fix);
            }
        }
    }
    sync_model_release(&model);
    release_held(held);
    naula_memory_release(held);
    return status;
}
