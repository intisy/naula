#ifndef NAULA_LAYOUT_APPLY_INTERNAL_H
#define NAULA_LAYOUT_APPLY_INTERNAL_H

#include <stddef.h>

#include "disk.h"
#include "naula/layout_apply.h"

#define APPLY_JOURNAL_FOLDER ".naula-journal"
#define APPLY_LINK_RECORD "links.json"

/** Separators read alike; on Windows, whose file names fold their case, ASCII case folded too. */
int apply_same_path(const char *left, const char *right);
/** What follows `root` and one separator in `path`, NULL when `path` is not strictly below `root` as written. */
const char *apply_below(const char *root, const char *path);
/** Whether two link targets are the same text once separators read alike. */
int apply_same_target(const char *left, const char *right);

/** Fills `refusal` with the path and the system's text for `error`. Answers NAULA_REFUSED. */
naula_status apply_refuse_disk(naula_refusal *refusal, const char *code, const char *fix, const char *what,
                               const char *path, unsigned long error);

/**
 * Refuses a root, or a `.naula-journal` in it, that is anything but a real folder; with `make`, a missing journal
 * folder is made. Every reader and writer of naula's own files at a root starts here.
 */
naula_status apply_check_root(const char *root, int make, naula_refusal *refusal);

/** The links naula recorded making at a root, in `<root>/.naula-journal/links.json`. */
typedef struct link_record {
    naula_layout_link *links;
    size_t count;
} link_record;

naula_status link_record_read(const char *root, link_record *record, naula_refusal *refusal);
/** Records `path` as a link to `target`, replacing any entry for `path`, and writes the record. */
naula_status link_record_put(const char *root, link_record *record, const char *path, const char *target,
                             naula_refusal *refusal);
/** Forgets `path` and writes the record; nothing to forget writes nothing. */
naula_status link_record_forget(const char *root, link_record *record, const char *path, naula_refusal *refusal);
const naula_layout_link *link_record_find(const link_record *record, const char *path);
void link_record_release(link_record *record);

/** How far a journal saw a step go. A link step is complete only once its record is written. */
typedef enum journal_progress {
    JOURNAL_NONE = 0,
    JOURNAL_INTENDED = 1,
    JOURNAL_DONE = 2,
    JOURNAL_RECORDED = 3
} journal_progress;

typedef struct journal {
    disk_appender *appender;
    char *path;
} journal;

#define JOURNAL_SEQUENCE_WIDTH 10
#define JOURNAL_SEQUENCE_DIGITS_LIMIT 19
#define JOURNAL_SEQUENCE_LARGEST 9999999999999999999ull

typedef enum journal_name_kind {
    JOURNAL_NAME_OTHER = 0,
    JOURNAL_NAME_SEQUENCED = 1,
    JOURNAL_NAME_UNREADABLE = 2
} journal_name_kind;

/**
 * Reads a name of a journal's form, `<sequence>-<UTC yyyymmddThhmmssZ>.jsonl`: SEQUENCED, with `sequence` set, when the
 * sequence has 10 to 19 digits, which always fits; UNREADABLE when it has fewer or more, which no listing may pass
 * over as no journal; OTHER for any other name. Journals are ordered by the sequence and never by the time, which a
 * clock set back would reorder.
 *
 * @implNote A journal named in the form before sequences, `<UTC yyyymmddThhmmssZ>.jsonl`, is OTHER and never read,
 * since naula supports no outdated form of its own files.
 */
journal_name_kind journal_name_read(const char *name, unsigned long long *sequence);

typedef struct journal_name {
    char *name;
    unsigned long long sequence;
} journal_name;

typedef struct journal_listing {
    journal_name *journals;
    size_t count;
} journal_listing;

/**
 * The journals directly in `folder`, the largest sequence first. A name of a journal's form whose sequence cannot be
 * read refuses the listing as `journal-sequence-invalid`, and two journals of one sequence, as a copied journal
 * leaves, as `journal-sequence-duplicate` naming both, so every run and resume stops until the file is moved.
 */
naula_status journal_list(const char *folder, journal_listing *listing, naula_refusal *refusal);
void journal_listing_release(journal_listing *listing);

/** Creates a new journal for `plan` at `root`, holding the root and every step before any step runs. */
naula_status journal_start(const char *root, const naula_layout_plan *plan, journal *journal, naula_refusal *refusal);
/** Appends one progress line for `step`; `code` and `previous` are written only when set. */
naula_status journal_mark(journal *journal, size_t step, const char *state, const char *code, const char *previous,
                          naula_refusal *refusal);
naula_status journal_finish(journal *journal, naula_refusal *refusal);
/** Says the record forgot a link it held, because the disk no longer holds it as recorded. */
naula_status journal_forgotten(journal *journal, const char *path, const char *target, naula_refusal *refusal);
/** Ends a run a refusal stopped while the disk stood exactly at the start of a step. */
naula_status journal_stop(journal *journal, naula_refusal *refusal);
void journal_close(journal *journal);

typedef struct journal_contents {
    char *root;
    naula_layout_plan plan;
    unsigned char *progress;
    int finished;
    int stopped;
    int ends_mid_line;
    int progressed;
} journal_contents;

/**
 * Reads a journal back. A last line cut short by a crash is ignored; any other malformed line refuses. One whose
 * opening append was cut short, its plan incomplete and no step begun, reads as stopped with an empty plan.
 */
naula_status journal_load(const char *path, journal_contents *contents, naula_refusal *refusal);
/** Opens an existing journal to append to, first ending a line a crash cut short. */
naula_status journal_reopen(const char *path, int ends_mid_line, journal *journal, naula_refusal *refusal);
void journal_contents_release(journal_contents *contents);

/** The memory bank folder of `real_path` under `banks`, NULL when memory runs out or the key does not fit. */
char *memory_bank_path(const char *banks, const char *real_path);

/**
 * Takes `<root>/.naula-journal/lock` in one exclusive create holding this process's id, so two naula never lay out
 * one root at once. A lock held by a running process refuses as `root-locked`; one whose process is gone refuses as
 * `stale-lock` and is never taken over, since the run it belonged to may have stopped halfway.
 */
naula_status root_lock_take(const char *root, disk_owned_file **lock, naula_refusal *refusal);
/**
 * Removes the lock this run made, allocating nothing, so a run that ran out of memory still leaves no lock. NULL
 * does nothing.
 */
void root_lock_release(disk_owned_file *lock);

#ifdef NAULA_TEST_HOOKS
/**
 * Makes the applier stop as if the process died at the `checkpoint`th boundary it reaches from now, 1 being the
 * first, answering NAULA_REFUSED `test-stopped`; 0 disarms. Boundaries fall after each journal line, after each change
 * to the disk, and between a checkout's move and its memory bank's.
 */
void naula_apply_test_stop_at(unsigned long checkpoint);
/** How many boundaries the applier reached since the last naula_apply_test_stop_at. */
unsigned long naula_apply_test_boundaries(void);
#endif

#endif
