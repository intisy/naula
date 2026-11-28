#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "naula/home.h"
#include "naula/memory.h"

/** The cache place: `NAULA_CACHE`, else the system's per-user cache folder with `naula` below it. */
static char *cache_directory(void) {
    char *named = cli_environment("NAULA_CACHE");
    if (named != NULL) {
        char *absolute = cli_absolute_path(named);
        naula_memory_release(named);
        return absolute;
    }
#if defined(_WIN32)
    char *base = cli_environment("LOCALAPPDATA");
    char *folder = base == NULL ? NULL : cli_join(base, "naula/cache");
#else
    char *base = cli_environment("XDG_CACHE_HOME");
    char *folder = base == NULL ? NULL : cli_join(base, "naula");
    if (base == NULL) {
        base = cli_environment("HOME");
        folder = base == NULL ? NULL : cli_join(base, ".cache/naula");
    }
#endif
    naula_memory_release(base);
    return folder;
}

static void ask(cli_context *context, char *question) {
    if (question != NULL && context->question_count < CLI_LIST_LIMIT) {
        context->questions[context->question_count++] = question;
    } else {
        naula_memory_release(question);
    }
}

static const char *session_token(void *opaque, const char *provider, const char *host) {
    cli_context *context = opaque;
    return cli_credentials_token(context->credentials, provider, host);
}

static int accept_primary(void *opaque, const char *id, const naula_place *was, const naula_place *becomes) {
    cli_context *context = opaque;
    char *becomes_text = naula_place_text(becomes);
    char *was_text = was == NULL ? NULL : naula_place_text(was);
    int accepted = 0;
    for (size_t index = 0; becomes_text != NULL && index < context->arguments->accept_primary_count; index++) {
        accepted = accepted || strcmp(context->arguments->accept_primary[index], becomes_text) == 0;
    }
    if (!accepted && becomes_text != NULL) {
        ask(context, cli_format("primary-changed: %s had the primary %s and now answers %s; once you know the move is "
                                "genuine, run again with --accept-primary %s",
                                id, was_text == NULL ? "(none)" : was_text, becomes_text, becomes_text));
    }
    naula_memory_release(becomes_text);
    naula_memory_release(was_text);
    return accepted;
}

/**
 * Never remembers a refusal and never asks a person: each run names the disputed number and the record's number it
 * disagrees with, and only a run given that number with --accept-number takes it.
 */
static int accept_number(void *opaque, const char *id, const naula_home *account, uint64_t was, uint64_t becomes) {
    cli_context *context = opaque;
    for (size_t index = 0; index < context->arguments->accept_number_count; index++) {
        if (context->arguments->accept_number[index] == becomes) {
            return 1;
        }
    }
    ask(context, cli_format("number-disputed: an earlier record of %s holds %s account %s as repository %llu, and "
                            "the host now answers %llu; it may be a re-registered account, so check on the host, then "
                            "run again with --accept-number %llu",
                            id, account->provider == NULL ? "a" : account->provider,
                            account->account == NULL ? "(unnamed)" : account->account, (unsigned long long)was,
                            (unsigned long long)becomes, (unsigned long long)becomes));
    return 0;
}

void cli_print_questions(const cli_context *context) {
    for (size_t index = 0; index < context->question_count; index++) {
        fprintf(stderr, "naula: %s\n", context->questions[index]);
    }
}

static naula_status open_providers(cli_context *context, naula_refusal *refusal) {
    naula_status status = naula_providers_open(&context->providers);
#if defined(NAULA_TEST_HOOKS)
    if (status == NAULA_OK) {
        status = cli_test_forge_register(context->providers, refusal);
    }
#else
    (void)refusal;
#endif
    return status;
}

naula_status cli_context_open(const cli_arguments *arguments, cli_context *context, naula_refusal *refusal) {
    memset(context, 0, sizeof *context);
    context->arguments = arguments;
    context->cache_directory = cache_directory();
    if (context->cache_directory == NULL) {
        naula_refuse(refusal, "no-cache-folder", "set NAULA_CACHE to a folder naula may keep its cache in",
                     "no cache folder could be found for this user");
        return NAULA_REFUSED;
    }
    naula_status status = cli_make_folders(context->cache_directory, refusal);
    if (status == NAULA_OK) {
        status = open_providers(context, refusal);
    }
    if (status == NAULA_OK) {
        context->credentials = cli_credentials_open();
        status = context->credentials == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    }
    if (status == NAULA_OK) {
        status = cli_history_open(context->cache_directory, context->providers, context->credentials,
                                  &context->history, refusal);
    }
    if (status == NAULA_OK) {
        naula_session_options options;
        memset(&options, 0, sizeof options);
        options.size = sizeof options;
        options.cache_directory = context->cache_directory;
        options.providers = context->providers;
        options.history = cli_history_reader(context->history);
        options.token_for = session_token;
        options.accept_primary = accept_primary;
        options.accept_number = accept_number;
        options.offline = arguments->offline;
        options.context = context;
        status = naula_session_open(&options, &context->session, refusal);
    }
    if (status != NAULA_OK) {
        cli_context_close(context);
    }
    return status;
}

void cli_context_close(cli_context *context) {
    naula_session_close(context->session);
    cli_history_close(context->history);
    cli_credentials_close(context->credentials);
    naula_providers_close(context->providers);
    naula_memory_release(context->cache_directory);
    for (size_t index = 0; index < context->question_count; index++) {
        naula_memory_release(context->questions[index]);
    }
    memset(context, 0, sizeof *context);
}

naula_status cli_read_home_file(cli_context *context, const naula_place *place, const char *branch, uint8_t **bytes,
                                size_t *length, naula_refusal *refusal) {
    *bytes = NULL;
    *length = 0;
    const naula_provider *provider = naula_providers_find(context->providers, place->provider, refusal);
    if (provider == NULL) {
        return NAULA_REFUSED;
    }
    const char *token = cli_credentials_token(context->credentials, place->provider, place->host);
    naula_status status = provider->read_file(provider->context, place->host, place->number, branch, "repo.json",
                                              token, bytes, length, refusal);
    if (status == NAULA_REFUSED && strcmp(refusal->code, "missing_file_or_ref") == 0) {
        return NAULA_NOT_FOUND;
    }
    return status;
}

naula_status cli_resolve(cli_context *context, const naula_reference *reference, naula_resolution *resolution,
                         naula_refusal *refusal) {
    memset(resolution, 0, sizeof *resolution);
    resolution->size = sizeof *resolution;
    return naula_resolve(context->session, reference, resolution, refusal);
}

void cli_print_verdicts(const naula_resolution *resolution) {
    for (size_t index = 0; index < resolution->verdict_count; index++) {
        const naula_verdict *verdict = &resolution->verdicts[index];
        char *text = naula_place_text(&verdict->home);
        fprintf(stderr, "  %s %s: %s\n", text == NULL ? "(home)" : text, verdict->code, verdict->detail);
        naula_memory_release(text);
    }
}
