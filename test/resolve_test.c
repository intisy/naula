#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#include <sys/utime.h>
#else
#include <dirent.h>
#include <utime.h>
#endif

#include "cache_internal.h"
#include "check.h"
#include "http_fixture.h"
#include "memory_testing.h"
#include "naula/json.h"
#include "naula/memory.h"
#include "naula/provider.h"
#include "naula/resolve.h"
#include "naula/session.h"
#include "provider_github_hooks.h"
#include "provider_web_hooks.h"
#include "transport_hooks.h"

#ifndef NAULA_TEST_SCRATCH
#error "NAULA_TEST_SCRATCH names the folder a test may write into"
#endif

#define X "dd92836cca7c292258635112ecfe1acb7b4f6ecd"
#define X256 "5e3d9c4b2a1f0e9d8c7b6a5f4e3d2c1b0a9f8e7d6c5b4a3f2e1d0c9b8a7f6e5d"
#define ELSEWHERE "0123456789abcdef0123456789abcdef01234567"
#define ID "git:" X
#define TOKEN "fake-token-for-the-forge"
#define HOME(account) "{\"provider\":\"forge\",\"account\":\"" account "\"}"
#define HOME_AT(host, account) "{\"provider\":\"forge\",\"host\":\"" host "\",\"account\":\"" account "\"}"
#define SSH_URL "ssh://git@example.org/alpha/lib.git"

#define FAKE_REPO_LIMIT 96
#define FAKE_TEXT_LIMIT 24
#define FAKE_TEXT_SIZE 4096
#define FAKE_LOG_SIZE 65536

typedef struct fake_repo {
    const char *host;
    uint64_t number;
    const char *account;
    const char *name;
    const char *default_branch;
    const char *development;
    const char *main_file;
    const char *holds;
    size_t parents;
    uint64_t fork_parent;
    int gone;
    int times_out;
    int find_times_out;
    int unreachable;
    int development_without_file;
} fake_repo;

typedef struct fake_forge {
    fake_repo repos[FAKE_REPO_LIMIT];
    size_t count;
    char texts[FAKE_TEXT_LIMIT][FAKE_TEXT_SIZE];
    size_t text_count;
    char log[FAKE_LOG_SIZE];
    size_t log_length;
    int wrong_tokens;
    int missing_times_out;
} fake_forge;

static fake_forge forge;
static naula_providers *registry;
static int accept_answer;
static int accept_calls;
static uint64_t accepted_was;
static uint64_t accepted_becomes;

static void forge_reset(void) {
    memset(&forge, 0, sizeof forge);
}

static void note(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(forge.log + forge.log_length, sizeof forge.log - forge.log_length, format, arguments);
    va_end(arguments);
    if (written > 0 && forge.log_length + (size_t)written + 1 < sizeof forge.log) {
        forge.log_length += (size_t)written;
        forge.log[forge.log_length++] = '\n';
        forge.log[forge.log_length] = '\0';
    }
}

static void forget_log(void) {
    forge.log[0] = '\0';
    forge.log_length = 0;
}

static fake_repo *add_repo(const char *host, uint64_t number, const char *account, const char *name,
                           const char *file) {
    fake_repo *repo = &forge.repos[forge.count++];
    memset(repo, 0, sizeof *repo);
    repo->host = host;
    repo->number = number;
    repo->account = account;
    repo->name = name;
    repo->default_branch = "main";
    repo->development = file;
    repo->holds = X;
    return repo;
}

static const char *repo_file(const char *name, const char *homes, const char *stated_id) {
    char *text = forge.texts[forge.text_count++];
    if (stated_id == NULL) {
        snprintf(text, FAKE_TEXT_SIZE, "{\"format\":2,\"name\":\"%s\",\"importName\":\"%s\",\"homes\":[%s]}",
                 name, name, homes);
    } else {
        snprintf(text, FAKE_TEXT_SIZE,
                 "{\"format\":2,\"name\":\"%s\",\"importName\":\"%s\",\"homes\":[%s],\"id\":\"%s\"}", name, name,
                 homes, stated_id);
    }
    return text;
}

static int same_host(const char *left, const char *right) {
    if (left == NULL || right == NULL) {
        return left == right;
    }
    return strcmp(left, right) == 0;
}

static fake_repo *by_number(const char *host, uint64_t number) {
    for (size_t index = 0; index < forge.count; index++) {
        if (same_host(forge.repos[index].host, host) && forge.repos[index].number == number) {
            return &forge.repos[index];
        }
    }
    return NULL;
}

static naula_status fake_refuse(naula_refusal *refusal, naula_status status, const char *code) {
    naula_refuse(refusal, code, "fix the fake forge", "the fake forge answered %s", code);
    return status;
}

static naula_status reach(const fake_repo *repo, naula_refusal *refusal) {
    if (repo == NULL && forge.missing_times_out) {
        return fake_refuse(refusal, NAULA_TIMED_OUT, "timed_out");
    }
    if (repo == NULL || repo->gone) {
        return fake_refuse(refusal, NAULA_NOT_FOUND, "not_found");
    }
    if (repo->times_out) {
        return fake_refuse(refusal, NAULA_TIMED_OUT, "timed_out");
    }
    if (repo->unreachable) {
        return fake_refuse(refusal, NAULA_UNREACHABLE, "host_not_resolved");
    }
    return NAULA_OK;
}

static int has_development_branch(const fake_repo *repo) {
    return repo->development != NULL || repo->development_without_file;
}

static void check_token(const char *token) {
    if (token == NULL || strcmp(token, TOKEN) != 0) {
        forge.wrong_tokens++;
    }
}

static const char *host_text(const char *host) {
    return host == NULL ? "-" : host;
}

static char *copy_text(const char *text) {
    return text == NULL ? NULL : naula_memory_duplicate_text(text);
}

static naula_status fake_describe(void *context, uint32_t *supported, int *anonymous_lookup) {
    (void)context;
    *supported = NAULA_OP_LOOKUP | NAULA_OP_FIND | NAULA_OP_READ_FILE | NAULA_OP_CONTAINS | NAULA_OP_PARENT_COUNT;
    *anonymous_lookup = 1;
    return NAULA_OK;
}

static naula_status fake_lookup(void *context, const char *host, uint64_t number, const char *token,
                                naula_repository *out, naula_refusal *refusal) {
    (void)context;
    note("lookup %s:%llu", host_text(host), (unsigned long long)number);
    check_token(token);
    fake_repo *repo = by_number(host, number);
    naula_status status = reach(repo, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    uint32_t size = out->size;
    memset(out, 0, sizeof *out);
    out->size = size;
    out->number = number;
    out->fork_parent = repo->fork_parent;
    out->account = copy_text(repo->account);
    out->name = copy_text(repo->name);
    out->default_branch = copy_text(repo->default_branch);
    if (out->account == NULL || out->name == NULL || out->default_branch == NULL) {
        naula_repository_release(out);
        return NAULA_OUT_OF_MEMORY;
    }
    return NAULA_OK;
}

static naula_status fake_find(void *context, const char *host, const char *account, const char *name,
                              const char *token, uint64_t *number, naula_refusal *refusal) {
    (void)context;
    note("find %s:%s/%s", host_text(host), account, name);
    check_token(token);
    *number = 0;
    for (size_t index = 0; index < forge.count; index++) {
        const fake_repo *repo = &forge.repos[index];
        if (same_host(repo->host, host) && strcmp(repo->account, account) == 0 && strcmp(repo->name, name) == 0) {
            if (repo->find_times_out) {
                return fake_refuse(refusal, NAULA_TIMED_OUT, "timed_out");
            }
            if (repo->unreachable) {
                return fake_refuse(refusal, NAULA_UNREACHABLE, "host_not_resolved");
            }
            if (!repo->gone) {
                *number = repo->number;
            }
        }
    }
    return NAULA_OK;
}

static naula_status fake_read_file(void *context, const char *host, uint64_t number, const char *ref,
                                   const char *path, const char *token, uint8_t **bytes, size_t *length,
                                   naula_refusal *refusal) {
    (void)context;
    note("read %s:%llu %s", host_text(host), (unsigned long long)number, ref);
    check_token(token);
    *bytes = NULL;
    *length = 0;
    fake_repo *repo = by_number(host, number);
    naula_status status = reach(repo, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    const char *text = NULL;
    if (strcmp(ref, "development") == 0) {
        text = repo->development;
    } else if (strcmp(ref, repo->default_branch) == 0) {
        text = repo->main_file;
    }
    if (text == NULL || strcmp(path, "repo.json") != 0) {
        return fake_refuse(refusal, NAULA_REFUSED, "missing_file_or_ref");
    }
    *length = strlen(text);
    *bytes = naula_memory_allocate(*length + 1);
    if (*bytes == NULL) {
        *length = 0;
        return NAULA_OUT_OF_MEMORY;
    }
    memcpy(*bytes, text, *length);
    return NAULA_OK;
}

static naula_status fake_contains(void *context, const char *host, uint64_t number, const char *ref,
                                  const char *commit, const char *token, int *contained, naula_refusal *refusal) {
    (void)context;
    note("contains %s:%llu %s", host_text(host), (unsigned long long)number, ref);
    check_token(token);
    *contained = 0;
    fake_repo *repo = by_number(host, number);
    naula_status status = reach(repo, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    if (repo->holds == NULL || (strcmp(ref, "development") == 0 && !has_development_branch(repo))) {
        return fake_refuse(refusal, NAULA_REFUSED, "missing_commit_or_ref");
    }
    *contained = strcmp(repo->holds, commit) == 0;
    return NAULA_OK;
}

static naula_status fake_parent_count(void *context, const char *host, uint64_t number, const char *commit,
                                      const char *token, size_t *count, naula_refusal *refusal) {
    (void)context;
    note("parents %s:%llu", host_text(host), (unsigned long long)number);
    check_token(token);
    *count = 0;
    fake_repo *repo = by_number(host, number);
    naula_status status = reach(repo, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    if (repo->holds == NULL || strcmp(repo->holds, commit) != 0) {
        return fake_refuse(refusal, NAULA_REFUSED, "missing_commit");
    }
    *count = repo->parents;
    return NAULA_OK;
}

static naula_status fake_list_repos(void *context, const char *host, const char *account, const char *token,
                                    naula_repository **out, size_t *count, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)token;
    note("list %s", account);
    *out = NULL;
    *count = 0;
    return fake_refuse(refusal, NAULA_REFUSED, "unsupported_operation");
}

static naula_status fake_release_index(void *context, const char *host, uint64_t number, const char *tag,
                                       const char *token, uint8_t **bytes, size_t *length, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)number;
    (void)tag;
    (void)token;
    *bytes = NULL;
    *length = 0;
    return fake_refuse(refusal, NAULA_REFUSED, "unsupported_operation");
}

/** A reader only: its size ends at the first shape, so the registry asks it for no writes. */
static const naula_provider fake_provider = {.size = (uint32_t)(offsetof(naula_provider, release_index) +
                                                                sizeof(((const naula_provider *)0)->release_index)),
                                             .word = "forge",
                                             .describe = fake_describe,
                                             .lookup = fake_lookup,
                                             .find = fake_find,
                                             .read_file = fake_read_file,
                                             .contains = fake_contains,
                                             .parent_count = fake_parent_count,
                                             .list_repos = fake_list_repos,
                                             .release_index = fake_release_index};

static const char *fake_token(void *context, const char *provider, const char *host) {
    (void)context;
    (void)host;
    return strcmp(provider, "forge") == 0 ? TOKEN : NULL;
}

static int fake_accept(void *context, const char *id, const naula_place *was, const naula_place *becomes) {
    (void)context;
    accept_calls++;
    CHECK(strncmp(id, "git:", 4) == 0);
    accepted_was = was->number;
    accepted_becomes = becomes->number;
    return accept_answer;
}

static void cache_directory(const char *test, char *out, size_t size) {
    snprintf(out, size, "%s/resolve-cache-%s", NAULA_TEST_SCRATCH, test);
}

#if defined(_WIN32)
/** The long path form, since a record named for a 64 hex id under the build tree passes 260 characters. */
static void long_path(const char *path, wchar_t *out, size_t size) {
    wchar_t plain[4096];
    wchar_t full[4096];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, plain, 4096);
    for (wchar_t *at = plain; *at != L'\0'; at++) {
        *at = *at == L'/' ? L'\\' : *at;
    }
    GetFullPathNameW(plain, 4096, full, NULL);
    swprintf(out, size, L"\\\\?\\%ls", full);
}

static FILE *open_file(const char *path, const char *mode) {
    wchar_t wide[4200];
    wchar_t wide_mode[8];
    long_path(path, wide, sizeof wide / sizeof wide[0]);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wide_mode, 8);
    return _wfopen(wide, wide_mode);
}

