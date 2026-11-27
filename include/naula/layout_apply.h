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
 * finds the disk already as it asks changes nothing, so a plan applied twice changes nothing the second time. A step
 * whose `path` or `target` names `.naula-journal` or `.naula.json` is refused as `apply-input-invalid`, on Windows
 * also by a name Windows opens as one of them: with a trailing dot or space, or as an 8.3 alias such as `NAULA-~1`;
 * and on every platform by any first component below the root that the disk opens as one of them, compared by file
 * identity without following a link, so an alias no rule on names knows is refused as well.
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
 * Applies `plan` in its order, journalled in `<root>/.naula-journal/<sequence>-<UTC yyyymmddThhmmssZ>.jsonl`, the
 * sequence of ten to nineteen digits, one past the largest there: journals are ordered by it, never by the clock, which
 * can be set back; the time is only information. A root whose journals cannot be put in order refuses every run, every
 * resume and every search for an unfinished journal: a name of a journal's form whose sequence naula cannot read as
 * `journal-sequence-invalid`, two journals of one sequence, as a copied journal leaves, as `journal-sequence-duplicate`
 * naming both. A journal of the largest sequence leaves none for the next run, refused as `journal-sequence-exhausted`.
 * A journal named in the form before sequences is not read. The whole plan goes first, then a line before each step (`intended`) and after it (`done`), each flushed to the disk before the step
 * goes on. Stops at the first step it refuses. A refusal that found the disk exactly at the step's start ends the
 * journal as `stopped`, so a new plan may be applied; one that came after part of a step's change, an allocation that
 * failed, or a process that died leaves it unfinished, and while any journal of the root is unfinished a new run is
 * refused as `journal-unfinished`: that journal is resumed first.
 *
 * The run holds `<root>/.naula-journal/lock`, made in one exclusive create naming its process id, and removes it at
 * the end. A lock another running naula holds refuses as `root-locked`; one whose process is gone, left by a run that
 * died, refuses as `stale-lock` and is never taken over: it is removed by hand once no naula runs.
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
 *   inside it: each is removed itself. The root is opened once and every folder from it down to and inside the clone
 *   through the open handle of the one before it, judged by its own handle, never by a listing or a path, so a link
 *   swapped in anywhere on the way is refused or removed, never followed. On Windows each folder is held against
 *   rename while it is emptied, and read-only is ignored rather than cleared, since a file's attributes belong to all
 *   its hard links. Any depth is removed. A file another process holds stops the run with `folder-held`, and a
 *   folder another program keeps filling, found refilled on 8 listings after its first whole one, with
 *   `folder-refills`; either leaves the journal to be resumed. On POSIX a folder inside the clone with another file
 *   system mounted on it, a bind mount included, is never entered: the run stops with `mount-inside`, to be resumed
 *   once it is unmounted. On Windows a mounted folder is a reparse point, removed itself.
 * - REPORT changes nothing.
 *
 * What it does not guard: each step but REMOVE_CLONE reads the disk and then changes it, so another program that swaps
 * a folder above a step's path for a link, or makes something at a target, between that check and that change is not
 * caught. A move
 * never replaces its target on Windows and on Linux (renameat2 with RENAME_NOREPLACE); elsewhere, or on a Linux file
 * system without it, it checks that the target is missing and then renames, since rename(2) would replace an empty
 * folder there. The lock covers naula against itself only, never another program.
 */
naula_status naula_layout_apply(const naula_layout_plan *plan, const naula_apply_options *options, naula_refusal *refusal);

/**
 * Carries on the run `journal_file` records from where it stopped, checking every remaining step against the disk
 * as naula_layout_apply does, and appending to the same journal. The journal must sit directly in
 * `<options->root>/.naula-journal/` and name that root. A journal that finished changes nothing. One that stopped
 * cleanly is refused as `journal-stopped`, since its plan was made for the disk as it stood then: a new plan is made
 * instead. One while the root holds a journal of a larger sequence is refused as `journal-superseded`. It takes the root's lock
 * as naula_layout_apply does.
 */
naula_status naula_journal_resume(const char *journal_file, const naula_apply_options *options, naula_refusal *refusal);

/**
 * The journal of the largest sequence in `<root>/.naula-journal/` that neither finished nor stopped cleanly, NAULA_NOT_FOUND when there
 * is none. A journal whose opening append was cut short, holding part of its plan and no step begun, stopped cleanly.
 */
naula_status naula_journal_unfinished(const char *root, char **journal_file, naula_refusal *refusal);

/**
 * Claude Code's memory bank key for a folder: `real_path` with every character that is not an ASCII letter or digit
 * replaced by `-`, one for each UTF-16 unit it takes, as `C:\Users\finn\.claude\verbrauch` is
 * `C--Users-finn--claude-verbrauch`; the drive letter keeps its case. Past 200 units the key is its first 200, `-`,
 * and the base 36 magnitude of a 32-bit hash of the path's UTF-16 units (`h = h * 31 + unit`, signed), as Claude Code
 * 2.1.332 does (measured). A key that does not fit `capacity` leaves `key` empty, never cut short, since a shortened
 * key names another folder's bank.
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
 * repointed reads as unrecorded. Another reparse point reads as an unrecorded link with an empty target. A recorded
 * link the disk no longer holds exactly as recorded is first forgotten, under the root's lock and journalled as
 * `forgotten`, so a link someone makes at that path later is never read as naula's. While a journal of the root is
 * unfinished, a scan that would forget refuses as `journal-unfinished`, since its newer journal would leave the
 * unfinished one unresumable.
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
/**
 * Replaces `<root>/.naula.json` whole, through a temporary beside it, holding the root's lock as naula_layout_apply
 * does, so it refuses as `root-locked` or `stale-lock` alike.
 */
naula_status naula_root_file_write(const char *root, const naula_root_file *file, naula_refusal *refusal);
void naula_root_file_release(naula_root_file *file);

#ifdef __cplusplus
}
#endif

#endif
