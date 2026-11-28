#include <stdio.h>
#include <string.h>

#include "naula/memory.h"
#include "naula/reconcile.h"
#include "sync_internal.h"

static void print_line(void *context, const char *line) {
    (void)context;
    printf("%s\n", line);
}

naula_status cli_reconcile(cli_context *context, const char *repository, naula_reconcile_report *report,
                           naula_refusal *refusal) {
    naula_reconcile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.cache_directory = context->cache_directory;
    options.url_is_allowed = cli_clone_url_is_allowed;
    options.say = print_line;
    memset(report, 0, sizeof *report);
    report->size = sizeof *report;
    return naula_reconcile(context->session, repository, &options, report, refusal);
}

naula_status sync_push_half(cli_context *context, const sync_request *request, int *incomplete) {
    *incomplete = 0;
    sync_model model;
    memset(&model, 0, sizeof model);
    naula_status status = sync_scan_checkouts(&model, request->root);
    for (size_t index = 0; status == NAULA_OK && index < model.checkout_count; index++) {
        const char *path = model.checkouts[index].path;
        naula_reconcile_report report;
        naula_refusal refusal = cli_fresh_refusal();
        naula_status reconciled = cli_reconcile(context, path, &report, &refusal);
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
    return status;
}
