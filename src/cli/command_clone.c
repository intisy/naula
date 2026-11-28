#include <string.h>

#include "naula/memory.h"
#include "sync_internal.h"

/**
 * Clones one repo into the root by laying the root out with it: a repo of an account the root holds is already in the
 * layout, and any other is kept in the top org's `extra`, so the clone lands in `repos/<12 hex>` with its link and
 * stays there on every later sync.
 */
int cli_command_clone(int count, char **values) {
    cli_arguments arguments;
    if (!cli_parse_arguments("clone", count, values, CLI_ALLOW_AT | CLI_ALLOW_ROOT | CLI_ALLOW_ACCEPT, 1, 1,
                             &arguments)) {
        return CLI_USAGE;
    }
    cli_context context;
    sync_request request;
    naula_refusal refusal = cli_fresh_refusal();
    naula_status status = sync_open_root(&arguments, &context, &request, &refusal);
    if (status != NAULA_OK) {
        return cli_refused(status, &refusal);
    }
    naula_reference reference;
    status = cli_reference_from_arguments(&context, arguments.positional[0], arguments.at, arguments.at_count,
                                          &reference, &refusal);
    if (status == NAULA_OK) {
        request.clone = &reference;
        status = sync_run(&context, &request, &refusal);
        request.clone = NULL;
        cli_reference_release(&reference);
    }
    sync_request_release(&request);
    cli_context_close(&context);
    return status == NAULA_OK ? CLI_DONE : cli_refused(status, &refusal);
}
