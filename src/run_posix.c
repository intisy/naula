#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "naula/memory.h"
#include "run_internal.h"

#define POLL_MILLISECONDS 20

typedef enum child_stage { STAGE_DIRECTORY = 1, STAGE_EXEC = 2 } child_stage;

typedef struct child_failure {
    int stage;
    int error;
} child_failure;

typedef struct run_pipe {
    int ends[2];
} run_pipe;

static int open_pipe(run_pipe *made) {
    if (pipe(made->ends) != 0) {
        return 0;
    }
    if (fcntl(made->ends[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(made->ends[1], F_SETFD, FD_CLOEXEC) != 0) {
        close(made->ends[0]);
        close(made->ends[1]);
        return 0;
    }
    return 1;
}

static void close_descriptor(int *descriptor) {
    if (*descriptor >= 0) {
        close(*descriptor);
    }
    *descriptor = -1;
}

static void close_pipe(run_pipe *made) {
    close_descriptor(&made->ends[0]);
    close_descriptor(&made->ends[1]);
}

static void report_and_exit(int report_descriptor, int stage, int error) {
    child_failure failure = {stage, error};
    ssize_t written = write(report_descriptor, &failure, sizeof failure);
    (void)written;
    _exit(127);
}

static void run_child(const char *const *arguments, const char *directory, const run_pipe *output, const run_pipe *error,
                      const run_pipe *report) {
    int null_device = open("/dev/null", O_RDONLY);
    if (directory != NULL && chdir(directory) != 0) {
        report_and_exit(report->ends[1], STAGE_DIRECTORY, errno);
    }
    if (null_device < 0 || dup2(null_device, 0) < 0 || dup2(output->ends[1], 1) < 0 || dup2(error->ends[1], 2) < 0) {
        report_and_exit(report->ends[1], STAGE_EXEC, errno);
    }
    execvp(arguments[0], (char *const *)arguments);
    report_and_exit(report->ends[1], STAGE_EXEC, errno);
}

static int read_failure(int descriptor, child_failure *failure) {
    ssize_t got;
    do {
        got = read(descriptor, failure, sizeof *failure);
    } while (got < 0 && errno == EINTR);
    return got == (ssize_t)sizeof *failure;
}

static int exit_code_of(int status) {
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

static void wait_for(pid_t child, int *status) {
    while (waitpid(child, status, 0) < 0 && errno == EINTR) {
    }
}

/** Reads one chunk; 0 once the descriptor ended, 1 otherwise, setting *out_of_memory when the chunk did not fit. */
static int take_chunk(int descriptor, run_buffer *buffer, int *out_of_memory) {
    char chunk[4096];
    ssize_t got = read(descriptor, chunk, sizeof chunk);
    if (got > 0) {
        *out_of_memory = *out_of_memory || !run_buffer_append(buffer, chunk, (size_t)got);
        return 1;
    }
    return got < 0 && errno == EINTR;
}

/**
 * 1 once the child exited, its output read to the end meanwhile.
 *
 * @implNote Output is read only until the child has exited and nothing is left to read, so a grandchild that
 * inherited a pipe, as a detached git maintenance run does, never holds the run open.
 */
static int collect(pid_t child, int output_descriptor, int error_descriptor, run_buffer *output, run_buffer *error,
                   int *status, int *out_of_memory) {
    int descriptors[2] = {output_descriptor, error_descriptor};
    run_buffer *buffers[2] = {output, error};
    int exited = 0;
    while (descriptors[0] >= 0 || descriptors[1] >= 0) {
        struct pollfd waiting[2];
        int slot_of[2];
        int count = 0;
        for (int slot = 0; slot < 2; slot++) {
            if (descriptors[slot] >= 0) {
                waiting[count].fd = descriptors[slot];
                waiting[count].events = POLLIN;
                waiting[count].revents = 0;
                slot_of[count++] = slot;
            }
        }
        int ready = poll(waiting, (nfds_t)count, exited ? 0 : POLL_MILLISECONDS);
        if ((ready < 0 && errno != EINTR) || (ready == 0 && exited)) {
            break;
        }
        for (int position = 0; ready > 0 && position < count; position++) {
            int slot = slot_of[position];
            if (waiting[position].revents != 0 && !take_chunk(waiting[position].fd, buffers[slot], out_of_memory)) {
                descriptors[slot] = -1;
            }
        }
        if (!exited && waitpid(child, status, WNOHANG) == child) {
            exited = 1;
        }
    }
    return exited;
}

static naula_status refuse_child_failure(const child_failure *failure, const char *program, const char *directory,
                                         naula_refusal *refusal) {
    if (failure->error == ENOMEM) {
        return NAULA_OUT_OF_MEMORY;
    }
    if (failure->stage == STAGE_DIRECTORY) {
        naula_refuse(refusal, "run-directory-missing", "run it from a folder that exists", "cannot enter %s: %s",
                     directory, strerror(failure->error));
        return NAULA_REFUSED;
    }
    if (failure->error == ENOENT) {
        naula_refuse(refusal, "program-not-found", "install it and put it on PATH", "%s was not found", program);
        return NAULA_NOT_FOUND;
    }
    naula_refuse(refusal, "run-failed", "check the program is executable", "cannot run %s: %s", program,
                 strerror(failure->error));
    return NAULA_REFUSED;
}

static naula_status run_started(pid_t child, run_pipe *output, run_pipe *error, run_pipe *report,
                                const char *const *arguments, const char *directory, naula_run_result *result,
                                naula_refusal *refusal) {
    close_descriptor(&output->ends[1]);
    close_descriptor(&error->ends[1]);
    close_descriptor(&report->ends[1]);
    child_failure failure;
    int status = 0;
    if (read_failure(report->ends[0], &failure)) {
        wait_for(child, &status);
        return refuse_child_failure(&failure, arguments[0], directory, refusal);
    }
    run_buffer collected_output = {NULL, 0, 0};
    run_buffer collected_error = {NULL, 0, 0};
    int out_of_memory = 0;
    if (!collect(child, output->ends[0], error->ends[0], &collected_output, &collected_error, &status, &out_of_memory)) {
        wait_for(child, &status);
    }
    if (out_of_memory) {
        run_buffer_discard(&collected_output);
        run_buffer_discard(&collected_error);
        return NAULA_OUT_OF_MEMORY;
    }
    return run_finish(result, &collected_output, &collected_error, exit_code_of(status));
}

naula_status naula_run(const char *const *arguments, const char *directory, naula_run_result *result,
                       naula_refusal *refusal) {
    naula_status begun = run_begin(arguments, result, refusal);
    if (begun != NAULA_OK) {
        return begun;
    }
    run_pipe output = {{-1, -1}};
    run_pipe error = {{-1, -1}};
    run_pipe report = {{-1, -1}};
    naula_status status = NAULA_OK;
    if (!open_pipe(&output) || !open_pipe(&error) || !open_pipe(&report)) {
        naula_refuse(refusal, "run-failed", "close some files and try again", "cannot open a pipe: %s", strerror(errno));
        status = errno == ENOMEM ? NAULA_OUT_OF_MEMORY : NAULA_REFUSED;
    } else {
        pid_t child = fork();
        if (child < 0) {
            naula_refuse(refusal, "run-failed", "close some programs and try again", "cannot start %s: %s", arguments[0],
                         strerror(errno));
            status = errno == ENOMEM ? NAULA_OUT_OF_MEMORY : NAULA_REFUSED;
        } else if (child == 0) {
            run_child(arguments, directory, &output, &error, &report);
        } else {
            status = run_started(child, &output, &error, &report, arguments, directory, result, refusal);
        }
    }
    close_pipe(&output);
    close_pipe(&error);
    close_pipe(&report);
    return status;
}

#endif
