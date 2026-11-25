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
 * The program is looked for on PATH only, never in the current directory. No shell is involved, so an argument reaches the program as written, and no
 * console window opens. NAULA_NOT_FOUND with code `program-not-found` says the program does not exist; a program
 * that ran and failed is NAULA_OK with its `exit_code`. Release the result with naula_run_result_release.
 */
naula_status naula_run(const char *const *arguments, const char *directory, const char *const *environment,
                       naula_run_result *result, naula_refusal *refusal);

void naula_run_result_release(naula_run_result *result);

#ifdef __cplusplus
}
#endif

#endif
