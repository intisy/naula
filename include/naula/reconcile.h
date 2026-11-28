#ifndef NAULA_RECONCILE_H
#define NAULA_RECONCILE_H

#include <stddef.h>
#include <stdint.h>

#include "naula/home.h"
#include "naula/session.h"
#include "naula/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * How naula_reconcile runs. `cache_directory` keeps, in `homes/<hex>-<anchor>.json`, the number each home was last
 * found at, `<anchor>` hashing the homes the checkout's own repo.json history listed, so another checkout of the id
 * (a fork naming its own homes) never shapes it: a hint only, used once the host shows that repo holds the id, and
 * as the serving home only for an account that history listed. `url_is_allowed` answers whether a clone url may be handed to git (NULL allows
 * https only). `held_accounts` are the accounts the orgs of the root hold, and `accepted_homes` the homes a person
 * accepted for this run, each written `<provider>[@<host>]:<account>`: a home is created, or transferred into, only in
 * one of the two. `say` receives one line for every change made and every home left alone with a reason, `context`
 * beside it. First shape ends at `accepted_home_count`.
 */
typedef struct naula_reconcile_options {
    uint32_t size;
    const char *cache_directory;
    int (*url_is_allowed)(const char *url);
    void (*say)(void *context, const char *line);
    void *context;
    const naula_home *held_accounts;
    size_t held_account_count;
    const char *const *accepted_homes;
    size_t accepted_home_count;
} naula_reconcile_options;

/**
 * What a run did: `changed` counts the writes the hosts took, `failed` the writes a host refused, and `first_failure`
 * says why the first of them failed. The caller sets `size`. First shape ends at `first_failure`.
 */
typedef struct naula_reconcile_report {
    uint32_t size;
    size_t changed;
    size_t failed;
    naula_refusal first_failure;
} naula_reconcile_report;

/**
 * Brings every home of the repo checked out at `repository` to the repo.json at the tip of the working branch of the
 * home that serves it, never to the local file: the serving home is the first home of the local file found on its
 * host holding the repo's id (the checkout's first-parent root, or the fork id it states). In order, each step
 * comparing before it changes anything: rename the primary, then every other home; transfer a home whose account
 * changed; create a home that does not exist and push every branch and tag to it; push to every other home; write
 * the metadata the file states. The caller relinks.
 *
 * A home is this repo's only when its host shows it holds the id, the way the resolver confirms one, and, unless it
 * serves the file, its own repo.json lists the serving home; so a copy holding the id can neither take writes nor
 * steer them. Refused before anything is sent: `home-unheard` (a home could not be heard, so nothing is decided from
 * it), `no-home-found`, `serving-unproven`, `home-unaccepted` (a home to create or transfer into that no org of the
 * root holds and no run accepted), `id-changed`, `primary-not-listed`, `disowned-by`, `home-unadopted`, `name-taken`
 * and `name-unverifiable` (another repo holds, or may hold, the name under a home's account) and `literal-consumers` (a
 * rename or transfer of a repo holding a `workflow_call` workflow, or a Gradle build with a release, which consumers
 * reach by its name). A write a host refuses is reported and the run goes on; it answers NAULA_REFUSED
 * `reconcile-incomplete` then, and a later run converges from what is there.
 */
naula_status naula_reconcile(naula_session *session, const char *repository, const naula_reconcile_options *options,
                             naula_reconcile_report *report, naula_refusal *refusal);

#ifdef __cplusplus
}
#endif

#endif
