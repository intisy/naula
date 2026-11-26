#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "cache_internal.h"
#include "forge_common.h"
#include "json_document.h"
#include "naula/memory.h"

#define CACHE_TEXT_LIMIT 255u
#define LARGEST_EXACT_INTEGER 9007199254740991.0

static void say(char *problem, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(problem, NAULA_CACHE_PROBLEM_SIZE, format, arguments);
    va_end(arguments);
}

static char *record_path(const char *directory, const char *hex) {
    size_t length = strlen(directory) + strlen("/identities/") + strlen(hex) + strlen(".json") + 1;
    char *path = naula_memory_allocate(length);
    if (path != NULL) {
        snprintf(path, length, "%s/identities/%s.json", directory, hex);
    }
    return path;
}

int naula_cache_is_temporary_name(const char *name) {
    size_t hex = 0;
    while ((name[hex] >= '0' && name[hex] <= '9') || (name[hex] >= 'a' && name[hex] <= 'f')) {
        hex++;
    }
    size_t length = strlen(name);
    return (hex == 40 || hex == 64) && strncmp(name + hex, ".json.", 6) == 0 && length >= hex + 6 + 4 &&
           strcmp(name + length - 4, ".tmp") == 0;
}

void naula_cache_release(identity_cache *cache) {
    for (size_t index = 0; cache->homes != NULL && index < cache->count; index++) {
        naula_place_release(&cache->homes[index].place);
        naula_memory_release(cache->homes[index].account);
        naula_memory_release(cache->homes[index].etag);
    }
    naula_memory_release(cache->homes);
    memset(cache, 0, sizeof *cache);
}

static int is_word_text(const char *text) {
    if (text == NULL || text[0] < 'a' || text[0] > 'z' || strlen(text) > CACHE_TEXT_LIMIT) {
        return 0;
    }
    for (const char *at = text; *at != '\0'; at++) {
        if (!((*at >= 'a' && *at <= 'z') || (*at >= '0' && *at <= '9') || *at == '-')) {
            return 0;
        }
    }
    return 1;
}

static int is_plain_text(const char *text) {
    if (text == NULL || text[0] == '\0' || strlen(text) > CACHE_TEXT_LIMIT) {
        return 0;
    }
    for (const unsigned char *at = (const unsigned char *)text; *at != '\0'; at++) {
        if (*at <= 0x20 || *at >= 0x7F) {
            return 0;
        }
    }
    return 1;
}

static int read_whole_number(const cJSON *object, const char *member, double *value) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, member);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > LARGEST_EXACT_INTEGER ||
        floor(item->valuedouble) != item->valuedouble) {
        return 0;
    }
    *value = item->valuedouble;
    return 1;
}

static int optional_text(const cJSON *object, const char *member, const char **text) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, member);
    *text = NULL;
    if (item == NULL) {
        return 1;
    }
    *text = cJSON_IsString(item) ? item->valuestring : NULL;
    return is_plain_text(*text);
}

static char *copy_text(const char *text) {
    return text == NULL ? NULL : naula_memory_duplicate_text(text);
}

