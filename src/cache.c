#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cache_internal.h"
#include "forge_common.h"
#include "json_document.h"
#include "naula/memory.h"
#include "naula/sha256.h"

#define CACHE_TEXT_LIMIT 255u
#define CACHE_AT_TEXT_LIMIT 2048u
#define PLACE_TEXT_SIZE 2304
#define LARGEST_EXACT_INTEGER 9007199254740991.0
#define GUARDS_SUFFIX ".guards.json"

static void say(char *problem, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(problem, NAULA_CACHE_PROBLEM_SIZE, format, arguments);
    va_end(arguments);
}

static char *joined_path(const char *directory, const char *hex, const char *tail) {
    size_t length = strlen(directory) + strlen("/identities/") + strlen(hex) + strlen(tail) + 1;
    char *path = naula_memory_allocate(length);
    if (path != NULL) {
        snprintf(path, length, "%s/identities/%s%s", directory, hex, tail);
    }
    return path;
}

static char *record_path(const char *directory, const char *hex, const char *anchor) {
    char tail[NAULA_CACHE_ANCHOR_SIZE + 8];
    snprintf(tail, sizeof tail, "-%s.json", anchor);
    return joined_path(directory, hex, tail);
}

static size_t hex_run(const char *text) {
    size_t length = 0;
    while ((text[length] >= '0' && text[length] <= '9') || (text[length] >= 'a' && text[length] <= 'f')) {
        length++;
    }
    return length;
}

static int is_id_hex(size_t length) {
    return length == 40 || length == 64;
}

/** The anchor of a record file name `<hex>-<anchor>.json` for `hex`, or NULL. */
static const char *record_anchor(const char *name, const char *hex) {
    size_t hex_length = strlen(hex);
    if (strncmp(name, hex, hex_length) != 0 || name[hex_length] != '-') {
        return NULL;
    }
    const char *anchor = name + hex_length + 1;
    size_t anchor_length = NAULA_CACHE_ANCHOR_SIZE - 1;
    return hex_run(anchor) == anchor_length && strcmp(anchor + anchor_length, ".json") == 0 ? anchor : NULL;
}

int naula_cache_is_temporary_name(const char *name) {
    size_t hex = hex_run(name);
    const char *rest = NULL;
    if (is_id_hex(hex) && name[hex] == '-' && hex_run(name + hex + 1) == NAULA_CACHE_ANCHOR_SIZE - 1) {
        rest = name + hex + NAULA_CACHE_ANCHOR_SIZE;
        rest = strncmp(rest, ".json.", 6) == 0 ? rest + 6 : NULL;
    } else if (is_id_hex(hex) && strncmp(name + hex, GUARDS_SUFFIX ".", strlen(GUARDS_SUFFIX) + 1) == 0) {
        rest = name + hex + strlen(GUARDS_SUFFIX) + 1;
    }
    size_t length = rest == NULL ? 0 : strlen(rest);
    return rest != NULL && length >= 4 && strcmp(rest + length - 4, ".tmp") == 0;
}

static void lowercase(char *text) {
    for (; *text != '\0'; text++) {
        if (*text >= 'A' && *text <= 'Z') {
            *text = (char)(*text + ('a' - 'A'));
        }
    }
}

/** GitHub's own host may be written or left out of a GitHub place, so it is always left out. */
static char *normal_place_text(const naula_place *place) {
    char text[PLACE_TEXT_SIZE];
    char host[300];
    if (place->kind == NAULA_HOME_WEB) {
        snprintf(text, sizeof text, "web:%s", place->domain);
        lowercase(text);
    } else if (place->kind == NAULA_HOME_URL) {
        snprintf(text, sizeof text, "%s", place->url);
    } else {
        snprintf(host, sizeof host, "%s", place->host == NULL ? "" : place->host);
        lowercase(host);
        if (strcmp(place->provider, "github") == 0 && strcmp(host, "github.com") == 0) {
            host[0] = '\0';
        }
        snprintf(text, sizeof text, "%s%s%s:%llu", place->provider, host[0] == '\0' ? "" : "@", host,
                 (unsigned long long)place->number);
    }
    return naula_memory_duplicate_text(text);
}

