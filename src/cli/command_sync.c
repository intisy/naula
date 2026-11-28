#include <string.h>

#include "naula/memory.h"
#include "sync_internal.h"

/** Opens the context and reads `.naula.json` of the root the arguments name, for every command acting on a root. */
naula_status sync_open_root(const cli_arguments *arguments, cli_context *context, sync_request *request,
                            naula_refusal *refusal) {
    memset(request, 0, sizeof *request);
    request->root_file.size = sizeof request->root_file;
    naula_status status = sync_find_root(arguments, &request->root, refusal);
    if (status == NAULA_OK) {
        status = naula_root_file_read(request->root, &request->root_file, refusal);
    }
    if (status == NAULA_OK) {
        status = cli_context_open(arguments, context, refusal);
    }
    if (status != NAULA_OK) {
        sync_request_release(request);
    }
    return status;
}

int cli_command_sync(int count, char **values) {
    cli_arguments arguments;
    if (!cli_parse_arguments("sync", count, values, CLI_ALLOW_ROOT | CLI_ALLOW_ACCEPT, 0, 1, &arguments)) {
        return CLI_USAGE;
    }
    cli_context context;
    sync_request request;
    naula_refusal refusal = cli_fresh_refusal();
    naula_status status = sync_open_root(&arguments, &context, &request, &refusal);
    if (status != NAULA_OK) {
        return cli_refused(status, &refusal);
    }
    request.org_filter = arguments.positional_count == 1 ? arguments.positional[0] : NULL;
    status = sync_run(&context, &request, &refusal);
    sync_request_release(&request);
    cli_context_close(&context);
    return status == NAULA_OK ? CLI_DONE : cli_refused(status, &refusal);
}
