#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define FAKE_REPO_LIMIT 16
#define FAKE_TEXT_LIMIT 24
#define FAKE_TEXT_SIZE 1024
#define FAKE_LOG_SIZE 16384

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
} fake_repo;

typedef struct fake_forge {
    fake_repo repos[FAKE_REPO_LIMIT];
    size_t count;
    char texts[FAKE_TEXT_LIMIT][FAKE_TEXT_SIZE];
    size_t text_count;
    char log[FAKE_LOG_SIZE];
    size_t log_length;
    int wrong_tokens;
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
        snprintf(text, FAKE_TEXT_SIZE, "{\"format\":2,\"name\":\"%s\",\"importName\":\"%s\",\"homes\":[%s]}", name, name,
                 homes);
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
    if (repo == NULL || repo->gone) {
        return fake_refuse(refusal, NAULA_NOT_FOUND, "not_found");
    }
    if (repo->times_out) {
        return fake_refuse(refusal, NAULA_TIMED_OUT, "timed_out");
    }
    return NAULA_OK;
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
    if (repo->holds == NULL) {
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

static const naula_provider fake_provider = {sizeof(naula_provider), "forge",          NULL,
                                             fake_describe,          fake_lookup,      fake_find,
                                             fake_read_file,         fake_contains,    fake_parent_count,
                                             fake_list_repos,        fake_release_index};

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

static void forget_cache(const char *directory, const char *hex) {
    char path[1024];
    snprintf(path, sizeof path, "%s/identities/%s.json", directory, hex);
    remove(path);
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
    CHECK(naula_session_open(&options, &session) == NAULA_OK);
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
    FILE *file = fopen(path, "rb");
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
    FILE *file = fopen(path, "wb");
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
    forget_cache(cache, X);
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
    snprintf(path, sizeof path, "%s/identities/%s.json", cache, X);
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
    forget_cache(cache, X);
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
    forget_cache(cache, X);
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
    add_repo("two.example", 20, "alpha", "lib", repo_file("lib", HOME("alpha") "," HOME_AT("two.example", "alpha"), NULL));
    CHECK(resolve_once(NULL, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "cycle", "pointers that loop");
    CHECK(resolution.home_count == 0);
    naula_resolution_release(&resolution);
}

static void refuses_a_home_whose_number_changed_since_the_cache(void) {
    char cache[512];
    cache_directory("number-changed", cache, sizeof cache);
    forget_cache(cache, X);
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
}

static void resolve_treats_a_timeout_as_unknown_not_dead(void) {
    char cache[512];
    cache_directory("timeout", cache, sizeof cache);
    forget_cache(cache, X);
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
    forget_cache(cache, X);
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
    forget_cache(cache, X);
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
    forget_cache(empty, X);
    offline = open_session(empty, 1, NULL);
    CHECK(resolve_from(offline, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_UNREACHABLE);
    check_text(refusal.code, "unresolvable", "offline with nothing cached");
    naula_resolution_release(&resolution);
    naula_session_close(offline);
}

static void primary_change_needs_accept(void) {
    char cache[512];
    cache_directory("primary-change", cache, sizeof cache);
    forget_cache(cache, X);
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
    forget_cache(cache, X256);
    forge_reset();
    fake_repo *repo = add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    repo->holds = X256;
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(cache, "git:" X256, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    CHECK(resolution.home_count == 1);
    naula_resolution_release(&resolution);
    snprintf(path, sizeof path, "%s/identities/%s.json", cache, X256);
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
    forget_cache(cache, X);
    forge_reset();
    add_repo(NULL, 10, "alpha", "lib", repo_file("lib", HOME("alpha"), NULL));
    naula_resolution resolution;
    naula_refusal refusal;
    CHECK(resolve_once(cache, ID, "forge:10", NULL, &resolution, &refusal) == NAULA_OK);
    naula_resolution_release(&resolution);
    snprintf(path, sizeof path, "%s/identities/%s.json", cache, X);
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

static void out_of_memory_scenario(void) {
    forge_reset();
    const char *both = repo_file("lib", HOME("alpha") "," HOME_AT("second.example", "beta") ",{\"url\":\"" SSH_URL "\"}",
                                 NULL);
    add_repo(NULL, 10, "alpha", "old-lib", both);
    add_repo("second.example", 20, "beta", "lib", both);
}

static naula_status resolve_counted(naula_session *session, const naula_reference *reference,
                                    naula_resolution *resolution, uint64_t fail_at) {
    naula_refusal refusal = fresh_refusal();
    start_resolution(resolution);
    naula_memory_fail_at(fail_at);
    naula_status status = naula_resolve(session, reference, resolution, &refusal);
    return status;
}

static void fails_cleanly_at_every_allocation(const char *cache, const char *label) {
    naula_session *session = open_session(cache, 0, NULL);
    naula_place place;
    memset(&place, 0, sizeof place);
    place.size = sizeof place;
    CHECK(naula_place_parse("forge:10", &place, NULL) == NAULA_OK);
    naula_reference reference;
    memset(&reference, 0, sizeof reference);
    reference.size = sizeof reference;
    reference.id = ID;
    reference.at = &place;
    reference.at_count = 1;
    naula_resolution resolution;
    CHECK(resolve_counted(session, &reference, &resolution, 0) == NAULA_OK);
    naula_resolution_release(&resolution);
    CHECK(resolve_counted(session, &reference, &resolution, 0) == NAULA_OK);
    uint64_t attempts = naula_memory_attempts();
    if (resolution.home_count != 2 || resolution.cache_problem[0] != '\0') {
        fprintf(stderr, "%s: %zu homes, cache problem \"%s\"\n", label, resolution.home_count,
                resolution.cache_problem);
        for (size_t index = 0; index < resolution.verdict_count; index++) {
            fprintf(stderr, "  %llu %s %s\n", (unsigned long long)resolution.verdicts[index].home.number,
                    resolution.verdicts[index].code, resolution.verdicts[index].detail);
        }
        fprintf(stderr, "%s", forge.log);
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
    naula_place_release(&place);
    naula_session_close(session);
}

static void out_of_memory_is_answered_at_every_allocation(void) {
    char cache[512];
    out_of_memory_scenario();
    fails_cleanly_at_every_allocation(NULL, "without a cache");
    cache_directory("out-of-memory", cache, sizeof cache);
    forget_cache(cache, X);
    fails_cleanly_at_every_allocation(cache, "with a cache");

    naula_session_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.cache_directory = cache;
    naula_memory_fail_at(0);
    naula_session *session = NULL;
    CHECK(naula_session_open(&options, &session) == NAULA_OK);
    uint64_t attempts = naula_memory_attempts();
    naula_session_close(session);
    for (uint64_t attempt = 1; attempt <= attempts; attempt++) {
        session = NULL;
        naula_memory_fail_at(attempt);
        naula_status status = naula_session_open(&options, &session);
        naula_memory_fail_at(0);
        CHECK(status == NAULA_OUT_OF_MEMORY && session == NULL);
        naula_session_close(session);
    }
}

static void session_refuses_options_smaller_than_their_first_shape(void) {
    naula_session_options options;
    memset(&options, 0, sizeof options);
    options.size = 8;
    naula_session *session = NULL;
    CHECK(naula_session_open(&options, &session) == NAULA_REFUSED && session == NULL);
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
             "{\"id\":%u,\"name\":\"lib\",\"owner\":{\"login\":\"%s\"},\"default_branch\":\"main\",\"archived\":false%s}",
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
    github_home_fill(&original, 1001, "octo", 0, repo_file("lib", "{\"provider\":\"github\",\"account\":\"octo\"}", NULL));
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
    static const char *const document = "{\"id\":\"" ID "\",\"homes\":[{\"provider\":\"web\",\"domain\":\"naula.dev\"}]}";
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
    out_of_memory_is_answered_at_every_allocation();
    naula_providers_close(registry);
    return check_summary("resolve_test");
}
