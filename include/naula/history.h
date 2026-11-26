#ifndef NAULA_HISTORY_H
#define NAULA_HISTORY_H

#include <stddef.h>
#include <stdint.h>

#include "naula/home.h"
#include "naula/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * How the resolver reads a home's history. The installer reads it through each provider's API; the command line and
 * the helper pass one backed by git. `ref` is the working branch the home's repo.json was read on, or NULL for a web
 * home, whose working branch the reader finds itself. A commit an existing branch does not hold answers `contained`
 * 0; a ref the home does not have answers REFUSED `missing_commit_or_ref`, which is how the resolver learns there is
 * no development branch, and a commit the home does not hold at all may answer either. `parent_count` of a commit
 * the home does not hold answers REFUSED `missing_commit`. NAULA_NOT_FOUND is answered only
 * for a home that is gone; a reader that cannot read a kind of home refuses it, which the resolver reports as
 * `unverifiable`. First shape ends at `parent_count`.
 */
typedef struct naula_history {
    uint32_t size;
    void *context;
    naula_status (*contains)(void *context, const naula_place *home, const char *ref, const char *commit,
                             int *contained, naula_refusal *refusal);
    naula_status (*parent_count)(void *context, const naula_place *home, const char *commit, size_t *count,
                                 naula_refusal *refusal);
} naula_history;

#ifdef __cplusplus
}
#endif

#endif
