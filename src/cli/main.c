#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

#include "cli.h"
#include "naula/git.h"
#include "naula/json.h"
#include "naula/memory.h"

typedef struct cli_command {
    const char *name;
    int (*run)(int count, char **values);
} cli_command;

/** Every command reads some history, so each starts by refusing a git too old for what naula asks of it. */
static const cli_command commands[] = {
    {"init", cli_command_init},       {"hold", cli_command_hold},   {"release", cli_command_release},
    {"sync", cli_command_sync},       {"clone", cli_command_clone}, {"resolve", cli_command_resolve},
    {"check", cli_command_check},     {"siblings", cli_command_siblings},
    {"new", cli_command_new},         {"adopt", cli_command_adopt}, {"add", cli_command_add},
    {"retire", cli_command_retire},
};

#if defined(_WIN32)
static void release_arguments(char **values, int count) {
    for (int index = 0; values != NULL && index < count; index++) {
        naula_memory_release(values[index]);
    }
    naula_memory_release(values);
}

/**
 * The command line as UTF-8, read from its UTF-16 form, since the `char` arguments the C runtime hands main are in the
 * ANSI code page and turn a folder name outside it into question marks.
 */
static char **utf8_arguments(int *count) {
    int wide_count = 0;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &wide_count);
    char **values = wide == NULL ? NULL : naula_memory_allocate(((size_t)wide_count + 1) * sizeof *values);
    int made = 0;
    while (values != NULL && made < wide_count) {
        int needed = WideCharToMultiByte(CP_UTF8, 0, wide[made], -1, NULL, 0, NULL, NULL);
        values[made] = needed <= 0 ? NULL : naula_memory_allocate((size_t)needed);
        if (values[made] == NULL ||
            WideCharToMultiByte(CP_UTF8, 0, wide[made], -1, values[made], needed, NULL, NULL) != needed) {
            naula_memory_release(values[made]);
            release_arguments(values, made);
            values = NULL;
            break;
        }
        made++;
    }
    if (values != NULL) {
        values[made] = NULL;
    }
    if (wide != NULL) {
        LocalFree(wide);
    }
    *count = made;
    return values;
}
#endif

static int run_command(int count, char **values) {
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

int main(int count, char **values) {
    naula_json_use_allocator();
#if defined(_WIN32)
    (void)values;
    char **utf8 = utf8_arguments(&count);
    if (utf8 == NULL) {
        fputs("naula: command-line-unreadable: the command line could not be read as UTF-8\n", stderr);
        return CLI_REFUSED;
    }
    int code = run_command(count, utf8);
    release_arguments(utf8, count);
    return code;
#else
    return run_command(count, values);
#endif
}