static void remove_file(const char *path) {
    wchar_t wide[4200];
    long_path(path, wide, sizeof wide / sizeof wide[0]);
    DeleteFileW(wide);
}
#else
static FILE *open_file(const char *path, const char *mode) {
    return fopen(path, mode);
}

static void remove_file(const char *path) {
    remove(path);
}
#endif

/** Removes every record in the cache, whichever anchor wrote it. */
static void forget_cache(const char *directory) {
    char folder[1024];
    char path[2048];
    snprintf(folder, sizeof folder, "%s/identities", directory);
#if defined(_WIN32)
    char pattern[1100];
    WIN32_FIND_DATAA found;
    snprintf(pattern, sizeof pattern, "%s/*", folder);
    HANDLE search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            snprintf(path, sizeof path, "%s/%s", folder, found.cFileName);
            remove_file(path);
        }
    } while (FindNextFileA(search, &found));
    FindClose(search);
#else
    DIR *opened = opendir(folder);
    if (opened == NULL) {
        return;
    }
    for (struct dirent *entry = readdir(opened); entry != NULL; entry = readdir(opened)) {
        if (entry->d_name[0] != '.') {
            snprintf(path, sizeof path, "%s/%s", folder, entry->d_name);
            remove_file(path);
        }
    }
    closedir(opened);
#endif
}

/** The record a consumer whose `at` is the one place `at` writes for `hex`. */
static void record_path(const char *directory, const char *hex, const char *at, char *out, size_t size) {
    naula_place place;
    char anchor[NAULA_CACHE_ANCHOR_SIZE];
    memset(&place, 0, sizeof place);
    place.size = sizeof place;
    CHECK(naula_place_parse(at, &place, NULL) == NAULA_OK);
    CHECK(naula_cache_anchor(&place, 1, anchor) == NAULA_OK);
    naula_place_release(&place);
    snprintf(out, size, "%s/identities/%s-%s.json", directory, hex, anchor);
}

static naula_session *open_session(const char *cache, int offline, const naula_history *history) {
    naula_session_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.cache_directory = cache;
    options.providers = registry;
    options.history = history;
    options.token_for = fake_token;
    options.accept_primary = fake_accept;
    options.offline = offline;
    naula_session *session = NULL;
    CHECK(naula_session_open(&options, &session, NULL) == NAULA_OK);
    return session;
}

static naula_refusal fresh_refusal(void) {
    naula_refusal refusal;
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    return refusal;
}

static void start_resolution(naula_resolution *resolution) {
    memset(resolution, 0, sizeof *resolution);
    resolution->size = sizeof *resolution;
}

/** Resolves `id` from up to two places of `at`, the second NULL for one. */
static naula_status resolve_from(naula_session *session, const char *id, const char *first, const char *second,
                                 naula_resolution *resolution, naula_refusal *refusal) {
    naula_place places[2];
    const char *texts[2] = {first, second};
    size_t count = 0;
    for (size_t index = 0; index < 2 && texts[index] != NULL; index++) {
        memset(&places[index], 0, sizeof places[index]);
        places[index].size = sizeof places[index];
        CHECK(naula_place_parse(texts[index], &places[index], NULL) == NAULA_OK);
        count++;
    }
    naula_reference reference;
    memset(&reference, 0, sizeof reference);
    reference.size = sizeof reference;
    reference.id = (char *)id;
    reference.at = places;
    reference.at_count = count;
    start_resolution(resolution);
    *refusal = fresh_refusal();
    naula_status status = naula_resolve(session, &reference, resolution, refusal);
    for (size_t index = 0; index < count; index++) {
        naula_place_release(&places[index]);
    }
    return status;
}

static naula_status resolve_once(const char *cache, const char *id, const char *first, const char *second,
                                 naula_resolution *resolution, naula_refusal *refusal) {
    naula_session *session = open_session(cache, 0, NULL);
    naula_status status = resolve_from(session, id, first, second, resolution, refusal);
    naula_session_close(session);
    return status;
}

static const naula_verdict *verdict_for(const naula_resolution *resolution, const char *host, uint64_t number) {
    for (size_t index = 0; index < resolution->verdict_count; index++) {
        const naula_place *home = &resolution->verdicts[index].home;
        if (home->kind == NAULA_HOME_PROVIDER && home->number == number && same_host(home->host, host)) {
            return &resolution->verdicts[index];
        }
    }
    return NULL;
}

static int verdict_is(const naula_resolution *resolution, const char *host, uint64_t number, const char *code,
                      int confirmed) {
    const naula_verdict *verdict = verdict_for(resolution, host, number);
    if (verdict == NULL) {
        fprintf(stderr, "no verdict for %s:%llu\n", host_text(host), (unsigned long long)number);
        return 0;
    }
    if (strcmp(verdict->code, code) != 0 || verdict->confirmed != confirmed) {
        fprintf(stderr, "verdict for %s:%llu is %s (%d, %s), expected %s (%d)\n", host_text(host),
                (unsigned long long)number, verdict->code, verdict->confirmed, verdict->detail, code, confirmed);
        return 0;
    }
    return 1;
}

static int has_home(const naula_resolution *resolution, const char *host, uint64_t number) {
    for (size_t index = 0; index < resolution->home_count; index++) {
        if (resolution->homes[index].number == number && same_host(resolution->homes[index].host, host)) {
            return 1;
        }
    }
    return 0;
}

static uint64_t primary_number(const naula_resolution *resolution) {
    return resolution->primary < resolution->home_count ? resolution->homes[resolution->primary].number : 0;
}

static char *read_text_file(const char *path) {
    FILE *file = open_file(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    char *text = naula_memory_allocate(65536);
    size_t length = text == NULL ? 0 : fread(text, 1, 65535, file);
    if (text != NULL) {
        text[length] = '\0';
    }
    fclose(file);
    return text;
}

static void write_text_file(const char *path, const char *text) {
    FILE *file = open_file(path, "wb");
    CHECK(file != NULL);
    if (file != NULL) {
        fwrite(text, 1, strlen(text), file);
        fclose(file);
    }
}

static void confirms_a_single_home_from_at(void) {
    char cache[512];
    char path[1024];
    cache_directory("single", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_CONFIRMED && resolution.home_count == 1 && primary_number(&resolution) == 10);
    check_text(resolution.file.name, "lib", "the primary's file");
    check_text(resolution.primary_repository.account, "alpha", "the primary's account");
    CHECK(verdict_is(&resolution, NULL, 10, "confirmed", 1));
    CHECK(resolution.cache_problem[0] == '\0');
    CHECK(forge.wrong_tokens == 0);
    naula_resolution_release(&resolution);
    record_path(cache, X, "forge:10", path, sizeof path);
    char *cached = read_text_file(path);
    CHECK(cached != NULL && strstr(cached, "\"number\":10") != NULL && strstr(cached, "\"alpha\"") != NULL);
    naula_memory_release(cached);
}

static void confirms_a_second_home_listed_both_ways(void) {
    forge_reset();
    const char *both = repo_file("lib", HOME("alpha") "," HOME_AT("second.example", "beta"), NULL);
    add_repo(NULL, 10, "alpha", "lib", both);
    add_repo("second.example", 20, "beta", "lib", both);
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(NULL, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_CONFIRMED && resolution.home_count == 2);
    CHECK(has_home(&resolution, NULL, 10) && has_home(&resolution, "second.example", 20));
    CHECK(primary_number(&resolution) == 10);
    CHECK(verdict_is(&resolution, "second.example", 20, "confirmed", 1));
    CHECK(strstr(forge.log, "find second.example:beta/lib") != NULL);
    naula_resolution_release(&resolution);
}

static void refuses_a_listed_home_that_does_not_list_back(void) {
    forge_reset();
    add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha") "," HOME("beta"), NULL));
    add_repo(NULL, 20, "beta", "lib", repo_file("lib", HOME("beta"), NULL));
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(NULL, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "disowned-by", "refusal code");
    CHECK(strstr(refusal.detail, "forge:20") != NULL);
    CHECK(resolution.home_count == 0 && resolution.state == 0);
    CHECK(verdict_is(&resolution, NULL, 10, "disowned-by", 0));
    naula_resolution_release(&resolution);
}

static void refuses_a_hostile_copy_listing_the_original(void) {
    forge_reset();
    add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    add_repo(NULL, 66, "mallory", "lib", repo_file("lib", HOME("alpha") "," HOME("mallory"), NULL));
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(NULL, ID, "forge:66", NULL, &resolution, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "disowned-by", "a copy pointed at directly");
    CHECK(strstr(refusal.detail, "forge:10") != NULL);
    CHECK(resolution.home_count == 0);
    naula_resolution_release(&resolution);

    CHECK(resolve_once(NULL, ID, "forge:10", "forge:66", &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 1 && has_home(&resolution, NULL, 10) && !has_home(&resolution, NULL, 66));
    CHECK(verdict_is(&resolution, NULL, 66, "disowned-by", 0));
    naula_resolution_release(&resolution);
}

static void never_reaches_a_self_listing_copy_from_at(void) {
    char cache[512];
    cache_directory("self-listing", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    fake_repo *original = add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    add_repo(NULL, 66, "mallory", "lib", repo_file("lib", HOME("mallory"), NULL));
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 1 && has_home(&resolution, NULL, 10));
    CHECK(strstr(forge.log, ":66") == NULL && strstr(forge.log, "mallory") == NULL);
    naula_resolution_release(&resolution);

    original->account = "beta";
    original->development = repo_file("lib", HOME("beta"), NULL);
    add_repo(NULL, 77, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    forget_log();
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 1 && has_home(&resolution, NULL, 10));
    check_text(resolution.primary_repository.account, "beta", "the transferred home");
    if (strstr(forge.log, ":77") != NULL || strstr(forge.log, "alpha") != NULL) {
        fprintf(stderr, "the copy at the old account was reached:\n%s", forge.log);
        check_failures++;
    }
    naula_resolution_release(&resolution);
}

static void rename_keeps_resolving_by_number_and_warns_pending(void) {
    forge_reset();
    add_repo(NULL, 10, "alpha", "old-lib", repo_file("lib", HOME("alpha"), NULL));
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(NULL, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 1 && verdict_is(&resolution, NULL, 10, "rename-pending", 1));
    check_text(resolution.file.name, "lib", "the name the file wants");
    check_text(resolution.primary_repository.name, "old-lib", "the name the host still has");
    naula_resolution_release(&resolution);
}

static void planned_move_follows_old_to_new_and_caches_it(void) {
    char cache[512];
    cache_directory("planned-move", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    const char *moved = repo_file("lib", HOME_AT("new.example", "alpha") "," HOME("alpha"), NULL);
    add_repo(NULL, 10, "alpha", "lib", moved);
    add_repo("new.example", 20, "alpha", "lib", moved);
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 2 && primary_number(&resolution) == 20);
    check_text(resolution.homes[resolution.primary].host, "new.example", "the new primary's host");
    naula_resolution_release(&resolution);

    naula_session *offline = open_session(cache, 1, NULL);
    forget_log();
    CHECK(resolve_from(offline, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_STALE && primary_number(&resolution) == 20 && resolution.home_count == 2);
    CHECK(forge.log_length == 0);
    naula_resolution_release(&resolution);
    naula_session_close(offline);
}

static void follows_primary_pointers_and_refuses_a_cycle(void) {
    forge_reset();
    add_repo(NULL, 10, "alpha", "lib",
             repo_file("lib", HOME_AT("two.example", "alpha") "," HOME("alpha") "," HOME_AT("three.example", "alpha"),
                       NULL));
    const char *ahead =
        repo_file("lib", HOME_AT("three.example", "alpha") "," HOME("alpha") "," HOME_AT("two.example", "alpha"), NULL);
    add_repo("two.example", 20, "alpha", "lib", ahead);
    add_repo("three.example", 30, "alpha", "lib", ahead);
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(NULL, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 3 && primary_number(&resolution) == 30);
    naula_resolution_release(&resolution);

    forge_reset();
    add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME_AT("two.example", "alpha") "," HOME("alpha"), NULL));
    add_repo("two.example", 20, "alpha", "lib",
             repo_file("lib", HOME("alpha") "," HOME_AT("two.example", "alpha"), NULL));
    CHECK(resolve_once(NULL, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "cycle", "pointers that loop");
    CHECK(resolution.home_count == 0);
    naula_resolution_release(&resolution);
}

static void refuses_a_home_whose_number_changed_since_the_cache(void) {
    char cache[512];
    cache_directory("number-changed", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    const char *both = repo_file("lib", HOME("alpha") "," HOME_AT("second.example", "bee"), NULL);
    add_repo(NULL, 10, "alpha", "lib", both);
    fake_repo *second = add_repo("second.example", 20, "bee", "lib", both);
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 2);
    naula_resolution_release(&resolution);

    second->gone = 1;
    add_repo("second.example", 21, "bee", "lib", both);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 1 && has_home(&resolution, NULL, 10));
    CHECK(!has_home(&resolution, "second.example", 21) && !has_home(&resolution, "second.example", 20));
    CHECK(verdict_is(&resolution, "second.example", 21, "number-changed", 0));
    CHECK(verdict_is(&resolution, "second.example", 20, "dead-skipped", 0));
    naula_resolution_release(&resolution);

    for (int run = 3; run <= 4; run++) {
        forget_log();
        CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
        if (has_home(&resolution, "second.example", 21) || resolution.home_count != 1) {
            fprintf(stderr, "run %d: the re-registered account's repository 21 became a home\n", run);
            check_failures++;
        }
        CHECK(verdict_is(&resolution, "second.example", 21, "number-changed", 0));
        CHECK(verdict_is(&resolution, "second.example", 20, "dead-skipped", 0));
        CHECK(strstr(forge.log, "lookup second.example:20") != NULL);
        naula_resolution_release(&resolution);
    }
}

static void resolve_treats_a_timeout_as_unknown_not_dead(void) {
    char cache[512];
    cache_directory("timeout", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    fake_repo *original = add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    add_repo(NULL, 66, "mallory", "lib", repo_file("lib", HOME("mallory") "," HOME("alpha"), NULL));
    original->times_out = 1;
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(NULL, ID, "forge:66", NULL, &resolution, &refusal) == NAULA_TIMED_OUT);
    check_text(refusal.code, "timed-out", "a copy whose original only times out");
    CHECK(resolution.home_count == 0 && verdict_is(&resolution, NULL, 66, "timed-out", 0));
    naula_resolution_release(&resolution);

    original->times_out = 0;
    original->find_times_out = 1;
    CHECK(resolve_once(NULL, ID, "forge:66", NULL, &resolution, &refusal) == NAULA_TIMED_OUT);
    CHECK(resolution.home_count == 0);
    naula_resolution_release(&resolution);

    original->find_times_out = 0;
    original->times_out = 1;
    CHECK(resolve_once(NULL, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_TIMED_OUT);
    naula_resolution_release(&resolution);

    original->times_out = 0;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    original->times_out = 1;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_STALE && has_home(&resolution, NULL, 10));
    naula_resolution_release(&resolution);
    naula_session *offline = open_session(cache, 1, NULL);
    CHECK(resolve_from(offline, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_STALE && has_home(&resolution, NULL, 10));
    naula_resolution_release(&resolution);
    naula_session_close(offline);
}

static void skips_a_dead_listed_home_and_reports_it(void) {
    char cache[512];
    cache_directory("dead-listed", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    const char *both = repo_file("lib", HOME("alpha") "," HOME_AT("second.example", "delta"), NULL);
    add_repo(NULL, 10, "alpha", "lib", both);
    fake_repo *second = add_repo("second.example", 20, "delta", "lib", both);
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 2);
    naula_resolution_release(&resolution);

    second->gone = 1;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_CONFIRMED && resolution.home_count == 1 && has_home(&resolution, NULL, 10));
    CHECK(verdict_is(&resolution, "second.example", 20, "dead-skipped", 0));
    naula_resolution_release(&resolution);

    forget_log();
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_CONFIRMED && resolution.home_count == 1);
    CHECK(verdict_is(&resolution, "second.example", 20, "dead-skipped", 0));
    CHECK(strstr(forge.log, "lookup second.example:20") != NULL);
    naula_resolution_release(&resolution);

    naula_status status = resolve_once(NULL, ID, "forge:10", NULL, &resolution, &refusal);
    CHECK(status == NAULA_REFUSED);
    check_text(refusal.code, "unverifiable", "a listed home missing by name and never seen by number");
    naula_resolution_release(&resolution);
}

