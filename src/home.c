#include "naula/home.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "naula/memory.h"

#define WORD_LIMIT 32u
#define ACCOUNT_LIMIT 100u
#define HOST_LIMIT 255u
#define URL_LIMIT 2048u
#define NUMBER_DIGITS_LIMIT 19u
#define PORT_DIGITS_LIMIT 5u
#define PLACE_TEXT_SIZE 2304
#define PLACE_HOST_SIZE 300

static const char *const HOME_FIX =
    "write the home as { \"provider\", \"account\" }, { \"provider\": \"web\", \"domain\" } or { \"url\": \"https://...\" }";
static const char *const PLACE_FIX =
    "write the place as <provider>:<number>, <provider>@<host>:<number>, web:<domain> or an https url";

static int is_lowercase_letter(char character) {
    return character >= 'a' && character <= 'z';
}

static int is_digit(char character) {
    return character >= '0' && character <= '9';
}

static int is_host_character(char character) {
    return is_lowercase_letter(character) || (character >= 'A' && character <= 'Z') || is_digit(character) ||
           character == '.' || character == '-';
}

static int is_word(const char *text, size_t length) {
    if (length == 0 || length > WORD_LIMIT || !is_lowercase_letter(text[0])) {
        return 0;
    }
    for (size_t index = 1; index < length; index++) {
        if (!is_lowercase_letter(text[index]) && !is_digit(text[index]) && text[index] != '-') {
            return 0;
        }
    }
    return 1;
}

static int is_account(const char *text, size_t length) {
    if (length == 0 || length > ACCOUNT_LIMIT || (length == 1 && text[0] == '.') ||
        (length == 2 && text[0] == '.' && text[1] == '.')) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        if (!is_host_character(text[index]) && text[index] != '_') {
            return 0;
        }
    }
    return 1;
}

static int is_domain(const char *text, size_t length) {
    if (length == 0 || length > HOST_LIMIT || text[0] == '.' || text[0] == '-') {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        if (!is_host_character(text[index])) {
            return 0;
        }
    }
    return 1;
}

static int is_port(const char *text, size_t length) {
    if (length == 0 || length > PORT_DIGITS_LIMIT) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        if (!is_digit(text[index])) {
            return 0;
        }
    }
    return 1;
}

static int is_host(const char *text, size_t length) {
    const char *colon = memchr(text, ':', length);
    if (colon == NULL) {
        return is_domain(text, length);
    }
    size_t host_length = (size_t)(colon - text);
    return is_domain(text, host_length) && is_port(colon + 1, length - host_length - 1);
}

