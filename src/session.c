#include <stddef.h>
#include <string.h>

#include "naula/memory.h"
#include "naula/session.h"
#include "resolve_internal.h"

#define OPTIONS_FIX "fill the options to their first shape, with a history whose functions are all set"

static int covers_first_shape(const naula_session_options *options) {
    return options->size >= offsetof(naula_session_options, context) + sizeof options->context;
}

static int history_is_whole(const naula_history *history) {
    return history->size >= offsetof(naula_history, parent_count) + sizeof history->parent_count &&
           history->contains != NULL && history->parent_count != NULL;
}

/** A drive path (`C:/`, `C:\`), a UNC or `\\?\` path, or a POSIX path from the root. */
static int is_absolute(const char *path) {
    int drive = ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':' &&
                (path[2] == '/' || path[2] == '\\');
#if defined(_WIN32)
    return drive || (path[0] == '\\' && path[1] == '\\') || (path[0] == '/' && path[1] == '/');
#else
    (void)drive;
    return path[0] == '/';
#endif
}

naula_status naula_session_open(const naula_session_options *options, naula_session **session,
                                naula_refusal *refusal) {
    if (session == NULL) {
        naula_refuse(refusal, "invalid_options", OPTIONS_FIX, "no session to open into");
        return NAULA_REFUSED;
    }
    *session = NULL;
    if (options == NULL || !covers_first_shape(options) ||
        (options->history != NULL && !history_is_whole(options->history))) {
        naula_refuse(refusal, "invalid_options", OPTIONS_FIX,
                     "the options or the history are smaller than their first shape");
        return NAULA_REFUSED;
    }
    if (options->cache_directory != NULL && !is_absolute(options->cache_directory)) {
        naula_refuse(refusal, "invalid_options", "name the cache folder by an absolute path",
                     "the cache folder %s is relative, and is never read against the working directory",
                     options->cache_directory);
        return NAULA_REFUSED;
    }
    naula_session *opened = naula_memory_allocate(sizeof *opened);
    if (opened == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    memset(opened, 0, sizeof *opened);
    if (options->cache_directory != NULL) {
        opened->cache_directory = naula_memory_duplicate_text(options->cache_directory);
        if (opened->cache_directory == NULL) {
            naula_session_close(opened);
            return NAULA_OUT_OF_MEMORY;
        }
    }
    if (options->providers != NULL) {
        opened->providers = options->providers;
    } else {
        naula_status status = naula_providers_open(&opened->providers);
        if (status != NAULA_OK) {
            naula_session_close(opened);
            return status;
        }
        opened->owns_providers = 1;
    }
    if (options->history != NULL) {
        opened->history.size = sizeof opened->history;
        opened->history.context = options->history->context;
        opened->history.contains = options->history->contains;
        opened->history.parent_count = options->history->parent_count;
    } else {
        naula_history_over_providers(opened, &opened->history);
    }
    opened->token_for = options->token_for;
    opened->accept_primary = options->accept_primary;
    opened->accept_number = options->accept_number;
    opened->offline = options->offline;
    opened->context = options->context;
    *session = opened;
    return NAULA_OK;
}

void naula_session_close(naula_session *session) {
    if (session == NULL) {
        return;
    }
    if (session->owns_providers) {
        naula_providers_close(session->providers);
    }
    naula_memory_release(session->cache_directory);
    naula_memory_release(session);
}

const char *naula_session_token(const naula_session *session, const char *provider, const char *host) {
    return session->token_for == NULL ? NULL : session->token_for(session->context, provider, host);
}
