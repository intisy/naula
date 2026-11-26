#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "naula/id.h"
#include "naula/memory.h"
#include "resolve_internal.h"

#define PLACE_TEXT_SIZE 160
#define REFERENCE_FIX "name the dependency by its id and the places `at` holds"
#define GIT_READER_FIX "name the dependency by id and at, or resolve it with the naula command line"
#define NETWORK_FIX "check the network, then try again; the home is not counted as gone"
#define HOME_FIX "check that `at` names the repository's own home, not a copy of it"

static int covers_resolution(const naula_resolution *resolution) {
    return resolution->size >= offsetof(naula_resolution, cache_problem) + sizeof resolution->cache_problem;
}

static int covers_reference(const naula_reference *reference) {
    return reference->size >= offsetof(naula_reference, at_count) + sizeof reference->at_count;
}

static void start_resolution(naula_resolution *resolution) {
    uint32_t size = resolution->size;
    memset(resolution, 0, sizeof *resolution);
    resolution->size = size;
    resolution->primary_repository.size = sizeof resolution->primary_repository;
    resolution->file.size = sizeof resolution->file;
}

static void release_places(naula_place *places, size_t count) {
    for (size_t index = 0; places != NULL && index < count; index++) {
        naula_place_release(&places[index]);
    }
    naula_memory_release(places);
}

void naula_resolution_release(naula_resolution *resolution) {
    if (resolution == NULL) {
        return;
    }
    release_places(resolution->homes, resolution->home_count);
    naula_repository_release(&resolution->primary_repository);
    naula_repo_file_release(&resolution->file);
    for (size_t index = 0; resolution->verdicts != NULL && index < resolution->verdict_count; index++) {
        naula_place_release(&resolution->verdicts[index].home);
    }
    naula_memory_release(resolution->verdicts);
    start_resolution(resolution);
}

static int is_settled(const candidate *home) {
    return home->outcome != CANDIDATE_PENDING && !home->probe;
}

static int is_confirmed(const candidate *home) {
    return home->outcome == CANDIDATE_CONFIRMED && home->in_closure;
}

