#include <string.h>

#include "naula/memory.h"
#include "sync_internal.h"

/** Refuses a folder that is already a root; the folder itself is made only once the first plan is accepted. */
static naula_status refuse_existing_root(const char *root, naula_refusal *refusal) {
    char *file = cli_join(root, ".naula.json");
    if (file == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    int exists = cli_exists(file);
    naula_memory_release(file);
    if (exists) {
        naula_refuse(refusal, "root-exists", "run naula sync there instead", "%s is already a root", root);
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

int cli_command_init(int count, char **values) {
    cli_arguments arguments;
    if (!cli_parse_arguments("init", count, values, CLI_ALLOW_AT | CLI_ALLOW_ACCEPT, 2, 2, &arguments)) {
        return CLI_USAGE;
    }
    sync_request request;
    memset(&request, 0, sizeof request);
    request.root_file.size = sizeof request.root_file;
    naula_refusal refusal = cli_fresh_refusal();
    request.root = cli_absolute_path(arguments.positional[0]);
    naula_status status = request.root == NULL ? NAULA_OUT_OF_MEMORY : refuse_existing_root(request.root, &refusal);
    cli_context context;
    memset(&context, 0, sizeof context);
    if (status == NAULA_OK) {
        status = cli_context_open(&arguments, &context, &refusal);
    }
    if (status == NAULA_OK) {
        status = cli_reference_from_arguments(&context, arguments.positional[1], arguments.at, arguments.at_count,
                                              &request.root_file.top, &refusal);
    }
    if (status == NAULA_OK) {
        request.write_root_file = 1;
        status = sync_run(&context, &request, &refusal);
    }
    sync_request_release(&request);
    cli_context_close(&context);
    return status == NAULA_OK ? CLI_DONE : cli_refused(status, &refusal);
}
