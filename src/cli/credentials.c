#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "naula/git.h"
#include "naula/memory.h"

#define TOKEN_LIMIT 16

typedef struct cli_token {
    char *host;
    char *secret;
} cli_token;

struct cli_credentials {
    cli_token tokens[TOKEN_LIMIT];
    size_t count;
};

cli_credentials *cli_credentials_open(void) {
    cli_credentials *credentials = naula_memory_allocate(sizeof *credentials);
    if (credentials != NULL) {
        memset(credentials, 0, sizeof *credentials);
    }
    return credentials;
}

static void wipe(char *secret) {
    if (secret != NULL) {
        naula_overwrite(secret, strlen(secret));
        naula_memory_release(secret);
    }
}

void cli_credentials_close(cli_credentials *credentials) {
    if (credentials == NULL) {
        return;
    }
    for (size_t index = 0; index < credentials->count; index++) {
        naula_memory_release(credentials->tokens[index].host);
        wipe(credentials->tokens[index].secret);
    }
    naula_memory_release(credentials);
}

/** The host git keeps a provider's credentials under, NULL for a provider naula asks git nothing for. */
static const char *credential_host(const char *provider, const char *host) {
    if (strcmp(provider, "github") == 0) {
        return host == NULL ? "github.com" : host;
    }
    if (strcmp(provider, "gitea") == 0 || strcmp(provider, "forgejo") == 0) {
        return host;
    }
    return NULL;
}

static int is_plain_host(const char *host) {
    return host[0] != '\0' && strlen(host) < 256 && strpbrk(host, "\r\n=/") == NULL;
}

/** The `password=` line of git's answer, copied; NULL when there is none. */
static char *password_of(const char *answer) {
    for (const char *line = answer; *line != '\0';) {
        size_t length = strcspn(line, "\r\n");
        if (length > 9 && strncmp(line, "password=", 9) == 0) {
            char *secret = naula_memory_allocate(length - 8);
            if (secret != NULL) {
                memcpy(secret, line + 9, length - 9);
                secret[length - 9] = '\0';
            }
            return secret;
        }
        line += length;
        line += strspn(line, "\r\n");
    }
    return NULL;
}

/** @implNote naula_git_environment keeps git from prompting, so a host with no stored credential answers nothing. */
static char *ask_git(const char *host) {
    char input[300];
    int written = snprintf(input, sizeof input, "protocol=https\nhost=%s\n\n", host);
    const char *arguments[] = {"credential", "fill", NULL};
    naula_run_result result;
    naula_refusal refusal = cli_fresh_refusal();
    if (naula_git_run(NULL, arguments, input, (size_t)written, &result, &refusal) != NAULA_OK) {
        return NULL;
    }
    char *secret = result.exit_code == 0 ? password_of(result.output) : NULL;
    naula_overwrite(result.output, result.output_length);
    naula_run_result_release(&result);
    return secret;
}

const char *cli_credentials_token(cli_credentials *credentials, const char *provider, const char *host) {
    const char *asked = provider == NULL ? NULL : credential_host(provider, host);
    if (credentials == NULL || asked == NULL || !is_plain_host(asked)) {
        return NULL;
    }
    for (size_t index = 0; index < credentials->count; index++) {
        if (strcmp(credentials->tokens[index].host, asked) == 0) {
            return credentials->tokens[index].secret;
        }
    }
    if (credentials->count >= TOKEN_LIMIT) {
        return NULL;
    }
    char *copy = naula_memory_duplicate_text(asked);
    if (copy == NULL) {
        return NULL;
    }
    cli_token *token = &credentials->tokens[credentials->count++];
    token->host = copy;
    token->secret = ask_git(asked);
    return token->secret;
}