static int starts_with(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static int is_git_url(const char *text) {
    size_t length = strlen(text);
    int has_scheme = starts_with(text, "https://") || starts_with(text, "ssh://");
    if (length > URL_LIMIT || !has_scheme || text[strstr(text, "://") - text + 3] == '\0') {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        if ((unsigned char)text[index] <= 0x20 || (unsigned char)text[index] >= 0x7F) {
            return 0;
        }
    }
    return 1;
}

static char *duplicate_range(const char *text, size_t length) {
    char *copy = naula_memory_allocate(length + 1);
    if (copy != NULL) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

static naula_status refuse_home(naula_refusal *refusal, const char *format, const char *subject) {
    naula_refuse(refusal, "invalid_home", HOME_FIX, format, subject);
    return NAULA_REFUSED;
}

static naula_status refuse_place(naula_refusal *refusal, const char *text, const char *reason) {
    naula_refuse(refusal, "invalid_place", PLACE_FIX, "\"%s\" %s", text, reason);
    return NAULA_REFUSED;
}

void naula_home_release(naula_home *home) {
    uint32_t size = home->size;
    naula_memory_release(home->provider);
    naula_memory_release(home->host);
    naula_memory_release(home->account);
    naula_memory_release(home->domain);
    naula_memory_release(home->url);
    memset(home, 0, sizeof *home);
    home->size = size;
}

void naula_place_release(naula_place *place) {
    uint32_t size = place->size;
    naula_memory_release(place->provider);
    naula_memory_release(place->host);
    naula_memory_release(place->domain);
    naula_memory_release(place->url);
    memset(place, 0, sizeof *place);
    place->size = size;
}

typedef struct home_members {
    const char *provider;
    const char *host;
    const char *account;
    const char *domain;
    const char *url;
} home_members;

static const char **member_slot(home_members *members, const char *name) {
    if (strcmp(name, "provider") == 0) {
        return &members->provider;
    }
    if (strcmp(name, "host") == 0) {
        return &members->host;
    }
    if (strcmp(name, "account") == 0) {
        return &members->account;
    }
    if (strcmp(name, "domain") == 0) {
        return &members->domain;
    }
    return strcmp(name, "url") == 0 ? &members->url : NULL;
}

static naula_status read_member(const cJSON *member, home_members *members, naula_refusal *refusal) {
    const char **slot = member_slot(members, member->string);
    if (slot == NULL) {
        return refuse_home(refusal, "a home has no member \"%s\"", member->string);
    }
    if (!cJSON_IsString(member) || member->valuestring == NULL) {
        return refuse_home(refusal, "the home's \"%s\" is not a string", member->string);
    }
    *slot = member->valuestring;
    return NAULA_OK;
}

static naula_status read_members(const cJSON *json, home_members *members, naula_refusal *refusal) {
    if (!cJSON_IsObject(json)) {
        return refuse_home(refusal, "a home is an object, not %s", cJSON_IsString(json) ? "a string" : "that");
    }
    memset(members, 0, sizeof *members);
    for (const cJSON *member = json->child; member != NULL; member = member->next) {
        naula_status status = read_member(member, members, refusal);
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

static naula_status check_url_home(const home_members *members, naula_refusal *refusal) {
    if (members->provider != NULL || members->host != NULL || members->account != NULL || members->domain != NULL) {
        return refuse_home(refusal, "a home with a url (%s) names nothing else", members->url);
    }
    if (!is_git_url(members->url)) {
        return refuse_home(refusal, "the url %s is not an https or ssh git url", members->url);
    }
    return NAULA_OK;
}

static naula_status check_web_home(const home_members *members, naula_refusal *refusal) {
    if (members->domain == NULL || members->account != NULL || members->host != NULL) {
        return refuse_home(refusal, "a %s home names a domain and nothing else", "web");
    }
    if (!is_domain(members->domain, strlen(members->domain))) {
        return refuse_home(refusal, "the domain %s is not a bare domain name", members->domain);
    }
    return NAULA_OK;
}

static naula_status check_provider_home(const home_members *members, naula_refusal *refusal) {
    if (!is_word(members->provider, strlen(members->provider))) {
        return refuse_home(refusal, "the provider %s is not a provider word", members->provider);
    }
    if (members->account == NULL || members->domain != NULL) {
        return refuse_home(refusal, "the %s home names an account and no domain", members->provider);
    }
    if (!is_account(members->account, strlen(members->account))) {
        return refuse_home(refusal, "the account %s is not an account name", members->account);
    }
    if (members->host != NULL && !is_host(members->host, strlen(members->host))) {
        return refuse_home(refusal, "the host %s is not a host name", members->host);
    }
    return NAULA_OK;
}

static naula_status check_home(const home_members *members, uint32_t *kind, naula_refusal *refusal) {
    if (members->url != NULL) {
        *kind = NAULA_HOME_URL;
        return check_url_home(members, refusal);
    }
    if (members->provider == NULL) {
        return refuse_home(refusal, "a home names a provider or a url, and %s does", "none");
    }
    if (strcmp(members->provider, "web") == 0) {
        *kind = NAULA_HOME_WEB;
        return check_web_home(members, refusal);
    }
    *kind = NAULA_HOME_PROVIDER;
    return check_provider_home(members, refusal);
}

static int copy_optional(const char *text, char **out) {
    if (text == NULL) {
        return 1;
    }
    *out = duplicate_range(text, strlen(text));
    return *out != NULL;
}

static void start_home(naula_home *home) {
    uint32_t size = home->size;
    memset(home, 0, sizeof *home);
    home->size = size;
}

naula_status naula_home_from_json(const cJSON *json, naula_home *home, naula_refusal *refusal) {
    if (home->size < offsetof(naula_home, url) + sizeof home->url) {
        return refuse_home(refusal, "the home struct is smaller than its first shape (%s)", "size");
    }
    start_home(home);
    home_members members;
    naula_status status = read_members(json, &members, refusal);
    if (status == NAULA_OK) {
        status = check_home(&members, &home->kind, refusal);
    }
    if (status != NAULA_OK) {
        return status;
    }
    if (!copy_optional(members.provider, &home->provider) || !copy_optional(members.host, &home->host) ||
        !copy_optional(members.account, &home->account) || !copy_optional(members.domain, &home->domain) ||
        !copy_optional(members.url, &home->url)) {
        naula_home_release(home);
        return NAULA_OUT_OF_MEMORY;
    }
    return NAULA_OK;
}

static naula_status read_number(const char *text, const char *digits, uint64_t *number, naula_refusal *refusal) {
    size_t length = strlen(digits);
    if (length == 0 || length > NUMBER_DIGITS_LIMIT) {
        return refuse_place(refusal, text, "carries no repository number");
    }
    uint64_t value = 0;
    for (size_t index = 0; index < length; index++) {
        if (!is_digit(digits[index])) {
            return refuse_place(refusal, text, "carries no repository number");
        }
        value = value * 10u + (uint64_t)(digits[index] - '0');
    }
    if (value == 0) {
        return refuse_place(refusal, text, "carries the number 0, which is no repository");
    }
    *number = value;
    return NAULA_OK;
}

typedef struct place_parts {
    const char *word;
    size_t word_length;
    const char *host;
    size_t host_length;
    const char *rest;
} place_parts;

/** A host may carry a port, so with an `@` the host ends at the last colon and the rest follows it. */
static int split_place(const char *text, place_parts *parts) {
    const char *colon = strchr(text, ':');
    const char *at = strchr(text, '@');
    if (colon == NULL) {
        return 0;
    }
    memset(parts, 0, sizeof *parts);
    parts->word = text;
    if (at == NULL || at > colon) {
        parts->word_length = (size_t)(colon - text);
        parts->rest = colon + 1;
        return 1;
    }
    const char *last_colon = strrchr(at, ':');
    if (last_colon == NULL) {
        return 0;
    }
    parts->word_length = (size_t)(at - text);
    parts->host = at + 1;
    parts->host_length = (size_t)(last_colon - at - 1);
    parts->rest = last_colon + 1;
    return 1;
}

static naula_status fill_web_place(const char *text, const place_parts *parts, naula_place *place,
                                   naula_refusal *refusal) {
    if (parts->host != NULL || !is_domain(parts->rest, strlen(parts->rest))) {
        return refuse_place(refusal, text, "does not name a bare web domain");
    }
    place->kind = NAULA_HOME_WEB;
    place->domain = duplicate_range(parts->rest, strlen(parts->rest));
    return place->domain == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}

static naula_status fill_numbered_place(const char *text, const place_parts *parts, naula_place *place,
                                        naula_refusal *refusal) {
    naula_status status = read_number(text, parts->rest, &place->number, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    if (parts->host != NULL && !is_host(parts->host, parts->host_length)) {
        return refuse_place(refusal, text, "names a host that is not a host name");
    }
    place->kind = NAULA_HOME_PROVIDER;
    place->provider = duplicate_range(parts->word, parts->word_length);
    place->host = parts->host == NULL ? NULL : duplicate_range(parts->host, parts->host_length);
    return place->provider == NULL || (parts->host != NULL && place->host == NULL) ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}

static naula_status fill_url_place(const char *text, naula_place *place, naula_refusal *refusal) {
    if (!is_git_url(text)) {
        return refuse_place(refusal, text, "is not an https or ssh git url");
    }
    place->kind = NAULA_HOME_URL;
    place->url = duplicate_range(text, strlen(text));
    return place->url == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}

static naula_status fill_place(const char *text, naula_place *place, naula_refusal *refusal) {
    place_parts parts;
    if (strstr(text, "://") != NULL) {
        return fill_url_place(text, place, refusal);
    }
    if (!split_place(text, &parts) || !is_word(parts.word, parts.word_length)) {
        return refuse_place(refusal, text, "is not <provider>:<number>, web:<domain> or a git url");
    }
    int is_web = parts.word_length == 3 && strncmp(parts.word, "web", 3) == 0;
    return is_web ? fill_web_place(text, &parts, place, refusal) : fill_numbered_place(text, &parts, place, refusal);
}

naula_status naula_place_parse(const char *text, naula_place *place, naula_refusal *refusal) {
    if (place->size < offsetof(naula_place, url) + sizeof place->url) {
        return refuse_place(refusal, "", "is read into a place struct smaller than its first shape");
    }
    uint32_t size = place->size;
    memset(place, 0, sizeof *place);
    place->size = size;
    naula_status status = fill_place(text, place, refusal);
    if (status != NAULA_OK) {
        naula_place_release(place);
    }
    return status;
}

static void lowercase(char *text) {
    for (; *text != '\0'; text++) {
        if (*text >= 'A' && *text <= 'Z') {
            *text = (char)(*text + ('a' - 'A'));
        }
    }
}

char *naula_place_text(const naula_place *place) {
    char text[PLACE_TEXT_SIZE];
    char host[PLACE_HOST_SIZE];
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
