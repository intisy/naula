#ifndef NAULA_GIT_H
#define NAULA_GIT_H

#include <stddef.h>

#include "naula/id.h"
#include "naula/run.h"
#include "naula/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The variables every git child gets so it can never wait for a person: no terminal, credential manager or askpass
 * prompt, and ssh in batch mode. Every variable git documents as choosing another repository, object store,
 * configuration, program or transport than the one named (GIT_DIR, GIT_WORK_TREE, GIT_CONFIG_*, GIT_EXEC_PATH,
 * GIT_SSH, GIT_SSL_NO_VERIFY and the like) is removed, so a hook or CI step that set one never redirects naula.
 * NULL-terminated, for naula_run.
 */
typedef struct naula_git_environment_entries {
    const char *list[48];
} naula_git_environment_entries;

void naula_git_environment(naula_git_environment_entries *entries);

/**
 * Runs git with the NULL-terminated `arguments` after the program name, in `repository` (NULL keeps the caller's),
 * with naula_git_environment and `input` on its standard input (NULL for none). A git that cannot start is the
 * `git-missing` refusal; a git that ran and failed is NAULA_OK with its exit code, for the caller to read.
 */
naula_status naula_git_run(const char *repository, const char *const *arguments, const char *input,
                           size_t input_length, naula_run_result *result, naula_refusal *refusal);

/** naula_git_run under naula_run_limited's `limits`. */
naula_status naula_git_run_limited(const char *repository, const char *const *arguments, const char *input,
                                   size_t input_length, const naula_run_limits *limits, naula_run_result *result,
                                   naula_refusal *refusal);

/**
 * Refuses with `git-missing` when no `git` runs, and with `git-too-old` below 2.24, the first release that takes
 * `--end-of-options` (2.20 already had `fetch --filter=tree:0` leaving out every tree). The other calls here need
 * that git and do not check again.
 */
naula_status naula_git_check_version(naula_refusal *refusal);

/** The full object id of the root commit reached from `ref` by following first parents only, 40 or 64 lowercase hex. */
naula_status naula_git_first_parent_root(const char *repository, const char *ref, char root[65],
                                         naula_refusal *refusal);

naula_status naula_git_is_ancestor(const char *repository, const char *commit, const char *ref, int *ancestor,
                                   naula_refusal *refusal);

naula_status naula_git_parent_count(const char *repository, const char *commit, size_t *count,
                                    naula_refusal *refusal);

/**
 * The id of the repository at `ref`: the first-parent root, or, when `stated_id` is not NULL, that id once it is
 * checked to name a commit on the first-parent line of `ref` that is not the root (the fork exception). Refusals:
 * `fork-id-not-on-line`, `fork-id-is-root`. Release `id` with naula_id_release.
 */
naula_status naula_git_repository_id(const char *repository, const char *ref, const char *stated_id, naula_id *id,
                                     naula_refusal *refusal);

/**
 * Refuses with `id-changed` when the id at `new_tip` is not the id at `previous_tip`, each being its tip's stated
 * fork id (NULL when none) or else its first-parent root. A tip with no stated id may take one: that is the fork
 * exception, allowed once it passes naula_git_repository_id at `new_tip`. Dropping or changing a stated id is refused.
 */
naula_status naula_git_check_id_kept(const char *repository, const char *previous_tip, const char *previous_stated_id,
                                     const char *new_tip, const char *new_stated_id, naula_refusal *refusal);

/** Fetches the commits of `ref` from `url` into `repository` without any tree, leaving them at FETCH_HEAD. */
naula_status naula_git_fetch_commits(const char *repository, const char *url, const char *ref, naula_refusal *refusal);

#ifdef __cplusplus
}
#endif

#endif