static naula_status fill_provider_home(const cJSON *item, const char *provider, cache_home *home, int *valid) {
    const char *account = naula_forge_text_member(item, "account");
    const char *host = NULL;
    uint64_t number = 0;
    if (!is_plain_text(account) || !optional_text(item, "host", &host) ||
        !naula_forge_read_exact_number(item, "number", &number)) {
        return NAULA_OK;
    }
    home->place.kind = NAULA_HOME_PROVIDER;
    home->place.number = number;
    home->place.provider = copy_text(provider);
    home->place.host = copy_text(host);
    home->account = copy_text(account);
    if (home->place.provider == NULL || (host != NULL && home->place.host == NULL) || home->account == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    *valid = 1;
    return NAULA_OK;
}

static naula_status fill_web_home(const cJSON *item, cache_home *home, int *valid) {
    const char *domain = naula_forge_text_member(item, "domain");
    if (!is_plain_text(domain)) {
        return NAULA_OK;
    }
    home->place.kind = NAULA_HOME_WEB;
    home->place.domain = copy_text(domain);
    if (home->place.domain == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    *valid = 1;
    return NAULA_OK;
}

static const char *const state_words[] = {"", "confirmed", "guard", "dead"};

static int read_state(const cJSON *item, cache_state *state) {
    const char *word = naula_forge_text_member(item, "state");
    for (int known = CACHE_CONFIRMED; word != NULL && known <= CACHE_DEAD; known++) {
        if (strcmp(word, state_words[known]) == 0) {
            *state = (cache_state)known;
            return 1;
        }
    }
    return 0;
}

static naula_status read_home(const cJSON *item, cache_home *home, int *valid) {
    double confirmed = 0;
    const char *etag = NULL;
    *valid = 0;
    home->place.size = sizeof home->place;
    const char *provider = cJSON_IsObject(item) ? naula_forge_text_member(item, "provider") : NULL;
    if (!is_word_text(provider) || !read_whole_number(item, "confirmed", &confirmed) ||
        !optional_text(item, "etag", &etag) || !read_state(item, &home->state) ||
        (home->state == CACHE_DEAD && strcmp(provider, "web") == 0)) {
        return NAULA_OK;
    }
    home->confirmed = (int64_t)confirmed;
    naula_status status = strcmp(provider, "web") == 0 ? fill_web_home(item, home, valid)
                                                        : fill_provider_home(item, provider, home, valid);
    if (status == NAULA_OK && *valid && etag != NULL) {
        home->etag = copy_text(etag);
        status = home->etag == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    }
    return status;
}

static naula_status read_record(const cJSON *json, const char *hex, identity_cache *cache, char *problem) {
    const char *id = cJSON_IsObject(json) ? naula_forge_text_member(json, "id") : NULL;
    const cJSON *homes = cJSON_IsObject(json) ? cJSON_GetObjectItemCaseSensitive(json, "homes") : NULL;
    double primary = 0;
    if (id == NULL || strncmp(id, "git:", 4) != 0 || strcmp(id + 4, hex) != 0) {
        say(problem, "the cache record for git:%s names the id %s and was ignored", hex, id == NULL ? "(none)" : id);
        return NAULA_OK;
    }
    int count = cJSON_IsArray(homes) ? cJSON_GetArraySize(homes) : 0;
    if (count <= 0 || count > NAULA_CACHE_HOME_LIMIT || !read_whole_number(json, "primary", &primary) ||
        primary >= (double)count) {
        say(problem, "the cache record for git:%s holds no readable homes or primary and was ignored", hex);
        return NAULA_OK;
    }
    cache->homes = naula_memory_allocate((size_t)count * sizeof *cache->homes);
    if (cache->homes == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    memset(cache->homes, 0, (size_t)count * sizeof *cache->homes);
    cache->count = (size_t)count;
    cache->primary = (size_t)primary;
    size_t index = 0;
    for (const cJSON *item = homes->child; item != NULL; item = item->next, index++) {
        int valid = 0;
        naula_status status = read_home(item, &cache->homes[index], &valid);
        if (status != NAULA_OK) {
            naula_cache_release(cache);
            return status;
        }
        if (!valid) {
            naula_cache_release(cache);
            say(problem, "the cache record for git:%s holds a home naula cannot read and was ignored", hex);
            return NAULA_OK;
        }
    }
    if (cache->homes[cache->primary].state != CACHE_CONFIRMED) {
        naula_cache_release(cache);
        say(problem, "the cache record for git:%s names a primary it does not hold confirmed and was ignored", hex);
        return NAULA_OK;
    }
    cache->present = 1;
    return NAULA_OK;
}

static naula_status read_bytes(const uint8_t *bytes, size_t length, const char *hex, identity_cache *cache,
                               char *problem) {
    cJSON *json = NULL;
    const char *reason = NULL;
    naula_status status = naula_json_parse(bytes, length, &json, &reason);
    if (status == NAULA_MALFORMED) {
        say(problem, "the cache record for git:%s is not JSON (%s) and was ignored", hex,
            reason == NULL ? "unreadable" : reason);
        return NAULA_OK;
    }
    if (status != NAULA_OK) {
        return status;
    }
    status = read_record(json, hex, cache, problem);
    cJSON_Delete(json);
    return status;
}

naula_status naula_cache_read(const char *directory, const char *hex, identity_cache *cache, char *problem) {
    memset(cache, 0, sizeof *cache);
    problem[0] = '\0';
    if (directory == NULL) {
        return NAULA_OK;
    }
    char *path = record_path(directory, hex);
    if (path == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    uint8_t *bytes = NULL;
    size_t length = 0;
    naula_status status = naula_cache_file_read(path, &bytes, &length);
    if (status == NAULA_OK) {
        status = read_bytes(bytes, length, hex, cache, problem);
    } else if (status == NAULA_NOT_FOUND) {
        status = NAULA_OK;
    } else if (status == NAULA_REFUSED) {
        say(problem, "the cache record %s cannot be read and was ignored", path);
        status = NAULA_OK;
    }
    naula_memory_release(bytes);
    naula_memory_release(path);
    return status;
}

static int add_home(cJSON *list, const cache_home *home) {
    cJSON *item = cJSON_CreateObject();
    if (item == NULL || !cJSON_AddItemToArray(list, item)) {
        cJSON_Delete(item);
        return 0;
    }
    const naula_place *place = &home->place;
    if (cJSON_AddStringToObject(item, "state", state_words[home->state]) == NULL) {
        return 0;
    }
    if (place->kind == NAULA_HOME_WEB) {
        return cJSON_AddStringToObject(item, "provider", "web") != NULL &&
               cJSON_AddStringToObject(item, "domain", place->domain) != NULL &&
               cJSON_AddNumberToObject(item, "confirmed", (double)home->confirmed) != NULL &&
               (home->etag == NULL || cJSON_AddStringToObject(item, "etag", home->etag) != NULL);
    }
    return cJSON_AddStringToObject(item, "provider", place->provider) != NULL &&
           (place->host == NULL || cJSON_AddStringToObject(item, "host", place->host) != NULL) &&
           cJSON_AddStringToObject(item, "account", home->account) != NULL &&
           cJSON_AddNumberToObject(item, "number", (double)place->number) != NULL &&
           cJSON_AddNumberToObject(item, "confirmed", (double)home->confirmed) != NULL &&
           (home->etag == NULL || cJSON_AddStringToObject(item, "etag", home->etag) != NULL);
}

static char *print_record(const char *hex, const identity_cache *cache, const unsigned char *kept) {
    char id[70];
    size_t primary = 0;
    for (size_t index = 0; index < cache->primary; index++) {
        primary += kept[index];
    }
    snprintf(id, sizeof id, "git:%s", hex);
    cJSON *root = cJSON_CreateObject();
    cJSON *homes = root == NULL ? NULL : cJSON_AddArrayToObject(root, "homes");
    int built = homes != NULL && cJSON_AddStringToObject(root, "id", id) != NULL &&
                cJSON_AddNumberToObject(root, "primary", (double)primary) != NULL;
    for (size_t index = 0; built && index < cache->count; index++) {
        built = !kept[index] || add_home(homes, &cache->homes[index]);
    }
    char *text = built ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return text;
}

/** Leaves out the oldest GUARD homes past the limit; 0 when CONFIRMED and DEAD homes alone pass it. */
static int choose_kept(const identity_cache *cache, unsigned char *kept) {
    size_t kept_count = cache->count;
    size_t fixed = 0;
    for (size_t index = 0; index < cache->count; index++) {
        kept[index] = 1;
        fixed += cache->homes[index].state != CACHE_GUARD ? 1u : 0u;
    }
    if (fixed > NAULA_CACHE_HOME_LIMIT) {
        return 0;
    }
    while (kept_count > NAULA_CACHE_HOME_LIMIT) {
        size_t oldest = cache->count;
        for (size_t index = 0; index < cache->count; index++) {
            int guard = kept[index] && cache->homes[index].state == CACHE_GUARD;
            if (guard && (oldest == cache->count || cache->homes[index].confirmed < cache->homes[oldest].confirmed)) {
                oldest = index;
            }
        }
        kept[oldest] = 0;
        kept_count--;
    }
    return 1;
}

naula_status naula_cache_write(const char *directory, const char *hex, const identity_cache *cache, char *problem) {
    unsigned char *kept = naula_memory_allocate(cache->count);
    if (kept == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    if (!choose_kept(cache, kept)) {
        naula_memory_release(kept);
        say(problem, "the cache record for git:%s would hold more confirmed and gone homes than %d and was not written",
            hex, NAULA_CACHE_HOME_LIMIT);
        return NAULA_OK;
    }
    char *text = print_record(hex, cache, kept);
    naula_memory_release(kept);
    if (text == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    char *path = record_path(directory, hex);
    naula_status status = path == NULL ? NAULA_OUT_OF_MEMORY
                                       : naula_cache_file_replace(directory, path, (const uint8_t *)text, strlen(text));
    if (status == NAULA_REFUSED) {
        say(problem, "the cache record %s could not be written", path);
        status = NAULA_OK;
    }
    naula_memory_release(path);
    cJSON_free(text);
    return status;
}
