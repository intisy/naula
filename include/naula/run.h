#ifndef NAULA_RUN_H
#define NAULA_RUN_H

#include <stddef.h>
#include <stdint.h>

#include "naula/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * What a finished program wrote: `output` is its standard output and `error` its standard error, both NUL-terminated
 * and owned by the result. First shape ends at `error`.
 */
typedef struct naula_run_result {
    uint32_t size;
    int exit_code;
    char *output;
    size_t output_length;
    char *error;
} naula_run_result;

/**
 * Runs `arguments[0]` with the NULL-terminated `arguments`, found on PATH, in `directory` (NULL keeps the caller's),
 * and waits for it, standard input empty. `environment` is NULL or a NULL-terminated list of `NAME=value` entries
 * the child sees on top of this process's environment, replacing a variable of the same name; a value may be empty.
 * An entry without `=` removes that variable instead, and one ending in `*` every variable whose name starts with
 * the text before the `*`.
 * The program is looked for on PATH only, never in the current directory. No shell is involved, so an argument reaches the program as written, and no
 * console window opens. NAULA_NOT_FOUND with code `program-not-found` says the program does not exist; a program
 * that ran and failed is NAULA_OK with its `exit_code`. Release the result with naula_run_result_release.
 */
naula_status naula_run(const char *const *arguments, const char *directory, const char *const *environment,
                       naula_run_result *result, naula_refusal *refusal);

/**
 * naula_run with `input_length` bytes of `input` on the program's standard input, then end of file; NULL `input` is
 * naula_run. Input over NAULA_RUN_INPUT_LIMIT is refused as `run-input-too-large`, since it is written before the
 * program starts and must fit the pipe whatever the system.
 */
naula_status naula_run_with_input(const char *const *arguments, const char *directory,
                                  const char *const *environment, const char *input, size_t input_length,
                                  naula_run_result *result, naula_refusal *refusal);

#define NAULA_RUN_INPUT_LIMIT 4096

/**
 * What stops a run early. A program still running `time_limit_milliseconds` after it started (0 for no limit), or
 * one whose `keep_going` answers 0 when asked, every few milliseconds while it runs, is stopped together with every
 * program it started: NAULA_TIMED_OUT `run-timed-out` or NAULA_REFUSED `run-stopped`, its output discarded. A program
 * seen ending at or past its time limit ran past it too and is refused the same way, however soon after the last
 * check it ended.
 * First shape ends at `context`.
 */
typedef struct naula_run_limits {
    uint32_t size;
    uint64_t time_limit_milliseconds;
    int (*keep_going)(void *context);
    void *context;
} naula_run_limits;

/** naula_run_with_input under `limits`; NULL `limits` is naula_run_with_input. */
naula_status naula_run_limited(const char *const *arguments, const char *directory, const char *const *environment,
                               const char *input, size_t input_length, const naula_run_limits *limits,
                               naula_run_result *result, naula_refusal *refusal);

void naula_run_result_release(naula_run_result *result);

#ifdef __cplusplus
}
#endif

#endif
