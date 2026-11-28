#include <string.h>

#include "cli.h"
#include "naula/git.h"
#include "naula/json.h"

typedef struct cli_command {
    const char *name;
    int (*run)(int count, char **values);
} cli_command;

/** Every command reads some history, so each starts by refusing a git too old for what naula asks of it. */
static const cli_command commands[] = {
    {"init", cli_command_init},       {"hold", cli_command_hold},   {"release", cli_command_release},
    {"sync", cli_command_sync},       {"clone", cli_command_clone}, {"resolve", cli_command_resolve},
    {"check", cli_command_check},     {"siblings", cli_command_siblings},
};

int main(int count, char **values) {
    naula_json_use_allocator();
    if (count < 2) {
        cli_usage();
        return CLI_USAGE;
    }
    if (strcmp(values[1], "--help") == 0 || strcmp(values[1], "help") == 0) {
        cli_usage();
        return CLI_DONE;
    }
    for (size_t index = 0; index < sizeof commands / sizeof commands[0]; index++) {
        if (strcmp(values[1], commands[index].name) == 0) {
            naula_refusal refusal = cli_fresh_refusal();
            naula_status status = naula_git_check_version(&refusal);
            return status == NAULA_OK ? commands[index].run(count - 2, values + 2) : cli_refused(status, &refusal);
        }
    }
    return cli_usage_error(values[1], "is not a naula command");
}
