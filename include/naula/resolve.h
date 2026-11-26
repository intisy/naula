#ifndef NAULA_RESOLVE_H
#define NAULA_RESOLVE_H

/**
 * The resolver: from a reference `{ id, at }` to the homes that speak for that id. Homes confirm an id by listing each
 * other both ways; the root commit only narrows which homes are asked. Trust starts at the consumer's `at` and its
 * cache, never at a name, so a rename, a transfer, a fork or a copy at an old name cannot redirect a consumer that
 * holds `at`.
 */

#include <stddef.h>
#include <stdint.h>

#include "naula/home.h"
#include "naula/provider.h"
#include "naula/repo_file.h"
#include "naula/session.h"
#include "naula/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum naula_resolution_state {
    NAULA_CONFIRMED = 1,
    NAULA_STALE = 2,
    NAULA_DOMAIN_ONLY = 3
} naula_resolution_state;

/**
 * What the resolver found for one home. `code` is `confirmed` or `rename-pending` (a warning) when `confirmed` is 1,
 * else one of `copy-of`, `disowned-by`, `not-in-history`, `has-parents`, `not-listed`, `not-linked`,
 * `states-another-id`, `number-changed`, `number-disputed`, `unverifiable`, `timed-out`, `unreachable` or
 * `dead-skipped`. `number-disputed` marks a home that lists an account the records its `at` grew from hold different
 * numbers for, none of which the session's `accept_number` took; its fix names `accept_number`. A home known
 * only by the account a repo.json lists it under, whose number was never learned, has `number` 0 and names the
 * account in `detail`. First shape ends at `detail`.
 */
typedef struct naula_verdict {
    uint32_t size;
    naula_place home;
    int confirmed;
    char code[64];
    char detail[256];
} naula_verdict;

/**
 * A resolved id. `homes` are the confirmed homes, or for a STALE answer the cached ones, and `primary` indexes them:
 * new versions come only from it. `primary_repository` and `file` are the primary's, read from its working branch,
 * and stay empty when the primary is a web home or the answer is STALE. `cache_problem` is empty, or says why the
 * cache was ignored or could not be written; neither stops an answer. A refused answer keeps its verdicts and has no
 * homes; an answer of NAULA_OUT_OF_MEMORY leaves the resolution empty. The caller sets `size`, and the resolver every
 * nested one. First shape ends at `cache_problem`.
 */
typedef struct naula_resolution {
    uint32_t size;
    uint32_t state;
    size_t primary;
    naula_place *homes;
    size_t home_count;
    naula_repository primary_repository;
    naula_repo_file file;
    naula_verdict *verdicts;
    size_t verdict_count;
    char cache_problem[256];
} naula_resolution;

/**
 * Resolves `reference`. The anchor is the place of `at` that confirms and comes first in the confirmed homes' own
 * order, `at` order deciding only between places that do not list each other; the answer is every home linked both
 * ways to it, so another place of `at` that confirms only itself is refused (`not-linked`). The cache keeps one
 * record per `at`, so one consumer's first contact never shapes another's answer. A record never confirms a home on
 * its own: it guards the number each account was confirmed at, a number that answered gone included, which is asked
 * again every run; it is the starting point only when every place of `at` answered gone; a run that confirms nothing
 * never demotes a home in it; and it answers STALE when a home could not be heard, serving only homes confirmed when
 * it was last written and not refused in this run. An `at` with no record yet reads only its lineage, the records
 * that confirmed a place their `at` and this one both name, fixed before any home is weighed; it accepts a number for
 * an account only when every lineage record holding that account holds it, whatever else the run refuses. When they
 * hold more than one, nothing local can say which is genuine, so the session's `accept_number` is asked once per run
 * for each lineage number that differs from the one found; without a yes to every one the listing home is refused
 * `number-disputed`, and no first record is written that would forget the dispute.
 * A first record keeps the lineage's agreed numbers as guards. A `local:` id is refused (`unresolvable`), since it
 * names a repo with no home and is never a dependency's target, and so is a reference by `url` (`unverifiable`),
 * which only a reader of git can follow. With nothing confirmed the refusal
 * carries the verdict of the first home `at` names: NAULA_TIMED_OUT or NAULA_UNREACHABLE when that home could not be
 * heard, and NAULA_NOT_FOUND only when every place `at` names is gone. A primary other than the one the record
 * confirmed needs the session's `accept_primary` (`primary-changed`); with no record of its own, the lineage sets the
 * primary, and asks in any case when its records disagree or this run refused a home one of them holds, naming a
 * former primary as `was`. Primary pointers that loop are refused (`cycle`).
 */
naula_status naula_resolve(naula_session *session, const naula_reference *reference, naula_resolution *resolution,
                           naula_refusal *refusal);

void naula_resolution_release(naula_resolution *resolution);

#ifdef __cplusplus
}
#endif

#endif