static naula_status collect_verdicts(const resolver *subject, naula_resolution *resolution) {
    size_t count = 0;
    for (size_t index = 0; index < subject->count; index++) {
        count += is_settled(subject->candidates[index]) ? 1u : 0u;
    }
    if (count == 0) {
        return NAULA_OK;
    }
    resolution->verdicts = naula_memory_allocate(count * sizeof *resolution->verdicts);
    if (resolution->verdicts == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    memset(resolution->verdicts, 0, count * sizeof *resolution->verdicts);
    for (size_t index = 0; index < subject->count; index++) {
        const candidate *home = subject->candidates[index];
        if (!is_settled(home)) {
            continue;
        }
        naula_verdict *verdict = &resolution->verdicts[resolution->verdict_count];
        verdict->size = sizeof *verdict;
        if (naula_resolver_copy_place(&home->place, &verdict->home) != NAULA_OK) {
            return NAULA_OUT_OF_MEMORY;
        }
        resolution->verdict_count++;
        verdict->confirmed = home->outcome == CANDIDATE_CONFIRMED;
        snprintf(verdict->code, sizeof verdict->code, "%s", home->code);
        snprintf(verdict->detail, sizeof verdict->detail, "%s", home->detail);
    }
    return NAULA_OK;
}

static naula_status read_id(const char *text, resolver *subject, naula_refusal *refusal) {
    naula_id id;
    memset(&id, 0, sizeof id);
    id.size = sizeof id;
    naula_refusal parsed;
    memset(&parsed, 0, sizeof parsed);
    parsed.size = sizeof parsed;
    naula_status status = naula_id_parse(text, strlen(text), &id, &parsed);
    if (status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    if (status != NAULA_OK) {
        naula_refuse(refusal, "invalid_reference", REFERENCE_FIX, "the id %s cannot be read: %s", text, parsed.detail);
        return NAULA_REFUSED;
    }
    if (id.kind != NAULA_ID_GIT) {
        naula_id_release(&id);
        naula_refuse(refusal, "unresolvable", "depend on a repo that has a home",
                     "%s names a repo with no home, which is never a dependency's target", text);
        return NAULA_REFUSED;
    }
    snprintf(subject->hex, sizeof subject->hex, "%s", id.commit);
    snprintf(subject->id_text, sizeof subject->id_text, "git:%s", id.commit);
    naula_id_release(&id);
    return NAULA_OK;
}

/**
 * Whether a cached home may be answered from the cache: only one confirmed when the record was written, and never one
 * this run heard refuse or answer that it is gone.
 */
static int is_servable(const resolver *subject, const cache_home *cached) {
    if (cached->state != CACHE_CONFIRMED) {
        return 0;
    }
    for (size_t index = 0; index < subject->count; index++) {
        const candidate *home = subject->candidates[index];
        if (naula_resolver_same_place(&home->place, home->account, &cached->place, cached->account)) {
            return home->outcome != CANDIDATE_REFUSED && home->outcome != CANDIDATE_DEAD;
        }
    }
    return 1;
}

/** The cached homes that may be served, as STALE; `answered` 0 when none may. */
static naula_status answer_stale(const resolver *subject, naula_resolution *resolution, int *answered) {
    const identity_cache *cache = &subject->cache;
    size_t servable = 0;
    *answered = 0;
    for (size_t index = 0; index < cache->count; index++) {
        servable += is_servable(subject, &cache->homes[index]) ? 1u : 0u;
    }
    if (servable == 0) {
        return NAULA_OK;
    }
    resolution->homes = naula_memory_allocate(servable * sizeof *resolution->homes);
    if (resolution->homes == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < cache->count; index++) {
        if (!is_servable(subject, &cache->homes[index])) {
            continue;
        }
        if (index == cache->primary) {
            resolution->primary = resolution->home_count;
        }
        if (naula_resolver_copy_place(&cache->homes[index].place, &resolution->homes[resolution->home_count]) !=
            NAULA_OK) {
            return NAULA_OUT_OF_MEMORY;
        }
        resolution->home_count++;
    }
    resolution->state = NAULA_STALE;
    *answered = 1;
    return collect_verdicts(subject, resolution);
}

/** Marks a home as one the answer depends on, so it is confirmed or refused in full rather than only read. */
static naula_status want(resolver *subject, size_t index) {
    subject->candidates[index]->wanted = 1;
    subject->candidates[index]->probe = 0;
    return naula_resolver_confirm(subject, index, 0);
}

static naula_status start_from_at(resolver *subject, const naula_reference *reference) {
    for (size_t place = 0; place < reference->at_count; place++) {
        size_t index = 0;
        naula_status status = naula_resolver_add(subject, &reference->at[place], NULL, &index);
        if (status != NAULA_OK) {
            return status;
        }
        subject->candidates[index]->wanted = 1;
    }
    subject->at_count = subject->count;
    for (size_t index = 0; index < subject->at_count; index++) {
        naula_status status = want(subject, index);
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

/**
 * Grows the confirmed set from the anchor through homes linked both ways: a home joins only when a member lists it
 * and it is confirmed, which it is only when it lists that member back. A home confirmed some other way, through
 * another place of `at` that no member lists, stays out.
 */
static naula_status close_from(resolver *subject, size_t anchor) {
    subject->candidates[anchor]->in_closure = 1;
    int grew = 1;
    while (grew) {
        grew = 0;
        for (size_t index = 0; index < subject->count; index++) {
            candidate *home = subject->candidates[index];
            if (!is_confirmed(home) || home->expanded) {
                continue;
            }
            home->expanded = 1;
            grew = 1;
            size_t count = 0;
            naula_candidate_homes(home, &count);
            for (size_t entry = 0; home->listed != NULL && entry < count; entry++) {
                size_t listed = home->listed[entry];
                if (listed == NAULA_NO_CANDIDATE) {
                    continue;
                }
                naula_status status = want(subject, listed);
                if (status != NAULA_OK) {
                    return status;
                }
                candidate *member = subject->candidates[listed];
                member->in_closure |= member->outcome == CANDIDATE_CONFIRMED;
            }
        }
    }
    return NAULA_OK;
}

static void refuse_unlinked(resolver *subject) {
    for (size_t index = 0; index < subject->count; index++) {
        candidate *home = subject->candidates[index];
        if (home->outcome == CANDIDATE_CONFIRMED && !home->in_closure && !home->probe) {
            home->outcome = CANDIDATE_REFUSED;
            snprintf(home->code, sizeof home->code, "%s", "not-linked");
            snprintf(home->detail, sizeof home->detail, "%s",
                     "it confirms itself, but no home linked both ways to the first home `at` reaches lists it");
        }
    }
}

static size_t confirmed_candidate_for(const resolver *subject, const naula_home *home) {
    for (size_t index = 0; index < subject->count; index++) {
        if (is_confirmed(subject->candidates[index]) && naula_home_names(home, subject->candidates[index])) {
            return index;
        }
    }
    return NAULA_NO_CANDIDATE;
}

static size_t earliest_confirmed_listed(const resolver *subject, size_t current) {
    size_t count = 0;
    const naula_home *homes = naula_candidate_homes(subject->candidates[current], &count);
    for (size_t entry = 0; entry < count; entry++) {
        size_t index = confirmed_candidate_for(subject, &homes[entry]);
        if (index != NAULA_NO_CANDIDATE) {
            return index;
        }
    }
    return current;
}

/**
 * Follows primary pointers from the anchor until a home names itself first. A pointer to a home that is not confirmed
 * stops at the earliest confirmed home the current one lists, which is the primary when it is reachable.
 */
static naula_status follow_primary(const resolver *subject, size_t anchor, size_t *primary, naula_refusal *refusal) {
    unsigned char *visited = naula_memory_allocate(subject->count);
    if (visited == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    memset(visited, 0, subject->count);
    size_t current = anchor;
    visited[current] = 1;
    naula_status status = NAULA_OK;
    for (;;) {
        size_t count = 0;
        const naula_home *homes = naula_candidate_homes(subject->candidates[current], &count);
        size_t target = count == 0 ? NAULA_NO_CANDIDATE : confirmed_candidate_for(subject, &homes[0]);
        if (target == current) {
            break;
        }
        if (target == NAULA_NO_CANDIDATE) {
            current = earliest_confirmed_listed(subject, current);
            break;
        }
        if (visited[target]) {
            char text[PLACE_TEXT_SIZE];
            naula_candidate_text(subject->candidates[target], text, sizeof text);
            naula_refuse(refusal, "cycle", "make the first entry of homes the same home in every repo.json",
                         "the homes of %s name each other first in a loop through %s", subject->id_text, text);
            status = NAULA_REFUSED;
            break;
        }
        visited[target] = 1;
        current = target;
    }
    naula_memory_release(visited);
    *primary = current;
    return status;
}

/**
 * The baseline a new primary is asked against: this anchor's record's primary, or with no record of its own the
 * primary of its lineage, any disagreement among them asking in any case, so a consumer that edits its `at` during a
 * move keeps the prompt.
 */
static naula_status baseline_primary(const resolver *subject, const naula_place **was, int *conflicting) {
    *was = NULL;
    *conflicting = 0;
    if (subject->cache.present) {
        *was = subject->cache.has_primary ? &subject->cache.homes[subject->cache.primary].place : NULL;
        return NAULA_OK;
    }
    for (size_t index = 0; index < subject->lineage_count; index++) {
        const identity_cache *record = &subject->lineage[index];
        if (!record->has_primary || !naula_resolver_lineage_usable(subject, index)) {
            continue;
        }
        const naula_place *primary = &record->homes[record->primary].place;
        if (*was == NULL) {
            *was = primary;
        } else {
            *conflicting |= !naula_resolver_same_place(*was, NULL, primary, NULL);
        }
    }
    return NAULA_OK;
}

static naula_status check_primary_change(const resolver *subject, size_t primary, naula_refusal *refusal) {
    const naula_place *was = NULL;
    int conflicting = 0;
    baseline_primary(subject, &was, &conflicting);
    const naula_place *becomes = &subject->candidates[primary]->place;
    if (was == NULL || (!conflicting && naula_resolver_same_place(was, NULL, becomes, NULL))) {
        return NAULA_OK;
    }
    const naula_session *session = subject->session;
    if (session->accept_primary != NULL && session->accept_primary(session->context, subject->id_text, was, becomes)) {
        return NAULA_OK;
    }
    char text[PLACE_TEXT_SIZE];
    naula_candidate_text(subject->candidates[primary], text, sizeof text);
    naula_refuse(refusal, "primary-changed", "accept the new primary, or find out why the repo's first home changed",
                 "the primary of %s is now %s, not the home confirmed before", subject->id_text, text);
    return NAULA_REFUSED;
}

static naula_status cache_entry(const candidate *home, int64_t now, cache_home *entry) {
    memset(entry, 0, sizeof *entry);
    entry->state = CACHE_CONFIRMED;
    entry->confirmed = now;
    if (naula_resolver_copy_place(&home->place, &entry->place) != NAULA_OK) {
        return NAULA_OUT_OF_MEMORY;
    }
    if (home->place.kind == NAULA_HOME_PROVIDER) {
        entry->account = naula_memory_duplicate_text(home->repository.account);
        if (entry->account == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
    }
    if (home->repository.etag != NULL) {
        entry->etag = naula_memory_duplicate_text(home->repository.etag);
        if (entry->etag == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
    }
    return NAULA_OK;
}

static naula_status copy_cache_entry(const cache_home *from, cache_state state, cache_home *entry) {
    memset(entry, 0, sizeof *entry);
    entry->state = state;
    entry->confirmed = from->confirmed;
    if (naula_resolver_copy_place(&from->place, &entry->place) != NAULA_OK) {
        return NAULA_OUT_OF_MEMORY;
    }
    if (from->account != NULL && (entry->account = naula_memory_duplicate_text(from->account)) == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    if (from->etag != NULL && (entry->etag = naula_memory_duplicate_text(from->etag)) == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    return NAULA_OK;
}

/**
 * Whether a cached home is in this run's confirmed set, which is written from the run; any other is kept as a GUARD,
 * so its number still guards its account but is never served, and is asked again on the next run.
 */
static int is_reconfirmed(const resolver *subject, const cache_home *cached) {
    for (size_t index = 0; index < subject->count; index++) {
        const candidate *home = subject->candidates[index];
        if (naula_resolver_same_place(&home->place, home->account, &cached->place, cached->account)) {
            return is_confirmed(home);
        }
    }
    return 0;
}

static naula_status build_cache(const resolver *subject, size_t primary, identity_cache *next) {
    int64_t now = (int64_t)time(NULL);
    size_t capacity = subject->count + subject->cache.count;
    memset(next, 0, sizeof *next);
    next->homes = naula_memory_allocate(capacity * sizeof *next->homes);
    next->at = naula_memory_allocate(subject->at_text_count * sizeof *next->at);
    if (next->homes == NULL || next->at == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < subject->at_text_count; index++) {
        next->at[index] = naula_memory_duplicate_text(subject->at_texts[index]);
        if (next->at[index] == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        next->at_count++;
    }
    for (size_t index = 0; index < subject->count; index++) {
        if (!is_confirmed(subject->candidates[index])) {
            continue;
        }
        if (index == primary) {
            next->primary = next->count;
            next->has_primary = 1;
        }
        naula_status status = cache_entry(subject->candidates[index], now, &next->homes[next->count++]);
        if (status != NAULA_OK) {
            return status;
        }
    }
    for (size_t entry = 0; entry < subject->cache.count; entry++) {
        if (is_reconfirmed(subject, &subject->cache.homes[entry])) {
            continue;
        }
        naula_status status = copy_cache_entry(&subject->cache.homes[entry], CACHE_GUARD, &next->homes[next->count++]);
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

static naula_status write_cache(const resolver *subject, size_t primary, naula_resolution *resolution) {
    if (subject->session->cache_directory == NULL) {
        return NAULA_OK;
    }
    identity_cache next;
    naula_status status = build_cache(subject, primary, &next);
    if (status == NAULA_OK && !subject->cache.foreign) {
        status = naula_cache_write(subject->session->cache_directory, subject->hex, subject->anchor, &next,
                                   resolution->cache_problem);
    }
    naula_cache_release(&next);
    return status;
}

static naula_status fill_answer(resolver *subject, size_t primary, naula_resolution *resolution) {
    size_t count = 0;
    int by_provider = 0;
    for (size_t index = 0; index < subject->count; index++) {
        count += is_confirmed(subject->candidates[index]) ? 1u : 0u;
    }
    naula_status status = collect_verdicts(subject, resolution);
    resolution->homes = status == NAULA_OK ? naula_memory_allocate(count * sizeof *resolution->homes) : NULL;
    if (resolution->homes == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < subject->count; index++) {
        const candidate *home = subject->candidates[index];
        if (!is_confirmed(home)) {
            continue;
        }
        if (index == primary) {
            resolution->primary = resolution->home_count;
        }
        if (naula_resolver_copy_place(&home->place, &resolution->homes[resolution->home_count]) != NAULA_OK) {
            return NAULA_OUT_OF_MEMORY;
        }
        resolution->home_count++;
        by_provider |= home->place.kind == NAULA_HOME_PROVIDER;
    }
    candidate *chosen = subject->candidates[primary];
    if (chosen->place.kind == NAULA_HOME_PROVIDER) {
        resolution->primary_repository = chosen->repository;
        memset(&chosen->repository, 0, sizeof chosen->repository);
        chosen->repository.size = sizeof chosen->repository;
        resolution->file = chosen->file;
        memset(&chosen->file, 0, sizeof chosen->file);
        chosen->file.size = sizeof chosen->file;
    }
    resolution->state = by_provider ? NAULA_CONFIRMED : NAULA_DOMAIN_ONLY;
    return NAULA_OK;
}

static int is_unheard(const candidate *home) {
    return home->outcome == CANDIDATE_UNKNOWN &&
           (strcmp(home->code, "timed-out") == 0 || strcmp(home->code, "unreachable") == 0);
}

static naula_status status_for(const resolver *subject, const candidate *decisive) {
    int every_at_gone = subject->at_count > 0;
    for (size_t index = 0; index < subject->at_count; index++) {
        every_at_gone &= subject->candidates[index]->outcome == CANDIDATE_DEAD;
    }
    if (every_at_gone) {
        return NAULA_NOT_FOUND;
    }
    if (strcmp(decisive->code, "timed-out") == 0) {
        return NAULA_TIMED_OUT;
    }
    return strcmp(decisive->code, "unreachable") == 0 ? NAULA_UNREACHABLE : NAULA_REFUSED;
}

/**
 * Nothing confirmed. A home that could not be heard is not refused: with a confirmation cached, the cached homes that
 * may be served are answered as STALE. Otherwise the first home `at` names decides the refusal.
 */
static naula_status answer_unconfirmed(const resolver *subject, naula_resolution *resolution, naula_refusal *refusal) {
    const candidate *decisive = NULL;
    int unheard = 0;
    for (size_t index = 0; index < subject->count; index++) {
        const candidate *home = subject->candidates[index];
        if (!home->wanted || !is_settled(home)) {
            continue;
        }
        unheard |= is_unheard(home);
        if (decisive == NULL) {
            decisive = home;
        }
    }
    if (unheard && subject->cache.present) {
        int answered = 0;
        naula_status stale = answer_stale(subject, resolution, &answered);
        if (stale != NAULA_OK || answered) {
            return stale;
        }
    }
    naula_status status = collect_verdicts(subject, resolution);
    if (status != NAULA_OK) {
        return status;
    }
    if (decisive == NULL) {
        naula_refuse(refusal, "unresolvable", REFERENCE_FIX, "nothing confirms %s", subject->id_text);
        return NAULA_REFUSED;
    }
    char text[PLACE_TEXT_SIZE];
    naula_candidate_text(decisive, text, sizeof text);
    status = status_for(subject, decisive);
    naula_refuse(refusal, decisive->code, is_unheard(decisive) ? NETWORK_FIX : HOME_FIX, "%s: %s", text,
                 decisive->detail);
    return status;
}

/**
 * The confirmed `at` place that comes first in the first confirmed one's own `homes`, the primary first; `at` order
 * decides only between places that do not list each other.
 */
static size_t choose_anchor(const resolver *subject) {
    size_t first = NAULA_NO_CANDIDATE;
    for (size_t index = 0; index < subject->at_count && first == NAULA_NO_CANDIDATE; index++) {
        first = subject->candidates[index]->outcome == CANDIDATE_CONFIRMED ? index : NAULA_NO_CANDIDATE;
    }
    if (first == NAULA_NO_CANDIDATE) {
        return first;
    }
    size_t count = 0;
    const naula_home *homes = naula_candidate_homes(subject->candidates[first], &count);
    for (size_t entry = 0; entry < count; entry++) {
        for (size_t index = 0; index < subject->at_count; index++) {
            const candidate *home = subject->candidates[index];
            if (home->outcome == CANDIDATE_CONFIRMED && naula_home_names(&homes[entry], home)) {
                return index;
            }
        }
    }
    return first;
}

static naula_status try_record_home(resolver *subject, size_t entry, size_t *anchor) {
    const cache_home *cached = &subject->cache.homes[entry];
    size_t index = 0;
    if (cached->state != CACHE_CONFIRMED || *anchor != NAULA_NO_CANDIDATE) {
        return NAULA_OK;
    }
    naula_status status = naula_resolver_add(subject, &cached->place, NULL, &index);
    if (status == NAULA_REFUSED) {
        return NAULA_OK;
    }
    if (status == NAULA_OK) {
        status = want(subject, index);
    }
    if (status == NAULA_OK && subject->candidates[index]->outcome == CANDIDATE_CONFIRMED) {
        *anchor = index;
    }
    return status;
}

/**
 * When every place of `at` answered gone, this anchor's record's confirmed homes, its primary first, are where to
 * start: a consumer that saw a planned move keeps the new home after the old one is deleted.
 */
static naula_status start_from_record(resolver *subject, size_t *anchor) {
    *anchor = NAULA_NO_CANDIDATE;
    if (!subject->cache.present || subject->at_count == 0) {
        return NAULA_OK;
    }
    for (size_t index = 0; index < subject->at_count; index++) {
        if (subject->candidates[index]->outcome != CANDIDATE_DEAD) {
            return NAULA_OK;
        }
    }
    naula_status status = NAULA_OK;
    if (subject->cache.has_primary) {
        status = try_record_home(subject, subject->cache.primary, anchor);
    }
    for (size_t entry = 0; entry < subject->cache.count && status == NAULA_OK; entry++) {
        status = try_record_home(subject, entry, anchor);
    }
    return status;
}

static naula_status answer(resolver *subject, naula_resolution *resolution, naula_refusal *refusal) {
    size_t anchor = choose_anchor(subject);
    naula_status status = NAULA_OK;
    if (anchor == NAULA_NO_CANDIDATE) {
        status = start_from_record(subject, &anchor);
    }
    if (status != NAULA_OK) {
        return status;
    }
    if (anchor == NAULA_NO_CANDIDATE) {
        return answer_unconfirmed(subject, resolution, refusal);
    }
    status = close_from(subject, anchor);
    if (status != NAULA_OK) {
        return status;
    }
    refuse_unlinked(subject);
    size_t primary = anchor;
    status = follow_primary(subject, anchor, &primary, refusal);
    if (status == NAULA_OK) {
        status = check_primary_change(subject, primary, refusal);
    }
    if (status == NAULA_REFUSED) {
        naula_status collected = collect_verdicts(subject, resolution);
        return collected == NAULA_OK ? status : collected;
    }
    if (status == NAULA_OK) {
        status = write_cache(subject, primary, resolution);
    }
    return status == NAULA_OK ? fill_answer(subject, primary, resolution) : status;
}

static naula_status resolve_online(resolver *subject, const naula_reference *reference, naula_resolution *resolution,
                                   naula_refusal *refusal) {
    naula_status status = start_from_at(subject, reference);
    if (status == NAULA_REFUSED) {
        naula_refuse(refusal, "unverifiable", REFERENCE_FIX, "%s names more places than are weighed", subject->id_text);
        return status;
    }
    return status == NAULA_OK ? answer(subject, resolution, refusal) : status;
}

static int holds_text(char *const *texts, size_t count, const char *text) {
    for (size_t index = 0; index < count; index++) {
        if (strcmp(texts[index], text) == 0) {
            return 1;
        }
    }
    return 0;
}

/** Whether `other` confirmed a place its own `at` holds and this `at` also names. */
static naula_status is_lineage(const resolver *subject, const identity_cache *other, int *lineage) {
    *lineage = 0;
    for (size_t entry = 0; entry < other->count && !*lineage; entry++) {
        if (other->homes[entry].state != CACHE_CONFIRMED) {
            continue;
        }
        char *text = naula_cache_place_text(&other->homes[entry].place);
        if (text == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        *lineage = holds_text(other->at, other->at_count, text) &&
                   holds_text(subject->at_texts, subject->at_text_count, text);
        naula_memory_release(text);
    }
    return NAULA_OK;
}

static naula_status keep_lineage(resolver *subject, identity_cache *other) {
    if (subject->lineage_count == subject->lineage_capacity) {
        size_t capacity = subject->lineage_capacity == 0 ? 4 : subject->lineage_capacity * 2;
        identity_cache *grown = naula_memory_reallocate(subject->lineage, capacity * sizeof *grown);
        if (grown == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        subject->lineage = grown;
        subject->lineage_capacity = capacity;
    }
    subject->lineage[subject->lineage_count++] = *other;
    memset(other, 0, sizeof *other);
    return NAULA_OK;
}

/**
 * With no record of this anchor's own, its lineage: the id's records whose stored `at` holds a place that record
 * confirmed and that this `at` also names. Nothing else any other consumer wrote is read.
 */
static naula_status read_lineage(resolver *subject) {
    const char *directory = subject->session->cache_directory;
    char **anchors = NULL;
    size_t count = 0;
    if (directory == NULL || subject->cache.present) {
        return NAULA_OK;
    }
    naula_status status = naula_cache_other_anchors(directory, subject->hex, subject->anchor, &anchors, &count);
    for (size_t index = 0; status == NAULA_OK && index < count; index++) {
        identity_cache other;
        char ignored[NAULA_CACHE_PROBLEM_SIZE] = "";
        int lineage = 0;
        status = naula_cache_read(directory, subject->hex, anchors[index], NULL, 0, &other, ignored);
        if (status == NAULA_OK && other.present) {
            status = is_lineage(subject, &other, &lineage);
        }
        if (status == NAULA_OK && lineage) {
            status = keep_lineage(subject, &other);
        }
        naula_cache_release(&other);
    }
    naula_cache_release_texts(anchors, count);
    return status;
}

static naula_status read_cache(resolver *subject, const naula_reference *reference, naula_resolution *resolution) {
    const char *directory = subject->session->cache_directory;
    naula_status status =
        naula_cache_normal_at(reference->at, reference->at_count, &subject->at_texts, &subject->at_text_count);
    if (status == NAULA_OK) {
        status = naula_cache_anchor(reference->at, reference->at_count, subject->anchor);
    }
    if (status == NAULA_OK) {
        status = naula_cache_read(directory, subject->hex, subject->anchor, subject->at_texts, subject->at_text_count,
                                  &subject->cache, resolution->cache_problem);
    }
    return status == NAULA_OK ? read_lineage(subject) : status;
}

static naula_status resolve_with(resolver *subject, const naula_reference *reference, naula_resolution *resolution,
                                 naula_refusal *refusal) {
    naula_status status = read_id(reference->id, subject, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    if (reference->at_count == 0) {
        naula_refuse(refusal, "invalid_reference", REFERENCE_FIX, "%s names no place to start from",
                     subject->id_text);
        return NAULA_REFUSED;
    }
    status = read_cache(subject, reference, resolution);
    if (status != NAULA_OK) {
        return status;
    }
    if (!subject->session->offline) {
        return resolve_online(subject, reference, resolution, refusal);
    }
    int answered = 0;
    status = answer_stale(subject, resolution, &answered);
    if (status != NAULA_OK || answered) {
        return status;
    }
    naula_refuse(refusal, "unresolvable", "resolve it once while online",
                 "offline, and no confirmation of %s is cached", subject->id_text);
    return NAULA_UNREACHABLE;
}

naula_status naula_resolve(naula_session *session, const naula_reference *reference, naula_resolution *resolution,
                           naula_refusal *refusal) {
    if (session == NULL || reference == NULL || resolution == NULL || !covers_resolution(resolution) ||
        !covers_reference(reference)) {
        naula_refuse(refusal, "invalid_reference", REFERENCE_FIX,
                     "a struct is missing or smaller than its first shape");
        return NAULA_REFUSED;
    }
    start_resolution(resolution);
    if (reference->url != NULL || reference->id == NULL) {
        naula_refuse(refusal, reference->url != NULL ? "unverifiable" : "invalid_reference", GIT_READER_FIX,
                     reference->url != NULL ? "a reference by url (%s) is followed only by a reader of git"
                                            : "the reference names no id%s",
                     reference->url != NULL ? reference->url : "");
        return NAULA_REFUSED;
    }
    resolver subject;
    memset(&subject, 0, sizeof subject);
    subject.session = session;
    naula_status status = resolve_with(&subject, reference, resolution, refusal);
    naula_resolver_release_candidates(&subject);
    naula_cache_release(&subject.cache);
    for (size_t index = 0; index < subject.lineage_count; index++) {
        naula_cache_release(&subject.lineage[index]);
    }
    naula_memory_release(subject.lineage);
    naula_cache_release_texts(subject.at_texts, subject.at_text_count);
    if (status == NAULA_OUT_OF_MEMORY) {
        naula_resolution_release(resolution);
    }
    return status;
}
