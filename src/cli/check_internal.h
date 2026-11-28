#ifndef NAULA_CLI_CHECK_INTERNAL_H
#define NAULA_CLI_CHECK_INTERNAL_H

#include <stddef.h>

#include "naula/repo_file.h"
#include "naula/status.h"

typedef enum check_level { CHECK_ERROR = 1, CHECK_WARNING = 2, CHECK_NOTE = 3 } check_level;

/** One thing the gate found. `line` is text to commit as it stands, NULL when there is none. */
typedef struct check_finding {
    check_level level;
    char code[64];
    char *detail;
    const char *fix;
    char *line;
} check_finding;

#define CHECK_FINDING_LIMIT 256

typedef struct check_findings {
    check_finding items[CHECK_FINDING_LIMIT];
    size_t count;
    int out_of_memory;
    int overflowed;
} check_findings;

/** Past CHECK_FINDING_LIMIT the last slot holds the error `too-many-findings`, so a gate that ran out of room fails. */
void check_add(check_findings *findings, check_level level, const char *code, const char *fix, const char *format,
               ...);

/** Attaches `line` to the finding added last, taking it. */
void check_attach_line(check_findings *findings, char *line);

void check_findings_release(check_findings *findings);

/**
 * Reads the repo's tracked files for the two rules a file's text can break: a relative path that climbs out of the
 * repo, and an `owner/repo` of one of `accounts` outside the places that must name one.
 */
naula_status check_tracked_files(const char *repository, const char *const *accounts, size_t account_count,
                                 check_findings *findings, naula_refusal *refusal);

#endif
