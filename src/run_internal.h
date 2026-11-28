#ifndef NAULA_RUN_INTERNAL_H
#define NAULA_RUN_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "naula/run.h"

typedef struct run_buffer {
    char *bytes;
    size_t length;
    size_t capacity;
} run_buffer;

/** 1 once bytes are appended, 0 when memory runs out. */
int run_buffer_append(run_buffer *buffer, const char *bytes, size_t length);

void run_buffer_discard(run_buffer *buffer);

/** Checks `arguments` and empties `result`; a refusal says why not. */
naula_status run_begin(const char *const *arguments, naula_run_result *result, naula_refusal *refusal);

/** Refuses input over NAULA_RUN_INPUT_LIMIT. */
naula_status run_check_input(const char *input, size_t input_length, naula_refusal *refusal);

typedef enum run_stop { RUN_NOT_STOPPED = 0, RUN_TIMED_OUT = 1, RUN_STOPPED = 2 } run_stop;

/** A run's limits with the moment it started, both platforms counting milliseconds on a monotonic clock. */
typedef struct run_limits_clock {
    const naula_run_limits *limits;
    uint64_t started;
} run_limits_clock;

/** Whether the run must stop now: past its time limit, or its `keep_going` answered 0. */
run_stop run_limits_judge(const run_limits_clock *clock, uint64_t now);

/** RUN_TIMED_OUT for a program seen ended at `ended`, at or past its time limit; RUN_NOT_STOPPED otherwise. */
run_stop run_limits_judge_ended(const run_limits_clock *clock, uint64_t ended);

/** The refusal of a run its limits stopped: NAULA_TIMED_OUT `run-timed-out` or NAULA_REFUSED `run-stopped`. */
naula_status run_refuse_stopped(run_stop stopped, const naula_run_limits *limits, const char *program,
                                naula_refusal *refusal);

/** Hands both buffers to `result` as NUL-terminated text, or discards them on running out of memory. */
naula_status run_finish(naula_run_result *result, run_buffer *output, run_buffer *error, int exit_code);

#endif
