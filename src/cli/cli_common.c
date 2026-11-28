#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "naula/git.h"
#include "naula/memory.h"

static const char usage_text[] =
    "usage: naula <command> [options]\n"
    "  init <folder> <top org home>         make a root and lay its orgs out\n"
    "  hold <org home>                      hold another org in this root, then sync\n"
    "  release <org>                        stop holding an org, then sync\n"
    "  sync [<org>]                         clone what is missing, relink, regenerate CLAUDE.md\n"
    "  clone <home | id --at <place>...>    clone a repo into its repos/<12 hex> folder\n"
    "  resolve <id> --at <place>...         print the homes that speak for an id\n"
    "  check [--repo <dir>] [--previous <commit>]   the gate a repo passes before it is pushed\n"
    "  siblings [--repo <dir>]              clone every dependency at its commit into siblings/\n"
    "A home is a place: github:<number>, gitea@<host>:<number>, web:<domain> or a git url.\n"
    "Options: --root <folder>, --json (resolve, check), --offline, --accept-primary <place>,\n"
    "  --accept-number <number>. Exit 0 done, 1 refused, 2 usage.\n";

void cli_usage(void) {
    fputs(usage_text, stdout);
}

int cli_usage_error(const char *command, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    fprintf(stderr, "naula %s: ", command);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    fputs(usage_text, stderr);
    return CLI_USAGE;
}

naula_refusal cli_fresh_refusal(void) {
    naula_refusal refusal;
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    return refusal;
}

int cli_refused(naula_status status, const naula_refusal *refusal) {
    if (status == NAULA_OUT_OF_MEMORY) {
        fputs("naula: out-of-memory: naula ran out of memory\n  fix: close some programs and try again\n", stderr);
        return CLI_REFUSED;
    }
    const char *code = refusal->code[0] == '\0' ? "refused" : refusal->code;
    fprintf(stderr, "naula: %s: %s\n", code, refusal->detail);
    if (refusal->fix != NULL) {
        fprintf(stderr, "  fix: %s\n", refusal->fix);
    }
    return CLI_REFUSED;
}

typedef struct option_spec {
    const char *name;
    unsigned allowed;
} option_spec;

static const option_spec value_options[] = {
    {"--at", CLI_ALLOW_AT},
    {"--root", CLI_ALLOW_ROOT},
    {"--repo", CLI_ALLOW_REPO},
    {"--previous", CLI_ALLOW_PREVIOUS},
    {"--accept-primary", CLI_ALLOW_ACCEPT},
    {"--accept-number", CLI_ALLOW_ACCEPT},
};

static int push(const char **list, size_t *count, const char *value) {
    if (*count >= CLI_LIST_LIMIT) {
        return 0;
    }
    list[(*count)++] = value;
    return 1;
}

static int read_number(const char *text, uint64_t *number) {
    if (text[0] < '1' || text[0] > '9' || strlen(text) > 20) {
        return 0;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    *number = (uint64_t)value;
    return errno == 0 && end != NULL && *end == '\0';
}

static int take_value(const char *command, const char *name, const char *value, cli_arguments *arguments) {
    int stored = 1;
    if (strcmp(name, "--at") == 0) {
        stored = push(arguments->at, &arguments->at_count, value);
    } else if (strcmp(name, "--root") == 0) {
        arguments->root = value;
    } else if (strcmp(name, "--repo") == 0) {
        arguments->repo = value;
    } else if (strcmp(name, "--previous") == 0) {
        arguments->previous = value;
    } else if (strcmp(name, "--accept-primary") == 0) {
        stored = push(arguments->accept_primary, &arguments->accept_primary_count, value);
    } else {
        uint64_t number = 0;
        if (!read_number(value, &number)) {
            return cli_usage_error(command, "--accept-number takes a repository number, not %s", value) == 0;
        }
        stored = arguments->accept_number_count < CLI_LIST_LIMIT;
        if (stored) {
            arguments->accept_number[arguments->accept_number_count++] = number;
        }
    }
    return stored ? 1 : cli_usage_error(command, "%s is given more than %d times", name, CLI_LIST_LIMIT) == 0;
}

static const option_spec *value_option(const char *word) {
    for (size_t index = 0; index < sizeof value_options / sizeof value_options[0]; index++) {
        if (strcmp(word, value_options[index].name) == 0) {
            return &value_options[index];
        }
    }
    return NULL;
}

static int take_word(const char *command, int count, char **values, int *index, unsigned allowed,
                     cli_arguments *arguments) {
    const char *word = values[*index];
    if (strcmp(word, "--json") == 0 && (allowed & CLI_ALLOW_JSON)) {
        arguments->json = 1;
        return 1;
    }
    if (strcmp(word, "--offline") == 0 && (allowed & CLI_ALLOW_OFFLINE)) {
        arguments->offline = 1;
        return 1;
    }
    const option_spec *option = value_option(word);
    if (option != NULL && (allowed & option->allowed)) {
        if (*index + 1 >= count) {
            return cli_usage_error(command, "%s needs a value", word) == 0;
        }
        *index += 1;
        return take_value(command, word, values[*index], arguments);
    }
    if (word[0] == '-') {
        return cli_usage_error(command, "does not take %s", word) == 0;
    }
    return push(arguments->positional, &arguments->positional_count, word) ||
           cli_usage_error(command, "takes too many arguments") == 0;
}

int cli_parse_arguments(const char *command, int count, char **values, unsigned allowed, size_t minimum,
                        size_t maximum, cli_arguments *arguments) {
    memset(arguments, 0, sizeof *arguments);
    for (int index = 0; index < count; index++) {
        if (!take_word(command, count, values, &index, allowed, arguments)) {
            return 0;
        }
    }
    if (arguments->positional_count < minimum || arguments->positional_count > maximum) {
        cli_usage_error(command, "takes %zu to %zu arguments, not %zu", minimum, maximum, arguments->positional_count);
        return 0;
    }
    return 1;
}

char *cli_format(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    va_list measured;
    va_copy(measured, arguments);
    int needed = vsnprintf(NULL, 0, format, measured);
    va_end(measured);
    char *text = needed < 0 ? NULL : naula_memory_allocate((size_t)needed + 1);
    if (text != NULL) {
        vsnprintf(text, (size_t)needed + 1, format, arguments);
    }
    va_end(arguments);
    return text;
}

char *cli_join(const char *folder, const char *name) {
    size_t length = strlen(folder);
    int has_separator = length > 0 && (folder[length - 1] == '/' || folder[length - 1] == '\\');
    return cli_format(has_separator ? "%s%s" : "%s/%s", folder, name);
}

naula_status cli_git(const char *repository, const char *const *arguments, naula_run_result *result,
                     naula_refusal *refusal) {
    naula_status status = naula_git_run(repository, arguments, NULL, 0, result, refusal);
    if (status == NAULA_OK && result->exit_code != 0) {
        size_t length = strlen(result->error);
        while (length > 0 && (result->error[length - 1] == '\n' || result->error[length - 1] == '\r')) {
            length--;
        }
        naula_refuse(refusal, "git-failed", "run the same git command by hand to see why", "git %s exited %d: %.*s",
                     arguments[0], result->exit_code, (int)(length > 700 ? 700 : length), result->error);
        naula_run_result_release(result);
        return NAULA_REFUSED;
    }
    return status;
}

int cli_clone_url_is_allowed(const char *url) {
    if (url == NULL) {
        return 0;
    }
#if defined(NAULA_TEST_HOOKS)
    if (strncmp(url, "file://", 7) == 0) {
        return 1;
    }
#endif
    return strncmp(url, "https://", 8) == 0 && url[8] != '\0';
}
