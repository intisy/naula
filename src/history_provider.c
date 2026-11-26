#include <string.h>

#include "naula/provider.h"
#include "resolve_internal.h"

#define GIT_READER_FIX "resolve it with the naula command line, which reads history with git"

static const naula_provider *provider_for(naula_session *session, const naula_place *home, naula_refusal *refusal) {
    if (home->kind != NAULA_HOME_PROVIDER) {
        naula_refuse(refusal, "unverifiable", GIT_READER_FIX,
                     "the installer reads history only through a provider, and a %s home has none",
                     home->kind == NAULA_HOME_WEB ? "web" : "url");
        return NULL;
    }
    return naula_providers_find(session->providers, home->provider, refusal);
}

static naula_status provider_contains(void *context, const naula_place *home, const char *ref, const char *commit,
                                      int *contained, naula_refusal *refusal) {
    naula_session *session = context;
    *contained = 0;
    const naula_provider *provider = provider_for(session, home, refusal);
    if (provider == NULL) {
        return NAULA_REFUSED;
    }
    return provider->contains(provider->context, home->host, home->number, ref, commit,
                              naula_session_token(session, home->provider, home->host), contained, refusal);
}

static naula_status provider_parent_count(void *context, const naula_place *home, const char *commit, size_t *count,
                                          naula_refusal *refusal) {
    naula_session *session = context;
    *count = 0;
    const naula_provider *provider = provider_for(session, home, refusal);
    if (provider == NULL) {
        return NAULA_REFUSED;
    }
    return provider->parent_count(provider->context, home->host, home->number, commit,
                                  naula_session_token(session, home->provider, home->host), count, refusal);
}

void naula_history_over_providers(naula_session *session, naula_history *history) {
    memset(history, 0, sizeof *history);
    history->size = sizeof *history;
    history->context = session;
    history->contains = provider_contains;
    history->parent_count = provider_parent_count;
}
