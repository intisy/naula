#include <stddef.h>
#include <string.h>

#include "naula/memory.h"
#include "naula/provider.h"

struct naula_providers {
    naula_provider **entries;
    size_t count;
    size_t capacity;
};

static int covers_first_shape(const naula_provider *provider) {
    return provider->size >= offsetof(naula_provider, release_index) + sizeof provider->release_index;
}

int naula_provider_offers_writes(const naula_provider *provider) {
    return provider->size >= offsetof(naula_provider, write_metadata) + sizeof provider->write_metadata;
}

static int has_every_write(const naula_provider *provider) {
    return provider->create != NULL && provider->rename != NULL && provider->transfer != NULL &&
           provider->archive != NULL && provider->read_metadata != NULL && provider->write_metadata != NULL;
}

static int has_every_function(const naula_provider *provider) {
    return provider->describe != NULL && provider->lookup != NULL && provider->find != NULL &&
           provider->read_file != NULL && provider->contains != NULL && provider->parent_count != NULL &&
           provider->list_repos != NULL && provider->release_index != NULL &&
           (!naula_provider_offers_writes(provider) || has_every_write(provider));
}

naula_status naula_refuse_account_rename(const char *word, naula_refusal *refusal) {
    naula_refuse(refusal, "unsupported_operation", NAULA_ACCOUNT_RENAME_FIX,
                 "the %s provider cannot rename an account over its API", word);
    return NAULA_REFUSED;
}

static const naula_provider *entry_for(const naula_providers *providers, const char *word) {
    for (size_t index = 0; index < providers->count; index++) {
        if (strcmp(providers->entries[index]->word, word) == 0) {
            return providers->entries[index];
        }
    }
    return NULL;
}

static int make_room(naula_providers *providers) {
    if (providers->count < providers->capacity) {
        return 1;
    }
    size_t capacity = providers->capacity == 0 ? 8 : providers->capacity * 2;
    naula_provider **entries = naula_memory_reallocate(providers->entries, capacity * sizeof *entries);
    if (entries == NULL) {
        return 0;
    }
    providers->entries = entries;
    providers->capacity = capacity;
    return 1;
}

naula_status naula_providers_register(naula_providers *providers, const naula_provider *provider,
                                      naula_refusal *refusal) {
    if (!covers_first_shape(provider) || !has_every_function(provider) || provider->word == NULL ||
        provider->word[0] == '\0') {
        naula_refuse(refusal, "invalid_provider", "fill every function the provider's size covers and name its word",
                     "the provider is smaller than its first shape or names no word");
        return NAULA_REFUSED;
    }
    if (entry_for(providers, provider->word) != NULL) {
        naula_refuse(refusal, "provider_word_taken", "register one provider per word, or name another word",
                     "a provider for %s is already registered", provider->word);
        return NAULA_REFUSED;
    }
    naula_provider *copy = naula_memory_allocate(sizeof *copy);
    if (copy == NULL || !make_room(providers)) {
        naula_memory_release(copy);
        return NAULA_OUT_OF_MEMORY;
    }
    memset(copy, 0, sizeof *copy);
    uint32_t kept = provider->size < sizeof *copy ? provider->size : (uint32_t)sizeof *copy;
    memcpy(copy, provider, kept);
    copy->size = kept;
    providers->entries[providers->count++] = copy;
    return NAULA_OK;
}

naula_status naula_providers_open(naula_providers **providers) {
    naula_providers *opened = naula_memory_allocate(sizeof *opened);
    if (opened == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    memset(opened, 0, sizeof *opened);
    const naula_provider *built_in[] = {naula_provider_github(), naula_provider_gitea(), naula_provider_forgejo(),
                                        naula_provider_web()};
    for (size_t index = 0; index < sizeof built_in / sizeof built_in[0]; index++) {
        naula_status status = naula_providers_register(opened, built_in[index], NULL);
        if (status != NAULA_OK) {
            naula_providers_close(opened);
            return status;
        }
    }
    *providers = opened;
    return NAULA_OK;
}

const naula_provider *naula_providers_find(const naula_providers *providers, const char *word,
                                           naula_refusal *refusal) {
    const naula_provider *found = entry_for(providers, word);
    if (found == NULL) {
        naula_refuse(refusal, "unknown_provider", NAULA_UNKNOWN_PROVIDER_FIX,
                     "no provider is built in or installed for %s", word);
    }
    return found;
}

void naula_providers_close(naula_providers *providers) {
    if (providers == NULL) {
        return;
    }
    for (size_t index = 0; index < providers->count; index++) {
        naula_memory_release(providers->entries[index]);
    }
    naula_memory_release(providers->entries);
    naula_memory_release(providers);
}
