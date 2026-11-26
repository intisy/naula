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
 * `states-another-id`, `number-changed`, `unverifiable`, `timed-out`, `unreachable` or `dead-skipped`. A home known
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
 * Resolves `reference`. The anchor is the first place of `at` that confirms; the answer is every home linked both
 * ways to it, so another place of `at` that confirms only itself is refused (`not-linked`). The cache never confirms
 * a home: it guards the number each account was confirmed at, keeps a number that answered gone as gone for good, and
 * answers STALE when a home could not be heard, serving only homes confirmed when it was written and not refused in
 * this run. A `local:` id is refused (`unresolvable`), since it names a repo with no home and is never a dependency's
 * target, and so is a reference by `url` (`unverifiable`), which only a reader of git can follow. With nothing
 * confirmed the refusal carries the verdict of the first home `at` names: NAULA_TIMED_OUT or NAULA_UNREACHABLE when
 * that home could not be heard, and NAULA_NOT_FOUND only when every place `at` names is gone. A primary other than the
 * cached one needs the session's `accept_primary` (`primary-changed`) when the cache speaks for the same confirmed
 * homes, and primary pointers that loop are refused (`cycle`).
 */
naula_status naula_resolve(naula_session *session, const naula_reference *reference, naula_resolution *resolution,
                           naula_refusal *refusal);

void naula_resolution_release(naula_resolution *resolution);

#ifdef __cplusplus
}
#endif

#endif
