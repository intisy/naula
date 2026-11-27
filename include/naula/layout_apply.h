#ifndef NAULA_LAYOUT_APPLY_H
#define NAULA_LAYOUT_APPLY_H

/**
 * Carrying out a layout plan on the disk, journalled so a run cut short resumes. Every struct is read only up to its
 * `size`; the caller sets it.
 *
 * Everything naula keeps about a root lives in the root itself and nowhere else: the journals and the record of the
 * links naula made in `<root>/.naula-journal/`, and the orgs it holds in `<root>/.naula.json`. Each is refused, and
 * so is the root, when it is a link or other reparse point, so nothing inside a checkout, a repo or a remote can
 * speak for them.
 *
 * Before any change, every step is checked against the disk: no component of its `path` or `target` below the root
 * may be a link or other reparse point, except the link a MAKE_LINK replaces or a REMOVE_LINK removes. A step that
 * finds the disk already as it asks changes nothing, so a plan applied twice changes nothing the second time.
 */

#include <stddef.h>
#include <stdint.h>

#include "naula/layout.h"
#include "naula/repo_file.h"
#include "naula/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * `root` is the root the plan was made for; every step's paths must lie below it. `memory_banks` is the folder of
 * Claude Code's memory banks, NULL to leave banks alone. With `dry_run` nothing is read or written: each step is
 * only checked for shape and passed to `report`. `report`, when set, is told each step once it is applied, with
 * NAULA_OK, or with the status of the refusal that stopped the run there. First shape ends at `context`.
 */
typedef struct naula_apply_options {
    uint32_t size;
    const char *root;
    const char *memory_banks;
    int dry_run;
    void (*report)(void *context, const naula_layout_step *step, naula_status status);
    void *context;
} naula_apply_options;

/**
 * Applies `plan` in its order, journalled in `<root>/.naula-journal/<UTC yyyymmddThhmmssZ>.jsonl`: the whole plan
 * first, then a line before each step (`intended`) and after it (`done`), each flushed to the disk before the step
 * goes on. Stops at the first step it refuses, leaving the journal to resume from there.
 *
 * - MAKE_FOLDER makes the one folder; its parent must exist.
 * - WRITE_FILE replaces the file through a temporary beside it; a file with other bytes is first copied, hex encoded,
 *   into the `intended` line as `previous`, so its old content survives.
 * - MOVE_CHECKOUT is one rename that never replaces and never copies: a process holding the folder stops the run
 *   with `folder-held`. The memory bank `<memory_banks>/<key of path>` is then renamed to the key of `target`; a
 *   bank already at the new key refuses the move before it is made, as `memory-bank-exists`.
 * - MAKE_LINK makes a junction on Windows and a symlink elsewhere, to a target that is a real folder. It replaces only
 *   a link naula's record says it made, with the target recorded; the link is recorded once it is made.
 * - REMOVE_LINK removes only a link whose target is exactly the recorded one and which the record holds, by one
 *   non-recursive call, never falling back to removing a folder; the record forgets it once it is gone.
 * - REMOVE_CLONE refuses a link at `path` and removes the real folder, never entering a link or other reparse point
 *   inside it: each is removed itself.
 * - REPORT changes nothing.
 */
naula_status naula_layout_apply(const naula_layout_plan *plan, const naula_apply_options *options, naula_refusal *refusal);

/**
 * Carries on the run `journal_file` records from where it stopped, checking every remaining step against the disk
 * as naula_layout_apply does, and appending to the same journal. The journal must sit directly in
 * `<options->root>/.naula-journal/` and name that root. A journal that finished changes nothing.
 */
naula_status naula_journal_resume(const char *journal_file, const naula_apply_options *options, naula_refusal *refusal);

/** The newest journal in `<root>/.naula-journal/` that did not finish, NAULA_NOT_FOUND when every one did. */
naula_status naula_journal_unfinished(const char *root, char **journal_file, naula_refusal *refusal);

/**
 * Claude Code's memory bank key for a folder: `real_path` with every `:`, `/` and `\` replaced by `-`, as
 * `F:\Documents\GitHub\spisor` is `F--Documents-GitHub-spisor`. A key that does not fit `capacity` leaves `key`
 * empty, never cut short, since a shortened key names another folder's bank.
 */
void naula_memory_bank_key(const char *real_path, char *key, size_t capacity);

/** First shape ends at `count`. */
typedef struct naula_layout_links {
    uint32_t size;
    naula_layout_link *links;
    size_t count;
} naula_layout_links;

/**
 * Every link and other reparse point under `root`, for naula_layout_input's `existing_links`, without entering a
 * link, a checkout (a folder holding `.git`) or `.naula-journal`. `made_by_naula` is set only where naula's record
 * holds the path and the link on disk still points at exactly the recorded target, so a link someone replaced or
 * repointed reads as unrecorded. Another reparse point reads as an unrecorded link with an empty target.
 */
naula_status naula_layout_links_scan(const char *root, naula_layout_links *links, naula_refusal *refusal);
void naula_layout_links_release(naula_layout_links *links);

/**
 * `<root>/.naula.json`: the top org, the orgs added and dropped against its `org.hold`, and `extra`, the single repos
 * kept from accounts no held org owns, `extra_orgs[i]` naming the org `extra[i]` is kept in, NULL for the top org.
 * First shape ends at `extra_count`.
 */
typedef struct naula_root_file {
    uint32_t size;
    naula_reference top;
    naula_reference *added;
    size_t added_count;
    char **dropped;
    size_t dropped_count;
    naula_reference *extra;
    size_t extra_count;
    char **extra_orgs;
} naula_root_file;

/** NAULA_NOT_FOUND when the root has no `.naula.json`. Release with naula_root_file_release. */
naula_status naula_root_file_read(const char *root, naula_root_file *file, naula_refusal *refusal);
/** Replaces `<root>/.naula.json` whole, through a temporary beside it. */
naula_status naula_root_file_write(const char *root, const naula_root_file *file, naula_refusal *refusal);
void naula_root_file_release(naula_root_file *file);

#ifdef __cplusplus
}
#endif

#endif
