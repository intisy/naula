#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "json_document.h"
#include "naula/id.h"
#include "naula/memory.h"
#include "resolve_internal.h"

#define FORK_DEPTH_LIMIT 8u
#define CANDIDATE_LIMIT 64u
#define PLACE_TEXT_SIZE 160
#define WORKING_BRANCH "development"
#define REPO_FILE "repo.json"

static naula_refusal fresh_refusal(void) {
    naula_refusal refusal;
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    return refusal;
}

static void settle(candidate *subject, candidate_outcome outcome, const char *code, const char *format, ...) {
    va_list arguments;
    subject->outcome = outcome;
    snprintf(subject->code, sizeof subject->code, "%s", code);
    va_start(arguments, format);
    vsnprintf(subject->detail, sizeof subject->detail, format, arguments);
    va_end(arguments);
}

int naula_resolver_equals_ignoring_case(const char *left, const char *right) {
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

/** GitHub's own host may be written or left out; any other provider's home always names its host. */
static const char *normal_host(const char *word, const char *host) {
    if (host != NULL && strcmp(word, "github") == 0 && naula_resolver_equals_ignoring_case(host, "github.com")) {
        return NULL;
    }
    return host;
}

int naula_resolver_same_host(const char *word, const char *left, const char *right) {
    return naula_resolver_equals_ignoring_case(normal_host(word, left), normal_host(word, right));
}

int naula_resolver_same_place(const naula_place *left, const char *left_account, const naula_place *right,
                              const char *right_account) {
    if (left->kind != right->kind) {
        return 0;
    }
    if (left->kind == NAULA_HOME_WEB) {
        return naula_resolver_equals_ignoring_case(left->domain, right->domain);
    }
    if (left->kind == NAULA_HOME_URL) {
        return strcmp(left->url, right->url) == 0;
    }
    return strcmp(left->provider, right->provider) == 0 &&
           naula_resolver_same_host(left->provider, left->host, right->host) && left->number == right->number &&
           (left->number != 0 || naula_resolver_equals_ignoring_case(left_account, right_account));
}

void naula_candidate_text(const candidate *subject, char *out, size_t size) {
    const naula_place *place = &subject->place;
    if (place->kind == NAULA_HOME_WEB) {
        snprintf(out, size, "web:%s", place->domain);
    } else if (place->kind == NAULA_HOME_URL) {
        snprintf(out, size, "%s", place->url);
    } else if (place->number == 0) {
        snprintf(out, size, "%s%s%s account %s", place->provider, place->host == NULL ? "" : "@",
                 place->host == NULL ? "" : place->host, subject->account == NULL ? "" : subject->account);
    } else {
        snprintf(out, size, "%s%s%s:%llu", place->provider, place->host == NULL ? "" : "@",
                 place->host == NULL ? "" : place->host, (unsigned long long)place->number);
    }
}

const naula_home *naula_candidate_homes(const candidate *subject, size_t *count) {
    if (subject->place.kind == NAULA_HOME_WEB) {
        *count = subject->document_home_count;
        return subject->document_homes;
    }
    *count = subject->file.home_count;
    return subject->file.homes;
}

int naula_home_names(const naula_home *home, const candidate *subject) {
    if (subject->place.kind == NAULA_HOME_WEB) {
        return home->kind == NAULA_HOME_WEB && naula_resolver_equals_ignoring_case(home->domain, subject->place.domain);
    }
    if (subject->place.kind != NAULA_HOME_PROVIDER || subject->repository.account == NULL) {
        return 0;
    }
    return home->kind == NAULA_HOME_PROVIDER && strcmp(home->provider, subject->place.provider) == 0 &&
           naula_resolver_same_host(home->provider, home->host, subject->place.host) &&
           naula_resolver_equals_ignoring_case(home->account, subject->repository.account);
}

static int lists(const naula_home *homes, size_t count, const candidate *subject) {
    for (size_t index = 0; index < count; index++) {
        if (naula_home_names(&homes[index], subject)) {
            return 1;
        }
    }
    return 0;
}

static char *copy_text(const char *text) {
    return text == NULL ? NULL : naula_memory_duplicate_text(text);
}

naula_status naula_resolver_copy_place(const naula_place *from, naula_place *to) {
    memset(to, 0, sizeof *to);
    to->size = sizeof *to;
    to->kind = from->kind;
    to->number = from->number;
    to->provider = copy_text(from->provider);
    to->host = copy_text(from->host);
    to->domain = copy_text(from->domain);
    to->url = copy_text(from->url);
    if ((from->provider != NULL && to->provider == NULL) || (from->host != NULL && to->host == NULL) ||
        (from->domain != NULL && to->domain == NULL) || (from->url != NULL && to->url == NULL)) {
        naula_place_release(to);
        return NAULA_OUT_OF_MEMORY;
    }
    return NAULA_OK;
}

static void release_candidate(candidate *subject) {
    naula_place_release(&subject->place);
    naula_memory_release(subject->account);
    naula_repository_release(&subject->repository);
    naula_memory_release(subject->branch);
    naula_repo_file_release(&subject->file);
    for (size_t index = 0; index < subject->document_home_count; index++) {
        naula_home_release(&subject->document_homes[index]);
    }
    naula_memory_release(subject->document_homes);
    naula_memory_release(subject->listed);
    naula_memory_release(subject);
}

static naula_status new_candidate(const naula_place *place, const char *account, candidate **made) {
    candidate *subject = naula_memory_allocate(sizeof *subject);
    *made = NULL;
    if (subject == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    memset(subject, 0, sizeof *subject);
    subject->repository.size = sizeof subject->repository;
    subject->file.size = sizeof subject->file;
    if (naula_resolver_copy_place(place, &subject->place) != NAULA_OK) {
        naula_memory_release(subject);
        return NAULA_OUT_OF_MEMORY;
    }
    if (place->kind == NAULA_HOME_PROVIDER && place->number == 0) {
        subject->account = copy_text(account);
        if (subject->account == NULL) {
            release_candidate(subject);
            return NAULA_OUT_OF_MEMORY;
        }
    }
    *made = subject;
    return NAULA_OK;
}

naula_status naula_resolver_add(resolver *subject, const naula_place *place, const char *account, size_t *index) {
    for (size_t existing = 0; existing < subject->count; existing++) {
        const candidate *other = subject->candidates[existing];
        if (naula_resolver_same_place(&other->place, other->account, place, account)) {
            *index = existing;
            return NAULA_OK;
        }
    }
    if (subject->count >= CANDIDATE_LIMIT) {
        return NAULA_REFUSED;
    }
    if (subject->count == subject->capacity) {
        size_t capacity = subject->capacity == 0 ? 8 : subject->capacity * 2;
        candidate **grown = naula_memory_reallocate(subject->candidates, capacity * sizeof *grown);
        if (grown == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        subject->candidates = grown;
        subject->capacity = capacity;
    }
    candidate *made = NULL;
    naula_status status = new_candidate(place, account, &made);
    if (status != NAULA_OK) {
        return status;
    }
    subject->candidates[subject->count] = made;
    *index = subject->count++;
    return NAULA_OK;
}

void naula_resolver_release_candidates(resolver *subject) {
    for (size_t index = 0; index < subject->count; index++) {
        release_candidate(subject->candidates[index]);
    }
    naula_memory_release(subject->candidates);
    subject->candidates = NULL;
    subject->count = 0;
    subject->capacity = 0;
}

static candidate_outcome failure_outcome(naula_status status, const char **code) {
    switch (status) {
    case NAULA_NOT_FOUND:
        *code = "dead-skipped";
        return CANDIDATE_DEAD;
    case NAULA_TIMED_OUT:
        *code = "timed-out";
        return CANDIDATE_UNKNOWN;
    case NAULA_UNREACHABLE:
        *code = "unreachable";
        return CANDIDATE_UNKNOWN;
    default:
        *code = "unverifiable";
        return CANDIDATE_UNKNOWN;
    }
}

/** Settles a failed read or history call; only NAULA_OUT_OF_MEMORY stops the resolver. */
static naula_status settle_failure(candidate *subject, naula_status status, const naula_refusal *refusal) {
    if (status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    const char *code = NULL;
    candidate_outcome outcome = failure_outcome(status, &code);
    settle(subject, outcome, code, "%s: %s", refusal->code, refusal->detail);
    return NAULA_OK;
}

static int refused_as(const naula_refusal *refusal, const char *code) {
    return strcmp(refusal->code, code) == 0;
}

/**
 * Whether the home has a development branch, asked of its history: a ref it does not have answers
 * `missing_commit_or_ref`, while a branch that exists answers whether it holds the id either way.
 */
static naula_status has_working_branch(resolver *subject, candidate *home, int *exists, naula_refusal *refusal) {
    const naula_history *history = &subject->session->history;
    int contained = 0;
    *exists = 0;
    naula_status status =
        history->contains(history->context, &home->place, WORKING_BRANCH, subject->hex, &contained, refusal);
    if (status == NAULA_REFUSED && refused_as(refusal, "missing_commit_or_ref")) {
        return NAULA_OK;
    }
    *exists = status == NAULA_OK;
    return status;
}

/**
 * Reads repo.json from development, or from the default branch only when there is no development branch: a
 * development branch without the file states nothing, and reading another branch then would let a stale default
 * branch speak for the home. Settles the home when no file is read.
 */
static naula_status read_repo_file(resolver *subject, const naula_provider *provider, const char *token,
                                   candidate *home, uint8_t **bytes, size_t *length) {
    naula_refusal refusal = fresh_refusal();
    const char *branch = WORKING_BRANCH;
    naula_status status = provider->read_file(provider->context, home->place.host, home->place.number, branch,
                                              REPO_FILE, token, bytes, length, &refusal);
    const char *fallback = home->repository.default_branch;
    if (status == NAULA_REFUSED && refused_as(&refusal, "missing_file_or_ref")) {
        int exists = 0;
        naula_refusal asked = fresh_refusal();
        naula_status branch_status = has_working_branch(subject, home, &exists, &asked);
        if (branch_status != NAULA_OK) {
            return settle_failure(home, branch_status, &asked);
        }
        if (exists || fallback == NULL || strcmp(fallback, WORKING_BRANCH) == 0) {
            settle(home, CANDIDATE_REFUSED, "not-listed", "its working branch holds no repo.json");
            return NAULA_OK;
        }
        refusal = fresh_refusal();
        branch = fallback;
        status = provider->read_file(provider->context, home->place.host, home->place.number, branch, REPO_FILE,
                                     token, bytes, length, &refusal);
    }
    if (status == NAULA_REFUSED && refused_as(&refusal, "missing_file_or_ref")) {
        settle(home, CANDIDATE_REFUSED, "not-listed", "its working branch holds no repo.json");
        return NAULA_OK;
    }
    if (status != NAULA_OK) {
        return settle_failure(home, status, &refusal);
    }
    home->branch = copy_text(branch);
    if (home->branch == NULL) {
        naula_memory_release(*bytes);
        *bytes = NULL;
        return NAULA_OUT_OF_MEMORY;
    }
    return NAULA_OK;
}

static naula_status read_provider_home(resolver *subject, candidate *home) {
    naula_refusal refusal = fresh_refusal();
    const naula_provider *provider = naula_providers_find(subject->session->providers, home->place.provider, &refusal);
    if (provider == NULL) {
        settle(home, CANDIDATE_DEAD, "dead-skipped", "no provider is installed for %s: %s", home->place.provider,
               NAULA_UNKNOWN_PROVIDER_FIX);
        return NAULA_OK;
    }
    const char *token = naula_session_token(subject->session, home->place.provider, home->place.host);
    naula_status status = provider->lookup(provider->context, home->place.host, home->place.number, token,
                                           &home->repository, &refusal);
    if (status != NAULA_OK) {
        return settle_failure(home, status, &refusal);
    }
    if (home->repository.number != home->place.number) {
        settle(home, CANDIDATE_REFUSED, "number-changed", "its provider answered repository %llu",
               (unsigned long long)home->repository.number);
        return NAULA_OK;
    }
    uint8_t *bytes = NULL;
    size_t length = 0;
    status = read_repo_file(subject, provider, token, home, &bytes, &length);
    if (status != NAULA_OK || home->outcome != CANDIDATE_PENDING) {
        naula_memory_release(bytes);
        return status;
    }
    status = naula_repo_file_parse(bytes, length, &home->file, &refusal);
    naula_memory_release(bytes);
    if (status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    if (status != NAULA_OK) {
        settle(home, CANDIDATE_REFUSED, "not-listed", "its repo.json cannot be read (%s): %s", refusal.code,
               refusal.detail);
        return NAULA_OK;
    }
    home->readable = 1;
    return NAULA_OK;
}

static naula_status read_document_homes(candidate *home, const cJSON *list, int *readable) {
    int count = cJSON_GetArraySize(list);
    *readable = 0;
    home->document_homes = count > 0 ? naula_memory_allocate((size_t)count * sizeof *home->document_homes) : NULL;
    if (count > 0 && home->document_homes == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    for (const cJSON *item = list->child; item != NULL; item = item->next) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        naula_home *next = &home->document_homes[home->document_home_count];
        memset(next, 0, sizeof *next);
        next->size = sizeof *next;
        naula_refusal refusal = fresh_refusal();
        naula_status status = naula_home_from_json(item, next, &refusal);
        if (status != NAULA_OK) {
            return status == NAULA_OUT_OF_MEMORY ? status : NAULA_OK;
        }
        home->document_home_count++;
    }
    *readable = 1;
    return NAULA_OK;
}

static naula_status read_web_home(resolver *subject, candidate *home) {
    naula_refusal refusal = fresh_refusal();
    uint8_t *bytes = NULL;
    size_t length = 0;
    naula_status status = naula_web_document(home->place.domain, subject->hex, &bytes, &length, &refusal);
    if (status == NAULA_NOT_FOUND || status == NAULA_MALFORMED || (status == NAULA_REFUSED &&
                                                                    refused_as(&refusal, "document_for_another_id"))) {
        settle(home, CANDIDATE_REFUSED, "not-listed", "the domain serves no identity document for this id: %s",
               refusal.detail);
        return NAULA_OK;
    }
    if (status != NAULA_OK) {
        return settle_failure(home, status, &refusal);
    }
    cJSON *json = NULL;
    const char *reason = NULL;
    status = naula_json_parse(bytes, length, &json, &reason);
    naula_memory_release(bytes);
    if (status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    int readable = 0;
    const cJSON *list = json == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(json, "homes");
    if (cJSON_IsArray(list)) {
        status = read_document_homes(home, list, &readable);
    }
    cJSON_Delete(json);
    if (status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    if (!readable) {
        settle(home, CANDIDATE_REFUSED, "not-listed", "the domain's identity document lists a home naula cannot read");
        return NAULA_OK;
    }
    home->readable = 1;
    return NAULA_OK;
}

/** Reads what a home says of itself, once. A failure settles it; only NAULA_OUT_OF_MEMORY is answered. */
static naula_status read_candidate(resolver *subject, candidate *home) {
    if (home->read) {
        return NAULA_OK;
    }
    home->read = 1;
    if (home->place.kind == NAULA_HOME_WEB) {
        return read_web_home(subject, home);
    }
    if (home->place.kind == NAULA_HOME_PROVIDER && home->place.number != 0) {
        return read_provider_home(subject, home);
    }
    return NAULA_OK;
}

static void settle_unreadable_kind(candidate *home) {
    if (home->place.kind == NAULA_HOME_URL) {
        settle(home, CANDIDATE_UNKNOWN, "unverifiable", "a %s home cannot be read without git",
               strncmp(home->place.url, "ssh://", 6) == 0 ? "ssh" : "url");
    } else {
        settle(home, CANDIDATE_UNKNOWN, "unverifiable", "no number was ever learned for this home");
    }
}

static naula_status check_stated_id(resolver *subject, candidate *home, int *stated) {
    naula_id id;
    memset(&id, 0, sizeof id);
    id.size = sizeof id;
    naula_status status = naula_id_parse(home->file.id, strlen(home->file.id), &id, NULL);
    if (status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    if (status == NAULA_OK && id.kind == NAULA_ID_GIT && id.segment_count == 0 &&
        strcmp(id.commit, subject->hex) == 0) {
        *stated = 1;
    } else {
        settle(home, CANDIDATE_REFUSED, "states-another-id", "its repo.json states the id %s", home->file.id);
    }
    naula_id_release(&id);
    return NAULA_OK;
}

static int is_missing_commit(naula_status status, const naula_refusal *refusal) {
    return status == NAULA_REFUSED &&
           (refused_as(refusal, "missing_commit_or_ref") || refused_as(refusal, "missing_commit"));
}

static naula_status check_history(resolver *subject, candidate *home, int stated) {
    const naula_history *history = &subject->session->history;
    naula_refusal refusal = fresh_refusal();
    int contained = 0;
    const char *ref = home->place.kind == NAULA_HOME_PROVIDER ? home->branch : NULL;
    naula_status status = history->contains(history->context, &home->place, ref, subject->hex, &contained, &refusal);
    if (is_missing_commit(status, &refusal)) {
        status = NAULA_OK;
        contained = 0;
    }
    if (status != NAULA_OK) {
        return settle_failure(home, status, &refusal);
    }
    home->holds_id = contained;
    if (!contained) {
        settle(home, CANDIDATE_REFUSED, "not-in-history", "its working branch does not hold %s", subject->hex);
        return NAULA_OK;
    }
    if (stated) {
        return NAULA_OK;
    }
    size_t parents = 0;
    refusal = fresh_refusal();
    status = history->parent_count(history->context, &home->place, subject->hex, &parents, &refusal);
    if (is_missing_commit(status, &refusal)) {
        settle(home, CANDIDATE_REFUSED, "not-in-history", "it does not hold %s", subject->hex);
        return NAULA_OK;
    }
    if (status != NAULA_OK) {
        return settle_failure(home, status, &refusal);
    }
    if (parents > 0) {
        settle(home, CANDIDATE_REFUSED, "has-parents",
               "%s has parents, and its repo.json does not state it as the repo's own id", subject->hex);
    }
    return NAULA_OK;
}

static naula_status check_fork_parent(resolver *subject, candidate *home, unsigned depth) {
    char text[PLACE_TEXT_SIZE];
    if (depth >= FORK_DEPTH_LIMIT) {
        settle(home, CANDIDATE_UNKNOWN, "unverifiable", "its fork parents run deeper than %u", FORK_DEPTH_LIMIT);
        return NAULA_OK;
    }
    naula_place parent = home->place;
    parent.number = home->repository.fork_parent;
    size_t index = 0;
    naula_status status = naula_resolver_add(subject, &parent, NULL, &index);
    if (status == NAULA_REFUSED) {
        settle(home, CANDIDATE_UNKNOWN, "unverifiable", "its fork parent could not be weighed: too many homes");
        return NAULA_OK;
    }
    if (status != NAULA_OK) {
        return status;
    }
    candidate *fork_parent = subject->candidates[index];
    if (!fork_parent->wanted) {
        fork_parent->probe = 1;
    }
    status = naula_resolver_confirm(subject, index, depth + 1);
    if (status != NAULA_OK) {
        return status;
    }
    naula_candidate_text(fork_parent, text, sizeof text);
    if (fork_parent->outcome == CANDIDATE_CONFIRMED) {
        settle(home, CANDIDATE_REFUSED, "copy-of", "it is a fork of %s, which confirms this id", text);
    } else if (fork_parent->outcome == CANDIDATE_REFUSED && strcmp(fork_parent->code, "copy-of") == 0) {
        settle(home, CANDIDATE_REFUSED, "copy-of", "it is a fork of %s, itself a copy of this id", text);
    } else if (fork_parent->outcome == CANDIDATE_UNKNOWN || fork_parent->outcome == CANDIDATE_PENDING) {
        settle(home, CANDIDATE_UNKNOWN, fork_parent->outcome == CANDIDATE_UNKNOWN ? fork_parent->code : "unverifiable",
               "its fork parent %s could be neither confirmed nor ruled out: %s", text, fork_parent->detail);
    }
    return NAULA_OK;
}

static int guards_account(const cache_home *cached, const naula_home *home) {
    return cached->place.kind == NAULA_HOME_PROVIDER && strcmp(cached->place.provider, home->provider) == 0 &&
           naula_resolver_same_host(home->provider, cached->place.host, home->host) &&
           naula_resolver_equals_ignoring_case(cached->account, home->account);
}

/** The entries of `record` for a listed account: 0 none, 1 all one `number`, 2 more than one number. */
static int record_number(const identity_cache *record, const naula_home *home, uint64_t *number) {
    int found = 0;
    for (size_t index = 0; index < record->count; index++) {
        const cache_home *cached = &record->homes[index];
        if (!guards_account(cached, home)) {
            continue;
        }
        if (found && cached->place.number != *number) {
            return 2;
        }
        *number = cached->place.number;
        found = 1;
    }
    return found;
}

/**
 * The number a listed account was confirmed at: this anchor's own record's, its confirmed home before its guards,
 * or with none, the one every lineage record that holds the account agrees on. The lineage is fixed before any home
 * is weighed, and its numbers refuse a different number for the whole run whatever else the run refuses. `disagree`
 * is set when the record's guards or the lineage's records hold different numbers, which refuses every number for
 * the account, failing closed.
 */
static int cache_number(const resolver *subject, const naula_home *home, uint64_t *number, int *disagree) {
    *disagree = 0;
    if (subject->cache.present) {
        for (size_t index = 0; index < subject->cache.count; index++) {
            const cache_home *cached = &subject->cache.homes[index];
            if (guards_account(cached, home) && cached->state == CACHE_CONFIRMED) {
                *number = cached->place.number;
                return 1;
            }
        }
        int holds = record_number(&subject->cache, home, number);
        *disagree = holds == 2;
        return holds != 0;
    }
    int known = 0;
    for (size_t index = 0; index < subject->lineage_count; index++) {
        uint64_t held = 0;
        int holds = record_number(&subject->lineage[index], home, &held);
        if (holds == 2 || (holds == 1 && known && held != *number)) {
            *disagree = 1;
        }
        if (holds == 1 && !known) {
            *number = held;
            known = 1;
        }
    }
    return known || *disagree;
}

/**
 * A home already reached by number that answers to the listed account and confirms or holds the id: for a provider
 * lister, one whose repo.json states the lister's name, which finds a home whose host still carries an old name.
 */
static uint64_t reached_number(const resolver *subject, const naula_home *home, const char *name) {
    for (size_t index = 0; index < subject->count; index++) {
        const candidate *other = subject->candidates[index];
        int trusted = (other->outcome == CANDIDATE_CONFIRMED && !other->probe) || other->holds_id;
        int fits = trusted && (name == NULL || naula_resolver_equals_ignoring_case(other->file.name, name));
        if (fits && naula_home_names(home, other)) {
            return other->place.number;
        }
    }
    return 0;
}

static naula_place home_place(const naula_home *home, uint64_t number) {
    naula_place place;
    memset(&place, 0, sizeof place);
    place.size = sizeof place;
    place.kind = home->kind;
    place.provider = home->provider;
    place.host = home->host;
    place.number = number;
    place.domain = home->domain;
    place.url = home->url;
    return place;
}

/** Adds the candidate a listed home names, settling a new one as `outcome` unless that is CANDIDATE_PENDING. */
static naula_status note_home(resolver *subject, const naula_home *home, uint64_t number, candidate_outcome outcome,
                              const char *code, const char *detail, size_t *index) {
    naula_place place = home_place(home, number);
    naula_status status = naula_resolver_add(subject, &place, home->account, index);
    if (status == NAULA_REFUSED) {
        *index = NAULA_NO_CANDIDATE;
        return NAULA_OK;
    }
    if (status == NAULA_OK && outcome != CANDIDATE_PENDING &&
        subject->candidates[*index]->outcome == CANDIDATE_PENDING) {
        settle(subject->candidates[*index], outcome, code, "%s", detail);
    }
    return status;
}

/**
 * The number of a home `lister` lists by account: the one this anchor's record holds for that account, confirmed or
 * kept as a guard, else one already reached by number, else the one its provider finds under the account
 * and `lister`'s name. A number found that differs from the cached one is refused as `number-changed`, so a
 * re-registered account never stands in for the home it replaced. When neither is known, or the provider could not
 * be heard, `lister` is settled and 0 answered.
 */
static naula_status listed_number(resolver *subject, candidate *lister, const naula_home *home, uint64_t *number) {
    char detail[NAULA_RESOLVE_DETAIL_SIZE];
    size_t index = 0;
    *number = 0;
    const naula_provider *provider = naula_providers_find(subject->session->providers, home->provider, NULL);
    if (provider == NULL) {
        snprintf(detail, sizeof detail, "no provider is installed for %s: %s", home->provider,
                 NAULA_UNKNOWN_PROVIDER_FIX);
        return note_home(subject, home, 0, CANDIDATE_DEAD, "dead-skipped", detail, &index);
    }
    uint64_t cached = 0;
    int disagree = 0;
    int known = cache_number(subject, home, &cached, &disagree);
    const char *name = lister->place.kind == NAULA_HOME_PROVIDER ? lister->file.name : NULL;
    if (!known) {
        *number = reached_number(subject, home, name);
    }
    if (*number != 0) {
        return NAULA_OK;
    }
    if (name == NULL && !known) {
        settle(lister, CANDIDATE_UNKNOWN, "unverifiable", "it lists %s account %s, which no confirmed home is",
               home->provider, home->account);
        return NAULA_OK;
    }
    uint64_t found = 0;
    if (name != NULL) {
        naula_refusal refusal = fresh_refusal();
        naula_status status = provider->find(provider->context, home->host, home->account, name,
                                             naula_session_token(subject->session, home->provider, home->host), &found,
                                             &refusal);
        if (status == NAULA_OUT_OF_MEMORY) {
            return status;
        }
        if (status != NAULA_OK && !known) {
            const char *code = NULL;
            candidate_outcome outcome = failure_outcome(status, &code);
            settle(lister, outcome == CANDIDATE_DEAD ? CANDIDATE_UNKNOWN : outcome, code,
                   "the home it lists at %s account %s could not be found: %s", home->provider, home->account,
                   refusal.detail);
            return NAULA_OK;
        }
        if (status != NAULA_OK) {
            found = 0;
        }
    }
    if (known && disagree) {
        if (found != 0) {
            snprintf(detail, sizeof detail, "the records %s/%s was confirmed in before hold different numbers",
                     home->account, name);
            naula_status status =
                note_home(subject, home, found, CANDIDATE_REFUSED, "number-changed", detail, &index);
            if (status != NAULA_OK) {
                return status;
            }
        }
        settle(lister, CANDIDATE_UNKNOWN, "unverifiable",
               "the records this at grew from hold different numbers for %s account %s", home->provider,
               home->account);
        return NAULA_OK;
    }
    if (known) {
        if (found != 0 && found != cached) {
            snprintf(detail, sizeof detail, "%s/%s is repository %llu now, not %llu as confirmed before", home->account,
                     name, (unsigned long long)found, (unsigned long long)cached);
            naula_status status =
                note_home(subject, home, found, CANDIDATE_REFUSED, "number-changed", detail, &index);
            if (status != NAULA_OK) {
                return status;
            }
        }
        *number = cached;
        return NAULA_OK;
    }
    if (found == 0) {
        snprintf(detail, sizeof detail, "no repository %s is under %s, and none was confirmed there before", name,
                 home->account);
        naula_status status = note_home(subject, home, 0, CANDIDATE_UNKNOWN, "unverifiable", detail, &index);
        if (status == NAULA_OK) {
            settle(lister, CANDIDATE_UNKNOWN, "unverifiable", "the home it lists at %s account %s cannot be found",
                   home->provider, home->account);
        }
        return status;
    }
    *number = found;
    return NAULA_OK;
}

static void disown(candidate *lister, const candidate *home, const char *reason) {
    char text[PLACE_TEXT_SIZE];
    naula_candidate_text(home, text, sizeof text);
    settle(lister, CANDIDATE_REFUSED, "disowned-by", "%s, which it lists, %s", text, reason);
}

/**
 * Whether the home `lister` lists at `entry` lists `lister` back. A home that is gone is skipped; one that cannot be
 * heard, or that this run already refused, leaves `lister` unknown, never confirmed: treating it as gone would let
 * anyone who can slow one home silence the home that would disown a copy, and a refused home neither lists nor
 * disowns.
 */
static naula_status check_listed_home(resolver *subject, candidate *lister, size_t entry) {
    size_t count = 0;
    const naula_home *home = &naula_candidate_homes(lister, &count)[entry];
    size_t index = NAULA_NO_CANDIDATE;
    naula_status status = NAULA_OK;
    if (home->kind == NAULA_HOME_URL) {
        status = note_home(subject, home, 0, CANDIDATE_PENDING, NULL, NULL, &index);
        if (status == NAULA_OK && index != NAULA_NO_CANDIDATE &&
            subject->candidates[index]->outcome == CANDIDATE_PENDING) {
            settle_unreadable_kind(subject->candidates[index]);
        }
        lister->listed[entry] = index;
        return status;
    }
    uint64_t number = 0;
    if (home->kind == NAULA_HOME_PROVIDER) {
        status = listed_number(subject, lister, home, &number);
        if (status != NAULA_OK || lister->outcome != CANDIDATE_PENDING || number == 0) {
            return status;
        }
    }
    status = note_home(subject, home, number, CANDIDATE_PENDING, NULL, NULL, &index);
    if (status != NAULA_OK) {
        return status;
    }
    if (index == NAULA_NO_CANDIDATE) {
        settle(lister, CANDIDATE_UNKNOWN, "unverifiable", "it lists more homes than are weighed");
        return NAULA_OK;
    }
    lister->listed[entry] = index;
    candidate *listed = subject->candidates[index];
    status = read_candidate(subject, listed);
    if (status != NAULA_OK) {
        return status;
    }
    size_t listed_count = 0;
    const naula_home *listed_homes = naula_candidate_homes(listed, &listed_count);
    char text[PLACE_TEXT_SIZE];
    naula_candidate_text(listed, text, sizeof text);
    if (listed->outcome == CANDIDATE_REFUSED) {
        settle(lister, CANDIDATE_UNKNOWN, "unverifiable", "%s, which it lists, was refused in this run: %s", text,
               listed->detail);
    } else if (listed->readable) {
        if (!lists(listed_homes, listed_count, lister)) {
            disown(lister, listed, "does not list it back");
        }
    } else if (listed->outcome == CANDIDATE_UNKNOWN) {
        settle(lister, CANDIDATE_UNKNOWN, listed->code, "%s, which it lists, could not be heard: %s", text,
               listed->detail);
    }
    return NAULA_OK;
}

static naula_status check_both_ways(resolver *subject, candidate *home) {
    size_t count = 0;
    const naula_home *homes = naula_candidate_homes(home, &count);
    if (count > 0) {
        home->listed = naula_memory_allocate(count * sizeof *home->listed);
        if (home->listed == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
    }
    for (size_t entry = 0; entry < count; entry++) {
        home->listed[entry] = NAULA_NO_CANDIDATE;
    }
    for (size_t entry = 0; entry < count && home->outcome == CANDIDATE_PENDING; entry++) {
        if (naula_home_names(&homes[entry], home)) {
            continue;
        }
        naula_status status = check_listed_home(subject, home, entry);
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

static naula_status confirm_readable(resolver *subject, candidate *home, unsigned depth) {
    int stated = home->place.kind == NAULA_HOME_WEB;
    naula_status status = NAULA_OK;
    if (home->place.kind == NAULA_HOME_PROVIDER && home->file.id != NULL) {
        status = check_stated_id(subject, home, &stated);
    }
    size_t count = 0;
    const naula_home *homes = naula_candidate_homes(home, &count);
    if (status == NAULA_OK && home->outcome == CANDIDATE_PENDING && !lists(homes, count, home)) {
        settle(home, CANDIDATE_REFUSED, "not-listed", "its repo.json does not list it in homes");
    }
    if (status == NAULA_OK && home->outcome == CANDIDATE_PENDING) {
        status = check_history(subject, home, stated);
    }
    if (status == NAULA_OK && home->outcome == CANDIDATE_PENDING && home->place.kind == NAULA_HOME_PROVIDER &&
        home->repository.fork_parent != 0) {
        status = check_fork_parent(subject, home, depth);
    }
    if (status == NAULA_OK && home->outcome == CANDIDATE_PENDING) {
        status = check_both_ways(subject, home);
    }
    if (status == NAULA_OK && home->outcome == CANDIDATE_PENDING) {
        int renamed = home->place.kind == NAULA_HOME_PROVIDER &&
                      !naula_resolver_equals_ignoring_case(home->file.name, home->repository.name);
        if (renamed) {
            settle(home, CANDIDATE_CONFIRMED, "rename-pending", "the host still names it %s, its repo.json %s",
                   home->repository.name, home->file.name);
        } else {
            settle(home, CANDIDATE_CONFIRMED, "confirmed", "it holds the id and every home it lists lists it back");
        }
    }
    return status;
}

naula_status naula_resolver_confirm(resolver *subject, size_t index, unsigned depth) {
    candidate *home = subject->candidates[index];
    if (home->outcome != CANDIDATE_PENDING || home->evaluating) {
        return NAULA_OK;
    }
    if (home->place.kind == NAULA_HOME_URL || (home->place.kind == NAULA_HOME_PROVIDER && home->place.number == 0)) {
        settle_unreadable_kind(home);
        return NAULA_OK;
    }
    naula_status status = read_candidate(subject, home);
    if (status != NAULA_OK || !home->readable || home->outcome != CANDIDATE_PENDING) {
        return status;
    }
    home->evaluating = 1;
    status = confirm_readable(subject, home, depth);
    home->evaluating = 0;
    return status;
}