static void ssh_home_is_unverifiable_never_confirmed(void) {
    forge_reset();
    add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha") ",{\"url\":\"" SSH_URL "\"}", NULL));
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(NULL, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 1 && has_home(&resolution, NULL, 10));
    int found = 0;
    for (size_t index = 0; index < resolution.verdict_count; index++) {
        const naula_verdict *verdict = &resolution.verdicts[index];
        if (verdict->home.kind == NAULA_HOME_URL) {
            found = 1;
            CHECK(verdict->confirmed == 0 && strcmp(verdict->code, "unverifiable") == 0);
            check_text(verdict->home.url, SSH_URL, "the ssh home");
        }
    }
    CHECK(found);
    naula_resolution_release(&resolution);

    CHECK(resolve_once(NULL, ID, SSH_URL, NULL, &resolution, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "unverifiable", "an ssh home alone");
    CHECK(resolution.home_count == 0);
    naula_resolution_release(&resolution);
}

static void answers_stale_from_cache_offline(void) {
    char cache[512];
    cache_directory("stale", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);

    naula_session *offline = open_session(cache, 1, NULL);
    forget_log();
    CHECK(resolve_from(offline, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_STALE && resolution.home_count == 1 && primary_number(&resolution) == 10);
    CHECK(resolution.file.name == NULL && forge.log_length == 0);
    naula_resolution_release(&resolution);
    naula_session_close(offline);

    char empty[512];
    cache_directory("stale-empty", empty, sizeof empty);
    forget_cache(empty);
    offline = open_session(empty, 1, NULL);
    CHECK(resolve_from(offline, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_UNREACHABLE);
    check_text(refusal.code, "unresolvable", "offline with nothing cached");
    naula_resolution_release(&resolution);
    naula_session_close(offline);
}

static void primary_change_needs_accept(void) {
    char cache[512];
    cache_directory("primary-change", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    fake_repo *first = add_repo(NULL, 10, "alpha", "lib",
                                repo_file("lib", HOME("alpha") "," HOME_AT("second.example", "beta"), NULL));
    fake_repo *second = add_repo("second.example", 20, "beta", "lib", first->development);
    naula_resolution resolution;
    naula_refusal refusal;
    accept_calls = 0;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(primary_number(&resolution) == 10 && accept_calls == 0);
    naula_resolution_release(&resolution);

    const char *swapped = repo_file("lib", HOME_AT("second.example", "beta") "," HOME("alpha"), NULL);
    first->development = swapped;
    second->development = swapped;
    accept_answer = 0;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "primary-changed", "a new primary refused");
    CHECK(accept_calls == 1 && accepted_was == 10 && accepted_becomes == 20);
    naula_resolution_release(&resolution);

    accept_answer = 1;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(primary_number(&resolution) == 20 && accept_calls == 2);
    naula_resolution_release(&resolution);

    accept_answer = 0;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(primary_number(&resolution) == 20 && accept_calls == 2);
    naula_resolution_release(&resolution);
}

static void fork_exception_confirms_a_stated_non_root_id(void) {
    forge_reset();
    fake_repo *stated = add_repo(NULL, 10, "alpha", "inventree", repo_file("inventree", HOME("alpha"), ID));
    stated->parents = 1;
    fake_repo *unstated = add_repo(NULL, 20, "beta", "shelf", repo_file("shelf", HOME("beta"), NULL));
    unstated->parents = 1;
    add_repo(NULL, 30, "gamma", "other", repo_file("other", HOME("gamma"), "git:" ELSEWHERE));
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(NULL, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 1 && verdict_is(&resolution, NULL, 10, "confirmed", 1));
    naula_resolution_release(&resolution);

    CHECK(resolve_once(NULL, ID, "forge:20", NULL, &resolution, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "has-parents", "a non-root commit not stated");
    naula_resolution_release(&resolution);

    CHECK(resolve_once(NULL, ID, "forge:30", NULL, &resolution, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "states-another-id", "a repo stating its own id");
    naula_resolution_release(&resolution);
}

static void sha256_id_resolves(void) {
    char cache[512];
    char path[1024];
    cache_directory("sha256", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    fake_repo *repo = add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    repo->holds = X256;
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(cache, "git:" X256, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 1);
    naula_resolution_release(&resolution);
    record_path(cache, X256, "forge:10", path, sizeof path);
    char *cached = read_text_file(path);
    CHECK(cached != NULL && strstr(cached, "git:" X256) != NULL);
    naula_memory_release(cached);
}

static void reads_repo_json_from_the_default_branch_without_development(void) {
    forge_reset();
    fake_repo *repo = add_repo(NULL, 10, "alpha", "lib", NULL);
    repo->main_file = repo_file("lib", HOME("alpha"), NULL);
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(NULL, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 1);
    CHECK(strstr(forge.log, "contains -:10 main") != NULL);
    naula_resolution_release(&resolution);

    repo->main_file = NULL;
    CHECK(resolve_once(NULL, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "not-listed", "a live repo with no repo.json is refused, never gone");
    naula_resolution_release(&resolution);
}

static void refuses_a_local_id_and_a_url_reference(void) {
    forge_reset();
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(NULL, "local:tools", "forge:10", NULL, &resolution, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "unresolvable", "a local id");
    naula_resolution_release(&resolution);

    naula_session *session = open_session(NULL, 0, NULL);
    naula_reference reference;
    memset(&reference, 0, sizeof reference);
    reference.size = sizeof reference;
    reference.url = "https://example.org/alpha/lib.git";
    start_resolution(&resolution);
    refusal = fresh_refusal();
    CHECK(naula_resolve(session, &reference, &resolution, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "unverifiable", "a url reference");
    naula_resolution_release(&resolution);
    naula_session_close(session);
    CHECK(forge.log_length == 0);
}

static void a_cache_that_fails_to_parse_is_absent_and_reported(void) {
    char cache[512];
    char path[1024];
    cache_directory("broken-cache", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    record_path(cache, X, "forge:10", path, sizeof path);
    write_text_file(path, "{\"id\":\"" ID "\",\"primary\":0,\"homes\":[{\"provider\":");

    naula_session *offline = open_session(cache, 1, NULL);
    CHECK(resolve_from(offline, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_UNREACHABLE);
    CHECK(resolution.cache_problem[0] != '\0');
    naula_resolution_release(&resolution);

    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.cache_problem[0] != '\0' && resolution.home_count == 1);
    naula_resolution_release(&resolution);
    CHECK(resolve_from(offline, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_STALE && resolution.cache_problem[0] == '\0');
    naula_resolution_release(&resolution);

    write_text_file(path, "{\"id\":\"git:" ELSEWHERE "\",\"primary\":0,\"homes\":[{\"provider\":\"forge\","
                          "\"account\":\"alpha\",\"number\":10,\"confirmed\":1}]}");
    CHECK(resolve_from(offline, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_UNREACHABLE);
    CHECK(strstr(resolution.cache_problem, "id") != NULL);
    naula_resolution_release(&resolution);
    naula_session_close(offline);
}

static void session_refuses_options_smaller_than_their_first_shape(void) {
    naula_session_options options;
    memset(&options, 0, sizeof options);
    options.size = 8;
    naula_session *session = NULL;
    CHECK(naula_session_open(&options, &session, NULL) == NAULA_REFUSED && session == NULL);
}

static const char base64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64(const char *text, char *out, size_t size) {
    size_t length = strlen(text);
    size_t written = 0;
    for (size_t index = 0; index < length && written + 5 < size; index += 3) {
        uint32_t group = (uint32_t)(unsigned char)text[index] << 16;
        if (index + 1 < length) {
            group |= (uint32_t)(unsigned char)text[index + 1] << 8;
        }
        if (index + 2 < length) {
            group |= (uint32_t)(unsigned char)text[index + 2];
        }
        out[written++] = base64_alphabet[(group >> 18) & 63];
        out[written++] = base64_alphabet[(group >> 12) & 63];
        out[written++] = index + 1 < length ? base64_alphabet[(group >> 6) & 63] : '=';
        out[written++] = index + 2 < length ? base64_alphabet[group & 63] : '=';
    }
    out[written] = '\0';
}

typedef struct github_home {
    char repository_route[64];
    char contents_route[128];
    char compare_route[160];
    char commits_route[128];
    char repository[512];
    char contents[1024];
} github_home;

static void github_home_fill(github_home *home, unsigned number, const char *account, unsigned parent,
                             const char *file) {
    char encoded[768];
    char parent_member[64] = "";
    if (parent != 0) {
        snprintf(parent_member, sizeof parent_member, ",\"parent\":{\"id\":%u}", parent);
    }
    snprintf(home->repository_route, sizeof home->repository_route, "/repositories/%u", number);
    snprintf(home->contents_route, sizeof home->contents_route, "/repositories/%u/contents/repo.json?ref=development",
             number);
    snprintf(home->compare_route, sizeof home->compare_route, "/repositories/%u/compare/%s...development?per_page=1",
             number, X);
    snprintf(home->commits_route, sizeof home->commits_route, "/repositories/%u/commits/%s", number, X);
    snprintf(home->repository, sizeof home->repository,
             "{\"id\":%u,\"name\":\"lib\",\"owner\":{\"login\":\"%s\"},\"default_branch\":\"main\","
             "\"archived\":false%s}",
             number, account, parent_member);
    base64(file, encoded, sizeof encoded);
    snprintf(home->contents, sizeof home->contents, "{\"type\":\"file\",\"encoding\":\"base64\",\"content\":\"%s\"}",
             encoded);
}

static http_route route(const char *path, const char *body) {
    http_route made;
    memset(&made, 0, sizeof made);
    made.path = path;
    made.status = 200;
    made.body = body;
    made.body_length = strlen(body);
    made.declared_length = HTTP_ROUTE_BODY_LENGTH;
    return made;
}

static size_t github_routes(const github_home *home, http_route *routes) {
    static const char *const ahead = "{\"status\":\"ahead\"}";
    static const char *const root = "{\"sha\":\"" X "\",\"parents\":[]}";
    routes[0] = route(home->repository_route, home->repository);
    routes[1] = route(home->contents_route, home->contents);
    routes[2] = route(home->compare_route, ahead);
    routes[3] = route(home->commits_route, root);
    return 4;
}

static http_fixture *serve(const http_route *routes, size_t count, int web) {
    http_fixture *server = http_fixture_start(routes, count);
    CHECK(server != NULL);
    if (server != NULL) {
        char root[64];
        snprintf(root, sizeof root, "http://127.0.0.1:%u", (unsigned)http_fixture_port(server));
        if (web) {
            naula_web_use_root(root);
        } else {
            naula_github_use_api_root(root);
        }
    }
    return server;
}

static void refuses_a_github_fork_whose_parent_confirms_the_id(void) {
    static github_home original;
    static github_home fork;
    forge_reset();
    github_home_fill(&original, 1001, "octo", 0,
                     repo_file("lib", "{\"provider\":\"github\",\"account\":\"octo\"}", NULL));
    github_home_fill(&fork, 2002, "mallory", 1001,
                     repo_file("lib", "{\"provider\":\"github\",\"account\":\"mallory\"}", NULL));
    http_route routes[8];
    size_t count = github_routes(&original, routes);
    count += github_routes(&fork, routes + count);
    http_fixture *server = serve(routes, count, 0);
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(NULL, ID, "github:2002", NULL, &resolution, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "copy-of", "a fork whose parent confirms the id");
    CHECK(strstr(refusal.detail, "github:1001") != NULL);
    CHECK(resolution.home_count == 0);
    naula_resolution_release(&resolution);

    CHECK(resolve_once(NULL, ID, "github:1001", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 1 && primary_number(&resolution) == 1001);
    check_text(resolution.primary_repository.account, "octo", "the original through GitHub");
    naula_resolution_release(&resolution);
    naula_github_use_api_root(NULL);
    naula_memory_release(http_fixture_stop(server));
}

static naula_status web_history_contains(void *context, const naula_place *home, const char *ref, const char *commit,
                                         int *contained, naula_refusal *refusal) {
    (void)context;
    (void)refusal;
    CHECK(home->kind == NAULA_HOME_WEB && ref == NULL);
    *contained = strcmp(commit, X) == 0;
    return NAULA_OK;
}

static naula_status web_history_parent_count(void *context, const naula_place *home, const char *commit,
                                             size_t *count, naula_refusal *refusal) {
    (void)context;
    (void)home;
    (void)commit;
    (void)refusal;
    *count = 0;
    return NAULA_OK;
}

static void marks_a_web_only_repo_domain_only(void) {
    static const char *const document =
        "{\"id\":\"" ID "\",\"homes\":[{\"provider\":\"web\",\"domain\":\"naula.dev\"}]}";
    forge_reset();
    http_route routes[1] = {route("/.well-known/repo-identity/" X ".json", document)};
    http_fixture *server = serve(routes, 1, 1);
    naula_history history = {sizeof(naula_history), NULL, web_history_contains, web_history_parent_count};
    naula_session *session = open_session(NULL, 0, &history);
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_from(session, ID, "web:naula.dev", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_DOMAIN_ONLY && resolution.home_count == 1);
    CHECK(resolution.homes[0].kind == NAULA_HOME_WEB);
    check_text(resolution.homes[0].domain, "naula.dev", "the domain home");
    CHECK(resolution.file.name == NULL && resolution.primary_repository.account == NULL);
    naula_resolution_release(&resolution);
    naula_session_close(session);

    CHECK(resolve_once(NULL, ID, "web:naula.dev", NULL, &resolution, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "unverifiable", "the installer cannot read a domain's history");
    naula_resolution_release(&resolution);
    naula_web_use_root(NULL);
    naula_memory_release(http_fixture_stop(server));
}

static void stale_serves_only_homes_confirmed_last_and_not_refused(void) {
    char cache[512];
    cache_directory("stale-served", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    const char *with_beta = repo_file("lib", HOME("alpha") "," HOME("beta"), NULL);
    fake_repo *original = add_repo(NULL, 10, "alpha", "lib", with_beta);
    add_repo(NULL, 20, "beta", "lib", with_beta);
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 2);
    naula_resolution_release(&resolution);

    const char *with_gamma = repo_file("lib", HOME("alpha") "," HOME("gamma"), NULL);
    original->development = with_gamma;
    fake_repo *gamma = add_repo(NULL, 30, "gamma", "lib", with_gamma);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(!has_home(&resolution, NULL, 20) && has_home(&resolution, NULL, 30));
    naula_resolution_release(&resolution);

    gamma->times_out = 1;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_STALE && has_home(&resolution, NULL, 10));
    if (has_home(&resolution, NULL, 20)) {
        fprintf(stderr, "a STALE answer served beta, which alpha stopped listing before the record was written\n");
        check_failures++;
    }
    naula_resolution_release(&resolution);

    original->times_out = 1;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_STALE && !has_home(&resolution, NULL, 20));
    naula_resolution_release(&resolution);

    cache_directory("stale-refused", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    with_beta = repo_file("lib", HOME("alpha") "," HOME("beta"), NULL);
    original = add_repo(NULL, 10, "alpha", "lib", with_beta);
    add_repo(NULL, 20, "beta", "lib", with_beta);
    CHECK(resolve_once(cache, ID, "forge:10", "forge:20", &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    original->development = repo_file("lib", HOME("alpha") "," HOME("gamma"), NULL);
    gamma = add_repo(NULL, 30, "gamma", "lib", original->development);
    gamma->times_out = 1;
    CHECK(resolve_once(cache, ID, "forge:10", "forge:20", &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_STALE && has_home(&resolution, NULL, 10));
    if (has_home(&resolution, NULL, 20)) {
        fprintf(stderr, "a STALE answer served beta, which this very run heard alpha disown\n");
        check_failures++;
    }
    naula_resolution_release(&resolution);
}

static void a_cached_home_never_confirms_on_its_own(void) {
    char cache[512];
    cache_directory("shared-cache", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    add_repo(NULL, 66, "mallory", "lib", repo_file("lib", HOME("mallory"), NULL));
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(cache, ID, "forge:66", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    accept_calls = 0;
    accept_answer = 0;
    forget_log();
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 1 && has_home(&resolution, NULL, 10) && accept_calls == 0);
    if (has_home(&resolution, NULL, 66) || strstr(forge.log, ":66") != NULL) {
        fprintf(stderr, "a copy another consumer met first was reached from the cache\n");
        check_failures++;
    }
    naula_resolution_release(&resolution);
    accept_answer = 1;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(!has_home(&resolution, NULL, 66));
    naula_resolution_release(&resolution);

    cache_directory("inventree-shelf", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    add_repo(NULL, 20, "beta", "shelf", repo_file("shelf", HOME("beta"), NULL));
    fake_repo *inventree = add_repo(NULL, 30, "gamma", "inventree", repo_file("inventree", HOME("gamma"), NULL));
    CHECK(resolve_once(cache, ID, "forge:30", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    accept_answer = 0;
    accept_calls = 0;
    CHECK(resolve_once(cache, ID, "forge:20", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 1 && has_home(&resolution, NULL, 20) && accept_calls == 0);
    naula_resolution_release(&resolution);
    inventree->development = repo_file("inventree", HOME("gamma"), "git:" ELSEWHERE);
    inventree->parents = 1;
    CHECK(resolve_once(cache, ID, "forge:20", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 1 && has_home(&resolution, NULL, 20) && accept_calls == 0);
    naula_resolution_release(&resolution);
}

static void matches_a_listed_home_only_to_one_holding_the_id(void) {
    forge_reset();
    add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    fake_repo *other = add_repo(NULL, 55, "alpha", "lib-old", repo_file("lib", HOME("alpha") "," HOME("gamma"), NULL));
    other->holds = ELSEWHERE;
    fake_repo *gamma = add_repo(NULL, 30, "gamma", "lib", repo_file("lib", HOME("alpha") "," HOME("gamma"), NULL));
    gamma->fork_parent = 55;
    naula_resolution resolution;
    naula_refusal refusal;
    naula_status status = resolve_once(NULL, ID, "forge:30", NULL, &resolution, &refusal);
    if (status == NAULA_OK && has_home(&resolution, NULL, 30)) {
        fprintf(stderr, "gamma was confirmed through alpha's other repository, which does not hold the id\n");
        check_failures++;
    }
    CHECK(strstr(forge.log, "find -:alpha/lib") != NULL);
    naula_resolution_release(&resolution);
}

/** A record of `count` homes; the first is CONFIRMED and the primary when `first_confirmed`, every other is `state`. */
static void write_record_of(const char *path, int count, int first_confirmed, const char *state) {
    static char record[32768];
    size_t used = (size_t)snprintf(record, sizeof record, "{\"at\":[\"forge:10\"],\"id\":\"" ID "\",%s\"homes\":[",
                                   first_confirmed ? "\"primary\":0," : "");
    for (int index = 0; index < count; index++) {
        used += (size_t)snprintf(record + used, sizeof record - used,
                                 "%s{\"state\":\"%s\",\"provider\":\"forge\",\"account\":\"a%d\",\"number\":%d,"
                                 "\"confirmed\":%d}",
                                 index == 0 ? "" : ",", index == 0 && first_confirmed ? "confirmed" : state, index,
                                 1000 + index, 1 + index);
    }
    snprintf(record + used, sizeof record - used, "]}");
    write_text_file(path, record);
}

static void the_written_record_is_always_readable(void) {
    char cache[512];
    char path[1024];
    cache_directory("full-record", cache, sizeof cache);
    forget_cache(cache);
    record_path(cache, X, "forge:10", path, sizeof path);
    forge_reset();
    add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    write_record_of(path, 64, 1, "confirmed");
    forge.missing_times_out = 1;
    accept_answer = 1;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.cache_problem[0] == '\0');
    naula_resolution_release(&resolution);
    naula_session *offline = open_session(cache, 1, NULL);
    CHECK(resolve_from(offline, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_STALE && resolution.home_count == 1 && has_home(&resolution, NULL, 10));
    CHECK(resolution.cache_problem[0] == '\0');
    naula_resolution_release(&resolution);

    forge_reset();
    const char *both = repo_file("lib", HOME("alpha") "," HOME("beta"), NULL);
    add_repo(NULL, 10, "alpha", "lib", both);
    add_repo(NULL, 20, "beta", "lib", both);
    write_record_of(path, 64, 0, "guard");
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 2 && resolution.cache_problem[0] == '\0');
    naula_resolution_release(&resolution);
    CHECK(resolve_from(offline, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.cache_problem[0] == '\0' && resolution.home_count == 2 && primary_number(&resolution) == 10);
    naula_resolution_release(&resolution);
    naula_session_close(offline);
}

static void anchor_of(const char *const *texts, size_t count, char anchor[NAULA_CACHE_ANCHOR_SIZE]) {
    naula_place places[4];
    for (size_t index = 0; index < count; index++) {
        memset(&places[index], 0, sizeof places[index]);
        places[index].size = sizeof places[index];
        CHECK(naula_place_parse(texts[index], &places[index], NULL) == NAULA_OK);
    }
    CHECK(naula_cache_anchor(places, count, anchor) == NAULA_OK);
    for (size_t index = 0; index < count; index++) {
        naula_place_release(&places[index]);
    }
}

static void the_anchor_is_the_sorted_places_hashed(void) {
    static const char *const listed[3] = {"github:1222336612", "web:naula.dev", "gitea@git.example.com:12"};
    static const char *const reordered[3] = {"web:naula.dev", "gitea@git.example.com:12", "github:1222336612"};
    static const char *const one[1] = {"forge:10"};
    char anchor[NAULA_CACHE_ANCHOR_SIZE];
    anchor_of(listed, 3, anchor);
    check_text(anchor, "ca6e43e082b3e59c206ff2cb54cee7a4ecc279fc403bdcc4fcc343d2fc52760e",
               "sha256 of the sorted places joined by newlines");
    anchor_of(reordered, 3, anchor);
    check_text(anchor, "ca6e43e082b3e59c206ff2cb54cee7a4ecc279fc403bdcc4fcc343d2fc52760e",
               "the order of at does not change the anchor");
    anchor_of(one, 1, anchor);
    check_text(anchor, "9e602b6f27a80f1e8c1dc1b3a84d6e173e1f480d501a96767167267d0f50e5ae", "one place");
}

static void another_consumers_first_contact_never_shapes_this_answer(void) {
    char cache[512];
    naula_resolution resolution;
    naula_refusal refusal;
    cache_directory("first-contact-stale", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    fake_repo *original = add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    add_repo(NULL, 66, "mallory", "lib", repo_file("lib", HOME("mallory"), NULL));
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    CHECK(resolve_once(cache, ID, "forge:66", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    original->times_out = 1;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_STALE && has_home(&resolution, NULL, 10) && !has_home(&resolution, NULL, 66));
    naula_resolution_release(&resolution);
    original->times_out = 0;
    naula_session *offline = open_session(cache, 1, NULL);
    CHECK(resolve_from(offline, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(has_home(&resolution, NULL, 10) && !has_home(&resolution, NULL, 66));
    naula_resolution_release(&resolution);
    naula_session_close(offline);

    cache_directory("first-contact-primary", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    original = add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    add_repo(NULL, 66, "mallory", "lib", repo_file("lib", HOME("mallory"), NULL));
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    CHECK(resolve_once(cache, ID, "forge:66", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    const char *taken = repo_file("lib", HOME("evil") "," HOME("alpha"), NULL);
    original->development = taken;
    add_repo(NULL, 88, "evil", "lib", taken);
    accept_answer = 0;
    accept_calls = 0;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "primary-changed", "a new primary after another consumer's first contact");
    CHECK(accept_calls == 1 && accepted_becomes == 88);
    naula_resolution_release(&resolution);

    cache_directory("first-contact-guards", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    const char *genuine = repo_file("lib", HOME("bee") "," HOME("alpha"), NULL);
    add_repo(NULL, 10, "alpha", "lib", genuine);
    fake_repo *bee = add_repo(NULL, 20, "bee", "lib", genuine);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    static char list[4096];
    static char accounts[64][8];
    size_t used = 0;
    for (int index = 0; index < 64; index++) {
        snprintf(accounts[index], sizeof accounts[index], "m%d", index);
        used += (size_t)snprintf(list + used, sizeof list - used, "%s{\"provider\":\"forge\",\"account\":\"%s\"}",
                                 index == 0 ? "" : ",", accounts[index]);
    }
    const char *copies = repo_file("lib", list, NULL);
    for (int index = 0; index < 64; index++) {
        add_repo(NULL, (uint64_t)(100 + index), accounts[index], "lib", copies);
    }
    CHECK(resolve_once(cache, ID, "forge:100", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    bee->gone = 1;
    add_repo(NULL, 21, "bee", "lib", genuine);
    accept_answer = 1;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    if (has_home(&resolution, NULL, 21)) {
        fprintf(stderr, "another consumer's 64 homes evicted the guard, and the re-registered bee/lib became a home\n");
        check_failures++;
    }
    CHECK(verdict_is(&resolution, NULL, 21, "number-changed", 0));
    naula_resolution_release(&resolution);
}

static void expect_one_record(const char *label, const char *const *left, size_t left_count, const char *const *right,
                              size_t right_count) {
    char one[NAULA_CACHE_ANCHOR_SIZE];
    char two[NAULA_CACHE_ANCHOR_SIZE];
    anchor_of(left, left_count, one);
    anchor_of(right, right_count, two);
    if (strcmp(one, two) != 0) {
        fprintf(stderr, "%s: %s and %s are two records for one at\n", label, one, two);
        check_failures++;
    }
}

static void places_naming_one_home_share_one_record(void) {
    static const char *const bare[1] = {"github:7"};
    static const char *const hosted[1] = {"github@github.com:7"};
    static const char *const hosted_upper[1] = {"github@GitHub.com:7"};
    static const char *const zeros[1] = {"github:007"};
    static const char *const upper_domain[1] = {"web:Naula.dev"};
    static const char *const lower_domain[1] = {"web:naula.dev"};
    static const char *const upper_host[1] = {"gitea@Git.Example.com:12"};
    static const char *const lower_host[1] = {"gitea@git.example.com:12"};
    static const char *const twice[2] = {"github:7", "github:7"};
    static const char *const forward[2] = {"github:7", "github:8"};
    static const char *const backward[2] = {"github:8", "github@github.com:7"};
    char anchor[NAULA_CACHE_ANCHOR_SIZE];
    expect_one_record("github with and without its host", bare, 1, hosted, 1);
    expect_one_record("github's host in another case", bare, 1, hosted_upper, 1);
    expect_one_record("leading zeros", bare, 1, zeros, 1);
    expect_one_record("domain case", upper_domain, 1, lower_domain, 1);
    expect_one_record("host case", upper_host, 1, lower_host, 1);
    expect_one_record("a duplicate place", bare, 1, twice, 2);
    expect_one_record("order and a written host", forward, 2, backward, 2);
    anchor_of(bare, 1, anchor);
    check_text(anchor, "2a47f80988756f901c09a18fc3fe67263fe52b6ed0eaac9012e26bdacb426c47", "github:7");
    anchor_of(forward, 2, anchor);
    check_text(anchor, "686548fde7ce4158a664b2c83df5127a842b41aa82bc79af4c082f507e05d752", "github:7 and github:8");
    naula_place place;
    memset(&place, 0, sizeof place);
    place.size = sizeof place;
    CHECK(naula_place_parse("web:a\nb", &place, NULL) != NAULA_OK);
    naula_place_release(&place);
}

static void an_edited_at_keeps_its_lineage(void) {
    char cache[512];
    naula_resolution resolution;
    naula_refusal refusal;
    cache_directory("at-changes", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    const char *genuine = repo_file("lib", HOME("bee") "," HOME("alpha") "," HOME("gamma"), NULL);
    add_repo(NULL, 10, "alpha", "lib", genuine);
    fake_repo *bee = add_repo(NULL, 20, "bee", "lib", genuine);
    add_repo(NULL, 30, "gamma", "lib", genuine);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(primary_number(&resolution) == 20);
    naula_resolution_release(&resolution);
    bee->gone = 1;
    add_repo(NULL, 21, "bee", "lib", genuine);
    accept_answer = 0;
    accept_calls = 0;
    naula_status status = resolve_once(cache, ID, "forge:10", "forge:30", &resolution, &refusal);
    if (has_home(&resolution, NULL, 21)) {
        fprintf(stderr, "a new at lost the guard for bee, and its re-registered repository 21 became a home\n");
        check_failures++;
    }
    CHECK(status == NAULA_REFUSED && accept_calls == 1 && accepted_was == 20);
    check_text(refusal.code, "primary-changed", "a new at keeps the primary prompt of the at it grew from");
    CHECK(verdict_is(&resolution, NULL, 21, "number-changed", 0));
    naula_resolution_release(&resolution);

    accept_answer = 1;
    CHECK(resolve_once(cache, ID, "forge:10", "forge:30", &resolution, &refusal) == NAULA_OK);
    CHECK(!has_home(&resolution, NULL, 21) && has_home(&resolution, NULL, 30));
    naula_resolution_release(&resolution);
}

static void a_first_contact_elsewhere_never_accepts_a_number_here(int bee_first) {
    char cache[512];
    naula_resolution resolution;
    naula_refusal refusal;
    cache_directory(bee_first ? "laundered-primary" : "laundered", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    const char *genuine = bee_first ? repo_file("lib", HOME("bee") "," HOME("alpha"), NULL)
                                    : repo_file("lib", HOME("alpha") "," HOME("bee"), NULL);
    add_repo(NULL, 10, "alpha", "lib", genuine);
    fake_repo *bee = add_repo(NULL, 20, "bee", "lib", genuine);
    accept_answer = 1;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    bee->gone = 1;
    add_repo(NULL, 21, "bee", "lib", genuine);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(!has_home(&resolution, NULL, 21));
    naula_resolution_release(&resolution);
    resolve_once(cache, ID, "forge:21", NULL, &resolution, &refusal);
    naula_resolution_release(&resolution);
    accept_answer = 0;
    resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal);
    if (has_home(&resolution, NULL, 21)) {
        fprintf(stderr, "another consumer's first contact on 21 made this consumer accept it\n");
        check_failures++;
    }
    CHECK(verdict_is(&resolution, NULL, 21, "number-changed", 0));
    naula_resolution_release(&resolution);
}

static void a_copys_homes_never_evict_what_protects_an_edited_at(void) {
    char cache[512];
    naula_resolution resolution;
    naula_refusal refusal;
    cache_directory("evicted", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    const char *genuine = repo_file("lib", HOME("alpha") "," HOME("bee") "," HOME("gamma"), NULL);
    add_repo(NULL, 10, "alpha", "lib", genuine);
    fake_repo *bee = add_repo(NULL, 20, "bee", "lib", genuine);
    add_repo(NULL, 30, "gamma", "lib", genuine);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    static char list[4096];
    static char accounts[64][8];
    size_t used = 0;
    for (int index = 0; index < 64; index++) {
        snprintf(accounts[index], sizeof accounts[index], "m%d", index);
        used += (size_t)snprintf(list + used, sizeof list - used, "%s{\"provider\":\"forge\",\"account\":\"%s\"}",
                                 index == 0 ? "" : ",", accounts[index]);
    }
    const char *copies = repo_file("lib", list, NULL);
    for (int index = 0; index < 64; index++) {
        add_repo(NULL, (uint64_t)(100 + index), accounts[index], "lib", copies);
    }
    resolve_once(cache, ID, "forge:100", NULL, &resolution, &refusal);
    naula_resolution_release(&resolution);
    bee->gone = 1;
    add_repo(NULL, 21, "bee", "lib", genuine);
    accept_answer = 1;
    resolve_once(cache, ID, "forge:10", "forge:30", &resolution, &refusal);
    if (has_home(&resolution, NULL, 21)) {
        fprintf(stderr, "a copy's 64 homes evicted the number that protects an edited at\n");
        check_failures++;
    }
    CHECK(verdict_is(&resolution, NULL, 21, "number-changed", 0));
    naula_resolution_release(&resolution);
}

static void a_copys_record_never_sets_a_baseline(void) {
    char cache[512];
    naula_resolution resolution;
    naula_refusal refusal;
    cache_directory("copy-baseline", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    add_repo(NULL, 66, "mallory", "lib", repo_file("lib", HOME("mallory"), NULL));
    resolve_once(cache, ID, "forge:66", "forge:10", &resolution, &refusal);
    naula_resolution_release(&resolution);
    accept_answer = 0;
    accept_calls = 0;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(accept_calls == 0 && has_home(&resolution, NULL, 10) && !has_home(&resolution, NULL, 66));
    naula_resolution_release(&resolution);
}

/**
 * The record guards bee at 20 and still holds a former mirror (40); a new `at` names the mirror, which this run
 * refuses. `mirror_order` 0 puts the mirror second in `at`, 1 first, 2 first with a mirror that lists no bee.
 */
static naula_status a_former_mirror_in_a_new_at(int mirror_order, naula_resolution *resolution) {
    char cache[512];
    char name[32];
    naula_refusal refusal;
    snprintf(name, sizeof name, "former-mirror-%d", mirror_order);
    cache_directory(name, cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    const char *with_mirror = repo_file("lib", HOME("bee") "," HOME("alpha") "," HOME("mirror"), NULL);
    fake_repo *alpha = add_repo(NULL, 10, "alpha", "lib", with_mirror);
    fake_repo *bee = add_repo(NULL, 20, "bee", "lib", with_mirror);
    fake_repo *mirror = add_repo(NULL, 40, "mirror", "lib", with_mirror);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, resolution, &refusal) == NAULA_OK);
    naula_resolution_release(resolution);
    const char *without = repo_file("lib", HOME("bee") "," HOME("alpha"), NULL);
    alpha->development = without;
    bee->development = without;
    accept_answer = 1;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, resolution, &refusal) == NAULA_OK);
    naula_resolution_release(resolution);
    bee->gone = 1;
    add_repo(NULL, 21, "bee", "lib", without);
    if (mirror_order == 2) {
        mirror->development = repo_file("lib", HOME("alpha") "," HOME("mirror"), NULL);
    }
    accept_answer = 0;
    accept_calls = 0;
    accepted_was = 0;
    return mirror_order == 0 ? resolve_once(cache, ID, "forge:10", "forge:40", resolution, &refusal)
                             : resolve_once(cache, ID, "forge:40", "forge:10", resolution, &refusal);
}

static void a_refusal_never_lifts_a_lineage_guard_whatever_the_order(void) {
    naula_status statuses[3];
    for (int order = 0; order < 3; order++) {
        naula_resolution resolution;
        statuses[order] = a_former_mirror_in_a_new_at(order, &resolution);
        if (has_home(&resolution, NULL, 21)) {
            fprintf(stderr, "order %d: a refused former mirror lifted the guard, and 21 became a home\n", order);
            check_failures++;
        }
        if (statuses[order] != NAULA_REFUSED || accept_calls != 1 || accepted_was != 20) {
            fprintf(stderr, "order %d: answered %d with %d prompts naming %llu, expected primary-changed asked of 20\n",
                    order, (int)statuses[order], accept_calls, (unsigned long long)accepted_was);
            check_failures++;
        }
        naula_resolution_release(&resolution);
    }
    CHECK(statuses[0] == statuses[1] && statuses[1] == statuses[2]);
}

static void a_home_refused_before_it_is_read_is_released_and_unheard(void) {
    char cache[512];
    naula_resolution resolution;
    naula_refusal refusal;
    cache_directory("refused-then-read", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    const char *genuine = repo_file("lib", HOME("bee") "," HOME("alpha") "," HOME("gamma"), NULL);
    add_repo(NULL, 10, "alpha", "lib", genuine);
    fake_repo *bee = add_repo(NULL, 20, "bee", "lib", genuine);
    add_repo(NULL, 30, "gamma", "lib", genuine);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    bee->gone = 1;
    add_repo(NULL, 21, "bee", "lib", genuine);
    accept_answer = 1;
    resolve_once(cache, ID, "forge:10", "forge:21", &resolution, &refusal);
    CHECK(!has_home(&resolution, NULL, 21));
    naula_resolution_release(&resolution);
    for (int run = 0; run < 3; run++) {
        resolve_once(cache, ID, "forge:10", "forge:30", &resolution, &refusal);
        CHECK(!has_home(&resolution, NULL, 21));
        naula_resolution_release(&resolution);
    }
    accept_answer = 0;
}

/**
 * A home refused number-changed for one account and then reached by number through a second account's guard: it is
 * read once already settled, which must release what it read, and counts as unheard rather than as disowning.
 */
static void a_settled_home_read_later_releases_its_file(void) {
    char cache[512];
    char path[1024];
    naula_resolution resolution;
    naula_refusal refusal;
    cache_directory("settled-then-read", cache, sizeof cache);
    forget_cache(cache);
    record_path(cache, X, "forge:10", path, sizeof path);
    forge_reset();
    fake_repo *alpha = add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    const char *listing = repo_file("lib", HOME("alpha") "," HOME("bee") "," HOME("bea"), NULL);
    alpha->development = listing;
    add_repo(NULL, 21, "bee", "lib", listing);
    write_text_file(path, "{\"at\":[\"forge:10\"],\"id\":\"" ID "\",\"primary\":0,\"homes\":["
                          "{\"state\":\"confirmed\",\"provider\":\"forge\",\"account\":\"alpha\",\"number\":10,"
                          "\"confirmed\":1},"
                          "{\"state\":\"guard\",\"provider\":\"forge\",\"account\":\"bee\",\"number\":20,"
                          "\"confirmed\":1},"
                          "{\"state\":\"guard\",\"provider\":\"forge\",\"account\":\"bea\",\"number\":21,"
                          "\"confirmed\":1}]}");
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "unverifiable", "a listed home refused earlier in the run is unheard");
    CHECK(verdict_is(&resolution, NULL, 21, "number-changed", 0));
    naula_resolution_release(&resolution);
}

/**
 * An install with at=[21, 30] makes first contact with the re-registered bee (21) and becomes lineage for the
 * victim's edited at=[10, 30] beside the victim's own record (bee 20). `genuine_lists_bee` 0 then drops bee from the
 * genuine homes, so only what the victim's first record inherited can protect it later.
 */
static void a_later_first_contact_never_denies_an_edited_at(int genuine_lists_bee) {
    char cache[512];
    naula_resolution resolution;
    naula_refusal refusal;
    cache_directory(genuine_lists_bee ? "earliest-wins" : "earliest-inherited", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    const char *genuine = repo_file("lib", HOME("alpha") "," HOME("bee") "," HOME("gamma"), NULL);
    fake_repo *alpha = add_repo(NULL, 10, "alpha", "lib", genuine);
    fake_repo *bee = add_repo(NULL, 20, "bee", "lib", genuine);
    fake_repo *gamma = add_repo(NULL, 30, "gamma", "lib", genuine);
    accept_answer = 0;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    bee->gone = 1;
    add_repo(NULL, 21, "bee", "lib", genuine);
    resolve_once(cache, ID, "forge:21", "forge:30", &resolution, &refusal);
    naula_resolution_release(&resolution);
    if (!genuine_lists_bee) {
        const char *dropped = repo_file("lib", HOME("alpha") "," HOME("gamma"), NULL);
        alpha->development = dropped;
        gamma->development = dropped;
    }
    for (int run = 1; run <= 3; run++) {
        naula_status status = resolve_once(cache, ID, "forge:10", "forge:30", &resolution, &refusal);
        if (status != NAULA_OK || has_home(&resolution, NULL, 21)) {
            fprintf(stderr, "%s, run %d: the edited at answered %d (%s), 21 %s\n",
                    genuine_lists_bee ? "a later first contact" : "an inherited number", run, (int)status,
                    refusal.code, has_home(&resolution, NULL, 21) ? "a home" : "refused");
            check_failures++;
        }
        naula_resolution_release(&resolution);
    }
    if (!genuine_lists_bee) {
        const char *relisted = repo_file("lib", HOME("alpha") "," HOME("bee") "," HOME("gamma"), NULL);
        alpha->development = relisted;
        gamma->development = relisted;
        CHECK(resolve_once(cache, ID, "forge:10", "forge:30", &resolution, &refusal) == NAULA_OK);
        CHECK(!has_home(&resolution, NULL, 21));
        CHECK(verdict_is(&resolution, NULL, 21, "number-changed", 0));
        naula_resolution_release(&resolution);
    }
}

static int is_number_character(char character) {
    return (character >= '0' && character <= '9') || character == '.' || character == 'e' || character == 'E' ||
           character == '+' || character == '-';
}

/**
 * Rewrites every confirmation time in the record at `path` to `replacement`, a whole number in microseconds.
 *
 * @implNote The writer may print a time with an exponent (`1.764123456789e+15`) when that form reads back exactly,
 * so the whole number token is replaced, never its digits one by one.
 */
static void rewrite_confirmation_times(const char *path, const char *replacement) {
    static char rewritten[65536];
    char *text = read_text_file(path);
    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    const char *marker = "\"confirmed\":";
    size_t used = 0;
    const char *from = text;
    for (const char *at = strstr(from, marker); at != NULL; at = strstr(from, marker)) {
        at += strlen(marker);
        used += (size_t)snprintf(rewritten + used, sizeof rewritten - used, "%.*s%s", (int)(at - from), from,
                                 replacement);
        while (is_number_character(*at)) {
            at++;
        }
        from = at;
    }
    snprintf(rewritten + used, sizeof rewritten - used, "%s", from);
    write_text_file(path, rewritten);
    naula_memory_release(text);
}

/**
 * An install with at=[21, 30] confirms the re-registered bee (21); its record's times are then moved, to the past as
 * a clock set back while it was written leaves them, or to the future, as a clock now behind finds them.
 */
static naula_status an_install_under_a_wrong_clock(int clock_was_behind, naula_resolution *resolution) {
    char cache[512];
    char path[1024];
    naula_refusal refusal;
    cache_directory(clock_was_behind ? "clock-was-behind" : "clock-is-behind", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    const char *genuine = repo_file("lib", HOME("alpha") "," HOME("bee") "," HOME("gamma"), NULL);
    add_repo(NULL, 10, "alpha", "lib", genuine);
    fake_repo *bee = add_repo(NULL, 20, "bee", "lib", genuine);
    add_repo(NULL, 30, "gamma", "lib", genuine);
    accept_answer = 0;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, resolution, &refusal) == NAULA_OK);
    naula_resolution_release(resolution);
    bee->gone = 1;
    add_repo(NULL, 21, "bee", "lib", genuine);
    accept_answer = 1;
    resolve_once(cache, ID, "forge:21", "forge:30", resolution, &refusal);
    naula_resolution_release(resolution);
    accept_answer = 0;
    naula_place places[2];
    const char *texts[2] = {"forge:21", "forge:30"};
    char anchor[NAULA_CACHE_ANCHOR_SIZE];
    for (size_t index = 0; index < 2; index++) {
        memset(&places[index], 0, sizeof places[index]);
        places[index].size = sizeof places[index];
        CHECK(naula_place_parse(texts[index], &places[index], NULL) == NAULA_OK);
    }
    CHECK(naula_cache_anchor(places, 2, anchor) == NAULA_OK);
    naula_place_release(&places[0]);
    naula_place_release(&places[1]);
    snprintf(path, sizeof path, "%s/identities/%s-%s.json", cache, X, anchor);
    rewrite_confirmation_times(path, clock_was_behind ? "1000000000000000" : "9000000000000000");
    return resolve_once(cache, ID, "forge:10", "forge:30", resolution, &refusal);
}

static void a_wrong_clock_never_settles_a_re_registered_number(void) {
    naula_resolution resolution;
    an_install_under_a_wrong_clock(1, &resolution);
    if (has_home(&resolution, NULL, 21)) {
        fprintf(stderr, "a record written while the clock was behind settled the re-registered 21 for bee\n");
        check_failures++;
    }
    CHECK(verdict_is(&resolution, NULL, 21, "number-changed", 0));
    naula_resolution_release(&resolution);

    an_install_under_a_wrong_clock(0, &resolution);
    CHECK(!has_home(&resolution, NULL, 21));
    CHECK(strstr(resolution.cache_problem, "clock is behind") != NULL);
    naula_resolution_release(&resolution);
}

static void records_that_disagree_on_the_primary_ask(void) {
    char cache[512];
    naula_resolution resolution;
    naula_refusal refusal;
    cache_directory("baselines-disagree", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    fake_repo *first = add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha") "," HOME("beta"), NULL));
    fake_repo *second = add_repo(NULL, 20, "beta", "lib", first->development);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    const char *swapped = repo_file("lib", HOME("beta") "," HOME("alpha"), NULL);
    first->development = swapped;
    second->development = swapped;
    accept_answer = 1;
    CHECK(resolve_once(cache, ID, "forge:20", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    accept_answer = 0;
    accept_calls = 0;
    CHECK(resolve_once(cache, ID, "forge:10", "forge:20", &resolution, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "primary-changed", "records sharing a place disagree on the primary");
    CHECK(accept_calls == 1);
    naula_resolution_release(&resolution);
}

static void a_record_for_another_at_is_ignored_and_kept(void) {
    char cache[512];
    char path[1024];
    naula_resolution resolution;
    naula_refusal refusal;
    cache_directory("foreign-record", cache, sizeof cache);
    forget_cache(cache);
    record_path(cache, X, "forge:10", path, sizeof path);
    forge_reset();
    add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    static const char *const foreign =
        "{\"at\":[\"forge:66\"],\"id\":\"" ID "\",\"primary\":0,\"homes\":[{\"state\":\"confirmed\",\"provider\":"
        "\"forge\",\"account\":\"mallory\",\"number\":66,\"confirmed\":1}]}";
    write_text_file(path, foreign);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(strstr(resolution.cache_problem, "another at") != NULL && !has_home(&resolution, NULL, 66));
    naula_resolution_release(&resolution);
    char *kept = read_text_file(path);
    check_text(kept, foreign, "a record for another at is never overwritten");
    naula_memory_release(kept);
    naula_session *offline = open_session(cache, 1, NULL);
    CHECK(resolve_from(offline, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_UNREACHABLE);
    CHECK(!has_home(&resolution, NULL, 66));
    naula_resolution_release(&resolution);
    naula_session_close(offline);
}

static void one_run_of_404s_never_demotes_a_confirmed_home(void) {
    char cache[512];
    naula_resolution resolution;
    naula_refusal refusal;
    cache_directory("one-bad-run", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    const char *moved = repo_file("lib", HOME_AT("new.example", "alpha") "," HOME("alpha"), NULL);
    fake_repo *old_home = add_repo(NULL, 10, "alpha", "lib", moved);
    fake_repo *new_home = add_repo("new.example", 20, "alpha", "lib", moved);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    old_home->gone = 1;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(has_home(&resolution, "new.example", 20));
    naula_resolution_release(&resolution);
    new_home->gone = 1;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_NOT_FOUND);
    CHECK(resolution.home_count == 0);
    naula_resolution_release(&resolution);
    new_home->gone = 0;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    if (!has_home(&resolution, "new.example", 20)) {
        fprintf(stderr, "one run of 404s ended the consumer's only way to its repository\n");
        check_failures++;
    }
    naula_resolution_release(&resolution);
}

static void a_relative_cache_folder_is_refused(void) {
    naula_session_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.cache_directory = "relative/cache";
    naula_session *session = NULL;
    naula_refusal refusal = fresh_refusal();
    CHECK(naula_session_open(&options, &session, &refusal) == NAULA_REFUSED && session == NULL);
    check_text(refusal.code, "invalid_options", "a relative cache folder");
}

#if defined(_WIN32)
static void windows_cache_folders_in_every_form(void) {
    char base[512];
    char deep[2048];
    wchar_t wide[4096];
    naula_resolution resolution;
    naula_refusal refusal;
    cache_directory("forms", base, sizeof base);
    CreateDirectoryA(base, NULL);
    snprintf(deep, sizeof deep, "%s", base);
    for (int level = 0; level < 6; level++) {
        size_t length = strlen(deep);
        snprintf(deep + length, sizeof deep - length, "/%s", "a-folder-name-that-is-forty-characters-x");
        int written = swprintf(wide, sizeof wide / sizeof wide[0], L"\\\\?\\%hs", deep);
        for (int index = 4; written > 0 && wide[index] != L'\0'; index++) {
            wide[index] = wide[index] == L'/' ? L'\\' : wide[index];
        }
        CreateDirectoryW(wide, NULL);
    }
    forge_reset();
    add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    CHECK(resolve_once(deep, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.cache_problem[0] == '\0');
    naula_resolution_release(&resolution);
    naula_session *offline = open_session(deep, 1, NULL);
    CHECK(resolve_from(offline, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_STALE);
    naula_resolution_release(&resolution);
    naula_session_close(offline);

    char prefixed[1024];
    snprintf(prefixed, sizeof prefixed, "\\\\?\\%s", base);
    for (char *at = prefixed + 4; *at != '\0'; at++) {
        *at = *at == '/' ? '\\' : *at;
    }
    CHECK(resolve_once(prefixed, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    if (resolution.cache_problem[0] != '\0') {
        fprintf(stderr, "a cache folder written with the long path prefix was not written: %s\n",
                resolution.cache_problem);
        check_failures++;
    }
    naula_resolution_release(&resolution);
    offline = open_session(prefixed, 1, NULL);
    CHECK(resolve_from(offline, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    naula_session_close(offline);
}
#endif

static void a_number_that_answered_gone_is_asked_again(void) {
    char cache[512];
    naula_resolution resolution;
    naula_refusal refusal;
    cache_directory("asked-again", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    const char *both = repo_file("lib", HOME("beta") "," HOME("alpha"), NULL);
    fake_repo *original = add_repo(NULL, 10, "alpha", "lib", both);
    add_repo(NULL, 20, "beta", "lib", both);
    CHECK(resolve_once(cache, ID, "forge:20", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    original->gone = 1;
    CHECK(resolve_once(cache, ID, "forge:20", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(verdict_is(&resolution, NULL, 10, "dead-skipped", 0));
    naula_resolution_release(&resolution);

    original->gone = 0;
    forget_log();
    CHECK(resolve_once(cache, ID, "forge:20", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(strstr(forge.log, "lookup -:10") != NULL && resolution.home_count == 2);
    naula_resolution_release(&resolution);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(has_home(&resolution, NULL, 10));
    naula_resolution_release(&resolution);
    add_repo(NULL, 66, "mallory", "lib", repo_file("lib", HOME("mallory") "," HOME("alpha"), NULL));
    CHECK(resolve_once(cache, ID, "forge:66", NULL, &resolution, &refusal) == NAULA_REFUSED);
    CHECK(!has_home(&resolution, NULL, 66));
    naula_resolution_release(&resolution);

    original->gone = 1;
    CHECK(resolve_once(cache, ID, "forge:20", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    add_repo(NULL, 11, "alpha", "lib", both);
    CHECK(resolve_once(cache, ID, "forge:20", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(!has_home(&resolution, NULL, 11) && verdict_is(&resolution, NULL, 11, "number-changed", 0));
    naula_resolution_release(&resolution);
}

static void a_dead_at_starts_from_the_new_home_the_record_keeps(void) {
    char cache[512];
    naula_resolution resolution;
    naula_refusal refusal;
    cache_directory("dead-at", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    const char *moved = repo_file("lib", HOME_AT("new.example", "alpha") "," HOME("alpha"), NULL);
    fake_repo *old_home = add_repo(NULL, 10, "alpha", "lib", moved);
    add_repo("new.example", 20, "alpha", "lib", moved);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    old_home->gone = 1;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 1 && has_home(&resolution, "new.example", 20));
    naula_resolution_release(&resolution);
    naula_session *offline = open_session(cache, 1, NULL);
    CHECK(resolve_from(offline, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 1 && has_home(&resolution, "new.example", 20));
    naula_resolution_release(&resolution);
    naula_session_close(offline);

    cache_directory("every-home-gone", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    const char *both = repo_file("lib", HOME("alpha") "," HOME("beta"), NULL);
    fake_repo *first = add_repo(NULL, 10, "alpha", "lib", both);
    fake_repo *second = add_repo(NULL, 20, "beta", "lib", both);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    first->gone = 1;
    second->gone = 1;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_NOT_FOUND);
    CHECK(resolution.home_count == 0);
    naula_resolution_release(&resolution);
    offline = open_session(cache, 1, NULL);
    CHECK(resolve_from(offline, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_STALE && resolution.home_count == 2);
    naula_resolution_release(&resolution);
    naula_session_close(offline);
}

static void the_anchor_comes_first_in_the_confirmed_homes(void) {
    naula_resolution resolution;
    naula_refusal refusal;
    forge_reset();
    const char *both = repo_file("lib", HOME("alpha") "," HOME("beta"), NULL);
    add_repo(NULL, 10, "alpha", "lib", both);
    add_repo(NULL, 66, "beta", "lib", both);
    CHECK(resolve_once(NULL, ID, "forge:66", "forge:10", &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 2 && primary_number(&resolution) == 10);
    CHECK(verdict_is(&resolution, NULL, 66, "confirmed", 1) && verdict_is(&resolution, NULL, 10, "confirmed", 1));
    naula_resolution_release(&resolution);

    forge_reset();
    add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    add_repo(NULL, 66, "mallory", "lib", repo_file("lib", HOME("mallory"), NULL));
    CHECK(resolve_once(NULL, ID, "forge:66", "forge:10", &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 1 && has_home(&resolution, NULL, 66));
    CHECK(verdict_is(&resolution, NULL, 10, "not-linked", 0));
    naula_resolution_release(&resolution);
}

static void a_development_branch_without_repo_json_is_not_listed(void) {
    forge_reset();
    fake_repo *repo = add_repo(NULL, 10, "alpha", "lib", NULL);
    repo->development_without_file = 1;
    repo->main_file = repo_file("lib", HOME("alpha"), NULL);
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(NULL, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "not-listed", "a development branch that states no file");
    CHECK(strstr(forge.log, "read -:10 main") == NULL);
    naula_resolution_release(&resolution);
}

static void set_written_time(const char *path, time_t when) {
#if defined(_WIN32)
    struct _utimbuf times = {when, when};
    wchar_t wide[4200];
    long_path(path, wide, sizeof wide / sizeof wide[0]);
    CHECK(_wutime(wide, &times) == 0);
#else
    struct utimbuf times = {when, when};
    CHECK(utime(path, &times) == 0);
#endif
}

static int file_exists(const char *path) {
    FILE *file = open_file(path, "rb");
    if (file != NULL) {
        fclose(file);
    }
    return file != NULL;
}

static void a_record_past_max_path_is_written(void) {
    char name[220];
    char cache[1024];
    memset(name, 'l', 200);
    name[200] = '\0';
    cache_directory(name, cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    fake_repo *repo = add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    repo->holds = X256;
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(cache, "git:" X256, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    if (resolution.cache_problem[0] != '\0') {
        fprintf(stderr, "a record %zu characters deep was not written: %s\n", strlen(cache) + 96,
                resolution.cache_problem);
        check_failures++;
    }
    naula_resolution_release(&resolution);
    naula_session *offline = open_session(cache, 1, NULL);
    CHECK(resolve_from(offline, "git:" X256, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.state == NAULA_STALE && resolution.home_count == 1);
    naula_resolution_release(&resolution);
    naula_session_close(offline);
}

static void a_writer_removes_temporary_files_left_behind(void) {
    char cache[512];
    char old_temporary[1024];
    char new_temporary[1024];
    cache_directory("temporaries", cache, sizeof cache);
    forget_cache(cache);
    forge_reset();
    add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    record_path(cache, X, "forge:10", old_temporary, sizeof old_temporary);
    record_path(cache, X, "forge:10", new_temporary, sizeof new_temporary);
    strcat(old_temporary, ".1.1.tmp");
    strcat(new_temporary, ".2.2.tmp");
    write_text_file(old_temporary, "{");
    write_text_file(new_temporary, "{");
    set_written_time(old_temporary, time(NULL) - 7200);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    CHECK(!file_exists(old_temporary));
    CHECK(file_exists(new_temporary));
    remove_file(new_temporary);
}

static naula_status resolve_counted(naula_session *session, const naula_reference *reference,
                                    naula_resolution *resolution, uint64_t fail_at) {
    naula_refusal refusal = fresh_refusal();
    start_resolution(resolution);
    naula_memory_fail_at(fail_at);
    return naula_resolve(session, reference, resolution, &refusal);
}

/** Fails every allocation one resolution makes, once each, after two runs that settle what the cache holds. */
static void fails_cleanly(naula_session *session, const char *first, const char *second, naula_status expected,
                          const char *label) {
    naula_place places[2];
    const char *texts[2] = {first, second};
    size_t place_count = second == NULL ? 1 : 2;
    for (size_t index = 0; index < place_count; index++) {
        memset(&places[index], 0, sizeof places[index]);
        places[index].size = sizeof places[index];
        CHECK(naula_place_parse(texts[index], &places[index], NULL) == NAULA_OK);
    }
    naula_reference reference;
    memset(&reference, 0, sizeof reference);
    reference.size = sizeof reference;
    reference.id = ID;
    reference.at = places;
    reference.at_count = place_count;
    naula_resolution resolution;
    resolve_counted(session, &reference, &resolution, 0);
    naula_resolution_release(&resolution);
    naula_status settled = resolve_counted(session, &reference, &resolution, 0);
    uint64_t attempts = naula_memory_attempts();
    if (settled != expected) {
        fprintf(stderr, "%s: answered %d, expected %d\n", label, (int)settled, (int)expected);
        check_failures++;
    }
    naula_resolution_release(&resolution);
    CHECK(attempts > 0);
    for (uint64_t attempt = 1; attempt <= attempts; attempt++) {
        naula_status status = resolve_counted(session, &reference, &resolution, attempt);
        naula_memory_fail_at(0);
        if (status != NAULA_OUT_OF_MEMORY || resolution.homes != NULL || resolution.verdicts != NULL ||
            resolution.file.name != NULL || resolution.primary_repository.account != NULL) {
            fprintf(stderr, "%s: failing allocation %llu answered %d\n", label, (unsigned long long)attempt,
                    (int)status);
            check_failures++;
        }
        naula_resolution_release(&resolution);
    }
    for (size_t index = 0; index < place_count; index++) {
        naula_place_release(&places[index]);
    }
}

static void fails_cleanly_in_a_session(const char *cache, int offline, const char *first, naula_status expected,
                                       const char *label) {
    naula_session *session = open_session(cache, offline, NULL);
    fails_cleanly(session, first, NULL, expected, label);
    naula_session_close(session);
}

static void fresh_cache(const char *name, char *cache, size_t size) {
    cache_directory(name, cache, size);
    forget_cache(cache);
}

static void out_of_memory_on_the_cache_paths(void) {
    char cache[512];
    naula_resolution resolution;
    naula_refusal refusal;

    fresh_cache("oom-stale", cache, sizeof cache);
    forge_reset();
    fake_repo *home = add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    home->times_out = 1;
    fails_cleanly_in_a_session(cache, 0, "forge:10", NAULA_OK, "stale while a home times out");
    fails_cleanly_in_a_session(cache, 1, "forge:10", NAULA_OK, "stale offline");

    fresh_cache("oom-number-changed", cache, sizeof cache);
    forge_reset();
    const char *both = repo_file("lib", HOME("alpha") "," HOME_AT("second.example", "bee"), NULL);
    add_repo(NULL, 10, "alpha", "lib", both);
    fake_repo *second = add_repo("second.example", 20, "bee", "lib", both);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    second->gone = 1;
    add_repo("second.example", 21, "bee", "lib", both);
    fails_cleanly_in_a_session(cache, 0, "forge:10", NAULA_OK, "a number changed since the cache");

    fresh_cache("oom-primary", cache, sizeof cache);
    forge_reset();
    fake_repo *first = add_repo(NULL, 10, "alpha", "lib",
                                repo_file("lib", HOME("alpha") "," HOME_AT("second.example", "beta"), NULL));
    second = add_repo("second.example", 20, "beta", "lib", first->development);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    first->development = repo_file("lib", HOME_AT("second.example", "beta") "," HOME("alpha"), NULL);
    second->development = first->development;
    accept_answer = 0;
    fails_cleanly_in_a_session(cache, 0, "forge:10", NAULA_REFUSED, "a primary change refused");

    fresh_cache("oom-at-changes", cache, sizeof cache);
    forge_reset();
    const char *genuine = repo_file("lib", HOME("bee") "," HOME("alpha") "," HOME("gamma"), NULL);
    add_repo(NULL, 10, "alpha", "lib", genuine);
    fake_repo *bee = add_repo(NULL, 20, "bee", "lib", genuine);
    add_repo(NULL, 30, "gamma", "lib", genuine);
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    bee->gone = 1;
    add_repo(NULL, 21, "bee", "lib", genuine);
    accept_answer = 0;
    naula_session *session = open_session(cache, 0, NULL);
    fails_cleanly(session, "forge:10", "forge:30", NAULA_REFUSED, "a new at read through its lineage");
    naula_session_close(session);
    accept_answer = 0;
}

static void out_of_memory_on_the_web_and_fork_paths(void) {
    static const char *const document =
        "{\"id\":\"" ID "\",\"homes\":[{\"provider\":\"web\",\"domain\":\"naula.dev\"}]}";
    forge_reset();
    fake_repo *original = add_repo(NULL, 1, "root", "lib", repo_file("lib", HOME("root"), NULL));
    fake_repo *fork = add_repo(NULL, 2, "fork", "lib", repo_file("lib", HOME("fork"), NULL));
    fork->fork_parent = original->number;
    fails_cleanly_in_a_session(NULL, 0, "forge:2", NAULA_REFUSED, "a fork whose parent confirms the id");

    http_route routes[1] = {route("/.well-known/repo-identity/" X ".json", document)};
    http_fixture *server = serve(routes, 1, 1);
    naula_history history = {sizeof(naula_history), NULL, web_history_contains, web_history_parent_count};
    naula_session *session = open_session(NULL, 0, &history);
    fails_cleanly(session, "web:naula.dev", NULL, NAULA_OK, "a web home");
    naula_session_close(session);
    naula_web_use_root(NULL);
    naula_memory_release(http_fixture_stop(server));
}

static void out_of_memory_is_answered_at_every_allocation(void) {
    char cache[512];
    forge_reset();
    const char *both =
        repo_file("lib", HOME("alpha") "," HOME_AT("second.example", "beta") ",{\"url\":\"" SSH_URL "\"}", NULL);
    add_repo(NULL, 10, "alpha", "old-lib", both);
    add_repo("second.example", 20, "beta", "lib", both);
    fails_cleanly_in_a_session(NULL, 0, "forge:10", NAULA_OK, "without a cache");
    fresh_cache("out-of-memory", cache, sizeof cache);
    fails_cleanly_in_a_session(cache, 0, "forge:10", NAULA_OK, "with a cache");
    out_of_memory_on_the_cache_paths();
    out_of_memory_on_the_web_and_fork_paths();

    naula_session_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.cache_directory = cache;
    naula_memory_fail_at(0);
    naula_session *session = NULL;
    CHECK(naula_session_open(&options, &session, NULL) == NAULA_OK);
    uint64_t attempts = naula_memory_attempts();
    naula_session_close(session);
    for (uint64_t attempt = 1; attempt <= attempts; attempt++) {
        session = NULL;
        naula_memory_fail_at(attempt);
        naula_status status = naula_session_open(&options, &session, NULL);
        naula_memory_fail_at(0);
        CHECK(status == NAULA_OUT_OF_MEMORY && session == NULL);
        naula_session_close(session);
    }
}

int main(void) {
    naula_json_use_allocator();
    naula_fetch_allow_plain_http(1);
    CHECK(naula_providers_open(&registry) == NAULA_OK);
    CHECK(naula_providers_register(registry, &fake_provider, NULL) == NAULA_OK);
    confirms_a_single_home_from_at();
    confirms_a_second_home_listed_both_ways();
    refuses_a_listed_home_that_does_not_list_back();
    refuses_a_github_fork_whose_parent_confirms_the_id();
    refuses_a_hostile_copy_listing_the_original();
    never_reaches_a_self_listing_copy_from_at();
    rename_keeps_resolving_by_number_and_warns_pending();
    planned_move_follows_old_to_new_and_caches_it();
    follows_primary_pointers_and_refuses_a_cycle();
    refuses_a_home_whose_number_changed_since_the_cache();
    resolve_treats_a_timeout_as_unknown_not_dead();
    skips_a_dead_listed_home_and_reports_it();
    marks_a_web_only_repo_domain_only();
    ssh_home_is_unverifiable_never_confirmed();
    answers_stale_from_cache_offline();
    primary_change_needs_accept();
    fork_exception_confirms_a_stated_non_root_id();
    sha256_id_resolves();
    reads_repo_json_from_the_default_branch_without_development();
    refuses_a_local_id_and_a_url_reference();
    a_cache_that_fails_to_parse_is_absent_and_reported();
    session_refuses_options_smaller_than_their_first_shape();
    stale_serves_only_homes_confirmed_last_and_not_refused();
    a_cached_home_never_confirms_on_its_own();
    matches_a_listed_home_only_to_one_holding_the_id();
    the_written_record_is_always_readable();
    a_development_branch_without_repo_json_is_not_listed();
    a_writer_removes_temporary_files_left_behind();
    a_record_past_max_path_is_written();
    the_anchor_is_the_sorted_places_hashed();
    another_consumers_first_contact_never_shapes_this_answer();
    a_number_that_answered_gone_is_asked_again();
    a_dead_at_starts_from_the_new_home_the_record_keeps();
    the_anchor_comes_first_in_the_confirmed_homes();
    places_naming_one_home_share_one_record();
    an_edited_at_keeps_its_lineage();
    a_first_contact_elsewhere_never_accepts_a_number_here(0);
    a_first_contact_elsewhere_never_accepts_a_number_here(1);
    a_copys_homes_never_evict_what_protects_an_edited_at();
    a_copys_record_never_sets_a_baseline();
    a_refusal_never_lifts_a_lineage_guard_whatever_the_order();
    a_home_refused_before_it_is_read_is_released_and_unheard();
    a_settled_home_read_later_releases_its_file();
    a_later_first_contact_never_denies_an_edited_at(1);
    a_later_first_contact_never_denies_an_edited_at(0);
    a_wrong_clock_never_settles_a_re_registered_number();
    records_that_disagree_on_the_primary_ask();
    a_record_for_another_at_is_ignored_and_kept();
    one_run_of_404s_never_demotes_a_confirmed_home();
    a_relative_cache_folder_is_refused();
#if defined(_WIN32)
    windows_cache_folders_in_every_form();
#endif
    out_of_memory_is_answered_at_every_allocation();
    naula_providers_close(registry);
    return check_summary("resolve_test");
}