static int compare_texts(const void *left, const void *right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

void naula_cache_release_texts(char **texts, size_t count) {
    for (size_t index = 0; texts != NULL && index < count; index++) {
        naula_memory_release(texts[index]);
    }
    naula_memory_release(texts);
}

naula_status naula_cache_normal_at(const naula_place *places, size_t count, char ***texts, size_t *text_count) {
    char **made = naula_memory_allocate((count == 0 ? 1 : count) * sizeof *made);
    *texts = NULL;
    *text_count = 0;
    if (made == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < count; index++) {
        made[index] = normal_place_text(&places[index]);
        if (made[index] == NULL) {
            naula_cache_release_texts(made, index);
            return NAULA_OUT_OF_MEMORY;
        }
    }
    qsort(made, count, sizeof *made, compare_texts);
    size_t kept = 0;
    for (size_t index = 0; index < count; index++) {
        if (kept > 0 && strcmp(made[kept - 1], made[index]) == 0) {
            naula_memory_release(made[index]);
        } else {
            made[kept++] = made[index];
        }
    }
    *texts = made;
    *text_count = kept;
    return NAULA_OK;
}

static void hash_texts(char *const *texts, size_t count, char anchor[NAULA_CACHE_ANCHOR_SIZE]) {
    naula_sha256 hash;
    naula_sha256_start(&hash);
    for (size_t index = 0; index < count; index++) {
        if (index > 0) {
            naula_sha256_add(&hash, (const uint8_t *)"\n", 1);
        }
        naula_sha256_add(&hash, (const uint8_t *)texts[index], strlen(texts[index]));
    }
    naula_sha256_hex(&hash, anchor);
}

naula_status naula_cache_anchor(const naula_place *places, size_t count, char anchor[NAULA_CACHE_ANCHOR_SIZE]) {
    char **texts = NULL;
    size_t text_count = 0;
    naula_status status = naula_cache_normal_at(places, count, &texts, &text_count);
    if (status == NAULA_OK) {
        hash_texts(texts, text_count, anchor);
    }
    naula_cache_release_texts(texts, text_count);
    return status;
}

void naula_cache_release(identity_cache *cache) {
    for (size_t index = 0; cache->homes != NULL && index < cache->count; index++) {
        naula_place_release(&cache->homes[index].place);
        naula_memory_release(cache->homes[index].account);
        naula_memory_release(cache->homes[index].etag);
    }
    naula_memory_release(cache->homes);
    naula_cache_release_texts(cache->at, cache->at_count);
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

static int is_text_within(const char *text, size_t limit) {
    if (text == NULL || text[0] == '\0' || strlen(text) > limit) {
        return 0;
    }
    for (const unsigned char *at = (const unsigned char *)text; *at != '\0'; at++) {
        if (*at <= 0x20 || *at >= 0x7F) {
            return 0;
        }
    }
    return 1;
}

static int is_plain_text(const char *text) {
    return is_text_within(text, CACHE_TEXT_LIMIT);
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

static const char *const state_words[] = {"", "confirmed", "guard"};

static int read_state(const cJSON *item, cache_state *state) {
    const char *word = naula_forge_text_member(item, "state");
    for (int known = CACHE_CONFIRMED; word != NULL && known <= CACHE_GUARD; known++) {
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
        !optional_text(item, "etag", &etag) || !read_state(item, &home->state)) {
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

/** Reads the array `member` of homes; `valid` 0 for one out of shape or past the limit. */
static naula_status read_homes(const cJSON *json, const char *member, identity_cache *cache, int *valid) {
    const cJSON *homes = cJSON_GetObjectItemCaseSensitive(json, member);
    int count = cJSON_IsArray(homes) ? cJSON_GetArraySize(homes) : -1;
    *valid = 0;
    if (count <= 0 || count > NAULA_CACHE_HOME_LIMIT) {
        return NAULA_OK;
    }
    cache->homes = naula_memory_allocate((size_t)count * sizeof *cache->homes);
    if (cache->homes == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    memset(cache->homes, 0, (size_t)count * sizeof *cache->homes);
    cache->count = (size_t)count;
    size_t index = 0;
    int all_valid = 1;
    for (const cJSON *item = homes->child; item != NULL && all_valid; item = item->next, index++) {
        naula_status status = read_home(item, &cache->homes[index], &all_valid);
        if (status != NAULA_OK) {
            return status;
        }
    }
    *valid = all_valid;
    return NAULA_OK;
}

static naula_status read_stated_at(const cJSON *json, identity_cache *cache, int *valid) {
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(json, "at");
    int count = cJSON_IsArray(list) ? cJSON_GetArraySize(list) : 0;
    *valid = 0;
    if (count <= 0 || count > NAULA_CACHE_HOME_LIMIT) {
        return NAULA_OK;
    }
    cache->at = naula_memory_allocate((size_t)count * sizeof *cache->at);
    if (cache->at == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    for (const cJSON *item = list->child; item != NULL; item = item->next) {
        if (!cJSON_IsString(item) || !is_text_within(item->valuestring, CACHE_AT_TEXT_LIMIT)) {
            return NAULA_OK;
        }
        cache->at[cache->at_count] = copy_text(item->valuestring);
        if (cache->at[cache->at_count] == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        cache->at_count++;
    }
    *valid = 1;
    return NAULA_OK;
}

static int same_texts(char *const *left, size_t left_count, char *const *right, size_t right_count) {
    if (left_count != right_count) {
        return 0;
    }
    for (size_t index = 0; index < left_count; index++) {
        if (strcmp(left[index], right[index]) != 0) {
            return 0;
        }
    }
    return 1;
}

static int names_the_id(const cJSON *json, const char *hex) {
    const char *id = cJSON_IsObject(json) ? naula_forge_text_member(json, "id") : NULL;
    return id != NULL && strncmp(id, "git:", 4) == 0 && strcmp(id + 4, hex) == 0;
}

static int primary_fits(const identity_cache *cache, int has_primary) {
    int any_confirmed = 0;
    for (size_t entry = 0; entry < cache->count; entry++) {
        any_confirmed |= cache->homes[entry].state == CACHE_CONFIRMED;
    }
    return any_confirmed == has_primary && (!has_primary || cache->homes[cache->primary].state == CACHE_CONFIRMED);
}

static naula_status read_record(const cJSON *json, const char *hex, char *const *at, size_t at_count,
                                identity_cache *cache, char *problem) {
    double primary = 0;
    if (!names_the_id(json, hex)) {
        say(problem, "the cache record for git:%s names another id and was ignored", hex);
        return NAULA_OK;
    }
    int valid = 0;
    naula_status status = read_stated_at(json, cache, &valid);
    if (status != NAULA_OK || !valid) {
        naula_cache_release(cache);
        if (status == NAULA_OK) {
            say(problem, "the cache record for git:%s states no readable at and was ignored", hex);
        }
        return status;
    }
    if (at != NULL && !same_texts(cache->at, cache->at_count, at, at_count)) {
        naula_cache_release(cache);
        cache->foreign = 1;
        say(problem, "the cache record for git:%s was written for another at; it was ignored and is kept", hex);
        return NAULA_OK;
    }
    int has_primary = cJSON_GetObjectItemCaseSensitive(json, "primary") != NULL;
    status = read_homes(json, "homes", cache, &valid);
    if (status == NAULA_OK && valid && has_primary) {
        valid = read_whole_number(json, "primary", &primary) && primary < (double)cache->count;
    }
    cache->primary = (size_t)primary;
    cache->has_primary = has_primary;
    if (status == NAULA_OK && (!valid || !primary_fits(cache, has_primary))) {
        say(problem, "the cache record for git:%s holds a home or primary naula cannot read and was ignored", hex);
    }
    if (status != NAULA_OK || !valid || !primary_fits(cache, has_primary)) {
        naula_cache_release(cache);
        return status;
    }
    cache->present = 1;
    return NAULA_OK;
}

/** Reads and parses the JSON at `path`; `json` NULL when the file is absent or was reported unreadable. */
static naula_status read_json_file(const char *path, const char *hex, cJSON **json, char *problem) {
    uint8_t *bytes = NULL;
    size_t length = 0;
    const char *reason = NULL;
    *json = NULL;
    naula_status status = naula_cache_file_read(path, &bytes, &length);
    if (status == NAULA_OK) {
        status = naula_json_parse(bytes, length, json, &reason);
        if (status == NAULA_MALFORMED) {
            say(problem, "the cache record for git:%s is not JSON (%s) and was ignored", hex,
                reason == NULL ? "unreadable" : reason);
            status = NAULA_OK;
        }
    } else if (status == NAULA_NOT_FOUND) {
        status = NAULA_OK;
    } else if (status == NAULA_REFUSED) {
        say(problem, "the cache record %s cannot be read and was ignored", path);
        status = NAULA_OK;
    }
    naula_memory_release(bytes);
    return status;
}

naula_status naula_cache_read(const char *directory, const char *hex, const char *anchor, char *const *at,
                              size_t at_count, identity_cache *cache, char *problem) {
    memset(cache, 0, sizeof *cache);
    if (directory == NULL) {
        return NAULA_OK;
    }
    char *path = record_path(directory, hex, anchor);
    if (path == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    cJSON *json = NULL;
    naula_status status = read_json_file(path, hex, &json, problem);
    if (status == NAULA_OK && json != NULL) {
        status = read_record(json, hex, at, at_count, cache, problem);
    }
    cJSON_Delete(json);
    naula_memory_release(path);
    return status;
}

naula_status naula_cache_read_guards(const char *directory, const char *hex, identity_cache *guards, char *problem) {
    memset(guards, 0, sizeof *guards);
    if (directory == NULL) {
        return NAULA_OK;
    }
    char *path = joined_path(directory, hex, GUARDS_SUFFIX);
    if (path == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    cJSON *json = NULL;
    naula_status status = read_json_file(path, hex, &json, problem);
    int valid = 0;
    if (status == NAULA_OK && json != NULL && names_the_id(json, hex)) {
        status = read_homes(json, "guards", guards, &valid);
    }
    for (size_t index = 0; status == NAULA_OK && valid && index < guards->count; index++) {
        valid = guards->homes[index].state == CACHE_GUARD && guards->homes[index].place.kind == NAULA_HOME_PROVIDER;
    }
    if (status == NAULA_OK && json != NULL && !valid) {
        say(problem, "the guards for git:%s cannot be read and were ignored", hex);
    }
    if (status != NAULA_OK || !valid) {
        naula_cache_release(guards);
    } else {
        guards->present = 1;
    }
    cJSON_Delete(json);
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

static int add_homes(cJSON *root, const char *member, const identity_cache *cache, const unsigned char *kept) {
    cJSON *homes = cJSON_AddArrayToObject(root, member);
    int built = homes != NULL;
    for (size_t index = 0; built && index < cache->count; index++) {
        built = !kept[index] || add_home(homes, &cache->homes[index]);
    }
    return built;
}

static char *print_record(const char *hex, const identity_cache *cache, const unsigned char *kept) {
    char id[70];
    size_t primary = 0;
    for (size_t index = 0; index < cache->primary; index++) {
        primary += kept[index];
    }
    snprintf(id, sizeof id, "git:%s", hex);
    cJSON *root = cJSON_CreateObject();
    cJSON *at = root == NULL ? NULL : cJSON_AddArrayToObject(root, "at");
    int built = at != NULL && cJSON_AddStringToObject(root, "id", id) != NULL &&
                (!cache->has_primary || cJSON_AddNumberToObject(root, "primary", (double)primary) != NULL);
    for (size_t index = 0; built && index < cache->at_count; index++) {
        cJSON *text = cJSON_CreateString(cache->at[index]);
        built = text != NULL && cJSON_AddItemToArray(at, text);
        if (text != NULL && !built) {
            cJSON_Delete(text);
        }
    }
    built = built && add_homes(root, "homes", cache, kept);
    char *text = built ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return text;
}

static char *print_guards(const char *hex, const identity_cache *guards, const unsigned char *kept) {
    char id[70];
    snprintf(id, sizeof id, "git:%s", hex);
    cJSON *root = cJSON_CreateObject();
    int built = root != NULL && cJSON_AddStringToObject(root, "id", id) != NULL &&
                add_homes(root, "guards", guards, kept);
    char *text = built ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return text;
}

/** Leaves out the oldest GUARD homes past the limit; 0 when CONFIRMED homes alone pass it. */
static int choose_kept(const identity_cache *cache, unsigned char *kept) {
    size_t kept_count = cache->count;
    size_t fixed = 0;
    for (size_t index = 0; index < cache->count; index++) {
        kept[index] = 1;
        fixed += cache->homes[index].state == CACHE_CONFIRMED ? 1u : 0u;
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

static naula_status replace_with(const char *directory, char *path, char *text, char *problem) {
    naula_status status = path == NULL || text == NULL
                              ? NAULA_OUT_OF_MEMORY
                              : naula_cache_file_replace(directory, path, (const uint8_t *)text, strlen(text));
    if (status == NAULA_REFUSED) {
        say(problem, "the cache record %s could not be written", path);
        status = NAULA_OK;
    }
    naula_memory_release(path);
    cJSON_free(text);
    return status;
}

naula_status naula_cache_write(const char *directory, const char *hex, const char *anchor, const identity_cache *cache,
                               char *problem) {
    if (cache->count == 0 || cache->foreign) {
        return NAULA_OK;
    }
    unsigned char *kept = naula_memory_allocate(cache->count);
    if (kept == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    if (!choose_kept(cache, kept)) {
        naula_memory_release(kept);
        say(problem, "the cache record for git:%s would hold more confirmed homes than %d and was not written", hex,
            NAULA_CACHE_HOME_LIMIT);
        return NAULA_OK;
    }
    char *text = print_record(hex, cache, kept);
    naula_memory_release(kept);
    if (text == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    return replace_with(directory, record_path(directory, hex, anchor), text, problem);
}

static int equals_ignoring_case(const char *left, const char *right) {
    if (left == NULL || right == NULL) {
        return left == right;
    }
    for (; *left != '\0' && *right != '\0'; left++, right++) {
        char lower_left = (*left >= 'A' && *left <= 'Z') ? (char)(*left + ('a' - 'A')) : *left;
        char lower_right = (*right >= 'A' && *right <= 'Z') ? (char)(*right + ('a' - 'A')) : *right;
        if (lower_left != lower_right) {
            return 0;
        }
    }
    return *left == *right;
}

static int same_guard(const cache_home *left, const cache_home *right) {
    return strcmp(left->place.provider, right->place.provider) == 0 &&
           equals_ignoring_case(left->place.host, right->place.host) &&
           equals_ignoring_case(left->account, right->account) && left->place.number == right->place.number;
}

static naula_status copy_guard(const cache_home *from, cache_home *to) {
    memset(to, 0, sizeof *to);
    to->state = CACHE_GUARD;
    to->confirmed = from->confirmed;
    to->place.size = sizeof to->place;
    to->place.kind = NAULA_HOME_PROVIDER;
    to->place.number = from->place.number;
    to->place.provider = copy_text(from->place.provider);
    to->place.host = copy_text(from->place.host);
    to->account = copy_text(from->account);
    return to->place.provider == NULL || (from->place.host != NULL && to->place.host == NULL) || to->account == NULL
               ? NAULA_OUT_OF_MEMORY
               : NAULA_OK;
}

static naula_status merge_guards(const identity_cache *guards, const identity_cache *learned, identity_cache *merged) {
    size_t capacity = guards->count + learned->count;
    memset(merged, 0, sizeof *merged);
    merged->homes = naula_memory_allocate((capacity == 0 ? 1 : capacity) * sizeof *merged->homes);
    if (merged->homes == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    const identity_cache *sources[2] = {guards, learned};
    for (int source = 0; source < 2; source++) {
        for (size_t index = 0; index < sources[source]->count; index++) {
            const cache_home *home = &sources[source]->homes[index];
            if (home->place.kind != NAULA_HOME_PROVIDER || home->account == NULL) {
                continue;
            }
            size_t existing = 0;
            while (existing < merged->count && !same_guard(&merged->homes[existing], home)) {
                existing++;
            }
            if (existing < merged->count) {
                if (home->confirmed > merged->homes[existing].confirmed) {
                    merged->homes[existing].confirmed = home->confirmed;
                }
                continue;
            }
            naula_status status = copy_guard(home, &merged->homes[merged->count++]);
            if (status != NAULA_OK) {
                return status;
            }
        }
    }
    return NAULA_OK;
}

naula_status naula_cache_write_guards(const char *directory, const char *hex, const identity_cache *guards,
                                      const identity_cache *learned, char *problem) {
    identity_cache merged;
    naula_status status = merge_guards(guards, learned, &merged);
    unsigned char *kept = status == NAULA_OK && merged.count > 0 ? naula_memory_allocate(merged.count) : NULL;
    if (status == NAULA_OK && merged.count > 0 && kept == NULL) {
        status = NAULA_OUT_OF_MEMORY;
    }
    if (status == NAULA_OK && merged.count > 0) {
        choose_kept(&merged, kept);
        char *text = print_guards(hex, &merged, kept);
        status = text == NULL ? NAULA_OUT_OF_MEMORY
                              : replace_with(directory, joined_path(directory, hex, GUARDS_SUFFIX), text, problem);
    }
    naula_memory_release(kept);
    naula_cache_release(&merged);
    return status;
}

naula_status naula_cache_other_anchors(const char *directory, const char *hex, const char *except, char ***anchors,
                                       size_t *count) {
    char **names = NULL;
    size_t name_count = 0;
    *anchors = NULL;
    *count = 0;
    naula_status status = naula_cache_file_names(directory, &names, &name_count);
    if (status != NAULA_OK) {
        return status;
    }
    for (size_t index = 0; index < name_count; index++) {
        const char *anchor = record_anchor(names[index], hex);
        if (anchor == NULL || strncmp(anchor, except, NAULA_CACHE_ANCHOR_SIZE - 1) == 0) {
            naula_memory_release(names[index]);
            continue;
        }
        memmove(names[index], anchor, NAULA_CACHE_ANCHOR_SIZE - 1);
        names[index][NAULA_CACHE_ANCHOR_SIZE - 1] = '\0';
        names[(*count)++] = names[index];
    }
    *anchors = names;
    return NAULA_OK;
}
