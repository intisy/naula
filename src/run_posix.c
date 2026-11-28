#if !defined(_WIN32)
#if defined(__linux__)
#define _GNU_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "naula/memory.h"
#include "run_internal.h"

#define POLL_MILLISECONDS 20

extern char **environ;

typedef enum child_stage { STAGE_DIRECTORY = 1, STAGE_EXEC = 2 } child_stage;

typedef struct child_failure {
    int stage;
    int error;
} child_failure;

typedef struct run_pipe {
    int ends[2];
} run_pipe;

/** What went wrong while collecting, beyond the child's own exit. */
typedef struct collect_state {
    int out_of_memory;
    int lost_child;
    int poll_failed;
    run_stop stopped;
} collect_state;

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

/** @implNote Both ends close on exec at once through pipe2 where it exists, so no process another thread starts carries one away. */
static int open_pipe(run_pipe *made) {
    made->ends[0] = -1;
    made->ends[1] = -1;
#if defined(__linux__)
    return pipe2(made->ends, O_CLOEXEC) == 0;
#else
    if (pipe(made->ends) != 0) {
        made->ends[0] = -1;
        made->ends[1] = -1;
        return 0;
    }
    if (fcntl(made->ends[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(made->ends[1], F_SETFD, FD_CLOEXEC) != 0) {
        int saved = errno;
        close_pipe(made);
        errno = saved;
        return 0;
    }
    return 1;
#endif
}

static void report_and_exit(int report_descriptor, int stage, int error) {
    child_failure failure = {stage, error};
    ssize_t written = write(report_descriptor, &failure, sizeof failure);
    (void)written;
    _exit(127);
}

static void restore_signals(void) {
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
}

/** What the child execs, all of it made before fork so the child calls nothing that allocates. */
typedef struct child_program {
    char *path;
    char **environment;
} child_program;

static size_t name_length(const char *entry) {
    const char *equals = strchr(entry, '=');
    return equals == NULL ? strlen(entry) : (size_t)(equals - entry);
}

static int is_removal(const char *addition) {
    return strchr(addition, '=') == NULL;
}

static int names_entry(const char *addition, const char *entry) {
    size_t length = name_length(addition);
    if (is_removal(addition) && length > 0 && addition[length - 1] == '*') {
        return strncmp(entry, addition, length - 1) == 0 && name_length(entry) >= length - 1;
    }
    return strncmp(entry, addition, length) == 0 && entry[length] == '=';
}

static int overrides(const char *entry, const char *const *additions) {
    for (size_t index = 0; additions != NULL && additions[index] != NULL; index++) {
        if (names_entry(additions[index], entry)) {
            return 1;
        }
    }
    return 0;
}

static void release_program(child_program *program) {
    naula_memory_release(program->path);
    naula_memory_release(program->environment);
    memset(program, 0, sizeof *program);
}

/** This process's environment with the additions replacing same-named entries; NULL when memory ran out. */
static char **merged_environment(const char *const *additions) {
    size_t inherited = 0;
    size_t added = 0;
    while (environ != NULL && environ[inherited] != NULL) {
        inherited++;
    }
    while (additions != NULL && additions[added] != NULL) {
        added++;
    }
    char **merged = naula_memory_allocate((inherited + added + 1) * sizeof *merged);
    size_t count = 0;
    for (size_t index = 0; merged != NULL && index < inherited; index++) {
        if (!overrides(environ[index], additions)) {
            merged[count++] = environ[index];
        }
    }
    for (size_t index = 0; merged != NULL && index < added; index++) {
        if (!is_removal(additions[index])) {
            merged[count++] = (char *)additions[index];
        }
    }
    if (merged != NULL) {
        merged[count] = NULL;
    }
    return merged;
}

static const char *path_of(char *const *environment) {
    for (size_t index = 0; environment[index] != NULL; index++) {
        if (strncmp(environment[index], "PATH=", 5) == 0) {
            return environment[index] + 5;
        }
    }
    return "/usr/bin:/bin";
}

static int is_runnable(const char *path) {
    struct stat details;
    return stat(path, &details) == 0 && S_ISREG(details.st_mode) && access(path, X_OK) == 0;
}

/**
 * The program as a path to exec, found on PATH only: an empty PATH entry, which the shell reads as the current
 * directory, is skipped. NULL when absent, with *out_of_memory set when memory ran out.
 */
static char *resolve_program(const char *name, const char *search, int *out_of_memory) {
    if (strchr(name, '/') != NULL) {
        char *copy = naula_memory_duplicate_text(name);
        *out_of_memory = copy == NULL;
        return copy;
    }
    size_t name_size = strlen(name);
    for (const char *entry = search; *entry != '\0';) {
        size_t length = strcspn(entry, ":");
        if (length > 0) {
            char *candidate = naula_memory_allocate(length + name_size + 2);
            if (candidate == NULL) {
                *out_of_memory = 1;
                return NULL;
            }
            memcpy(candidate, entry, length);
            candidate[length] = '/';
            memcpy(candidate + length + 1, name, name_size + 1);
            if (is_runnable(candidate)) {
                return candidate;
            }
            naula_memory_release(candidate);
        }
        entry += length + (entry[length] == ':' ? 1 : 0);
    }
    return NULL;
}

static naula_status prepare_program(const char *name, const char *const *additions, child_program *program,
                                    naula_refusal *refusal) {
    int out_of_memory = 0;
    memset(program, 0, sizeof *program);
    program->environment = merged_environment(additions);
    if (program->environment != NULL) {
        program->path = resolve_program(name, path_of(program->environment), &out_of_memory);
    }
    if (program->environment == NULL || out_of_memory) {
        release_program(program);
        return NAULA_OUT_OF_MEMORY;
    }
    if (program->path == NULL) {
        release_program(program);
        naula_refuse(refusal, "program-not-found", "install it and put it on PATH", "%s was not found", name);
        return NAULA_NOT_FOUND;
    }
    return NAULA_OK;
}

static void run_child(const char *const *arguments, const char *directory, const child_program *program,
                      const run_pipe *input, const run_pipe *output, const run_pipe *error, const run_pipe *report,
                      int own_group) {
    if (own_group) {
        setpgid(0, 0);
    }
    int standard_input = input->ends[0] >= 0 ? input->ends[0] : open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (directory != NULL && chdir(directory) != 0) {
        report_and_exit(report->ends[1], STAGE_DIRECTORY, errno);
    }
    if (standard_input < 0 || dup2(standard_input, 0) < 0 || dup2(output->ends[1], 1) < 0 || dup2(error->ends[1], 2) < 0) {
        report_and_exit(report->ends[1], STAGE_EXEC, errno);
    }
    restore_signals();
    execve(program->path, (char *const *)arguments, program->environment);
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

/** 1 once the child is reaped, 0 when the system has no such child to wait for (SIGCHLD ignored by the caller). */
static int wait_for(pid_t child, int *status) {
    pid_t waited;
    do {
        waited = waitpid(child, status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == child;
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

static uint64_t now_milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/** Stops the child and everything it started, which share its process group, once a limit says so. */
static void enforce_limits(pid_t child, const run_limits_clock *clock, collect_state *state) {
    if (state->stopped != RUN_NOT_STOPPED) {
        return;
    }
    state->stopped = run_limits_judge(clock, now_milliseconds());
    if (state->stopped != RUN_NOT_STOPPED) {
        kill(-child, SIGKILL);
        kill(child, SIGKILL);
    }
}

/** 1 once the child is reaped; sets state->lost_child when it cannot be. */
static int reap_if_exited(pid_t child, int *status, collect_state *state) {
    pid_t waited = waitpid(child, status, WNOHANG);
    if (waited < 0 && errno == ECHILD) {
        state->lost_child = 1;
        return 1;
    }
    return waited == child;
}

/**
 * 1 once the child exited, its output read to the end meanwhile; 0 when polling failed first.
 *
 * @implNote Output is read only until the child has exited and nothing is left to read, so a grandchild that
 * inherited a pipe, as a detached git maintenance run does, never holds the run open.
 */
static int collect(pid_t child, int output_descriptor, int error_descriptor, run_buffer *output, run_buffer *error,
                   int *status, const run_limits_clock *clock, collect_state *state) {
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
        if (ready < 0 && errno != EINTR) {
            state->poll_failed = 1;
            return exited;
        }
        if (ready == 0 && exited) {
            break;
        }
        for (int position = 0; ready > 0 && position < count; position++) {
            int slot = slot_of[position];
            if (waiting[position].revents != 0 && !take_chunk(waiting[position].fd, buffers[slot], &state->out_of_memory)) {
                descriptors[slot] = -1;
            }
        }
        exited = exited || reap_if_exited(child, status, state);
        if (!exited && clock->limits != NULL) {
            enforce_limits(child, clock, state);
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

static naula_status refuse_collecting(const collect_state *state, const char *program, naula_refusal *refusal) {
    if (state->out_of_memory) {
        return NAULA_OUT_OF_MEMORY;
    }
    naula_refuse(refusal, "run-failed", "do not ignore SIGCHLD around naula_run",
                 state->poll_failed ? "cannot read the output of %s" : "cannot wait for %s", program);
    return NAULA_REFUSED;
}

static naula_status run_started(pid_t child, run_pipe *output, run_pipe *error, run_pipe *report,
                                const char *const *arguments, const char *directory, const naula_run_limits *limits,
                                naula_run_result *result, naula_refusal *refusal) {
    if (limits != NULL) {
        setpgid(child, child);
    }
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
    collect_state state = {0, 0, 0, RUN_NOT_STOPPED};
    run_limits_clock clock = {limits, now_milliseconds()};
    int exited = collect(child, output->ends[0], error->ends[0], &collected_output, &collected_error, &status, &clock,
                         &state);
    if (state.poll_failed && !exited) {
        kill(child, SIGKILL);
    }
    if (!exited && !wait_for(child, &status)) {
        state.lost_child = 1;
    }
    if (state.out_of_memory || state.lost_child || state.poll_failed || state.stopped != RUN_NOT_STOPPED) {
        run_buffer_discard(&collected_output);
        run_buffer_discard(&collected_error);
        return state.stopped != RUN_NOT_STOPPED && !state.out_of_memory
                   ? run_refuse_stopped(state.stopped, limits, arguments[0], refusal)
                   : refuse_collecting(&state, arguments[0], refusal);
    }
    return run_finish(result, &collected_output, &collected_error, exit_code_of(status));
}

static naula_status refuse_system_failure(const char *what, const char *program, naula_refusal *refusal) {
    int saved = errno;
    naula_refuse(refusal, "run-failed", "close some programs and try again", "cannot %s %s: %s", what, program,
                 strerror(saved));
    return saved == ENOMEM ? NAULA_OUT_OF_MEMORY : NAULA_REFUSED;
}

/**
 * A pipe already holding all of `input` with its write end closed, so the child reads it and then end of file.
 *
 * @implNote Written before the child starts, while this process still holds the read end, so the write can never
 * raise SIGPIPE; run_check_input keeps it within what every system's pipe holds.
 */
static int fill_input(const char *input, size_t input_length, run_pipe *made) {
    if (!open_pipe(made)) {
        return 0;
    }
    size_t written = 0;
    while (written < input_length) {
        ssize_t wrote = write(made->ends[1], input + written, input_length - written);
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            return 0;
        }
        written += (size_t)wrote;
    }
    close_descriptor(&made->ends[1]);
    return 1;
}

naula_status naula_run_limited(const char *const *arguments, const char *directory, const char *const *environment,
                               const char *input, size_t input_length, const naula_run_limits *limits,
                               naula_run_result *result, naula_refusal *refusal) {
    naula_status begun = run_begin(arguments, result, refusal);
    if (begun == NAULA_OK) {
        begun = run_check_input(input, input_length, refusal);
    }
    if (begun != NAULA_OK) {
        return begun;
    }
    child_program program;
    begun = prepare_program(arguments[0], environment, &program, refusal);
    if (begun != NAULA_OK) {
        return begun;
    }
    run_pipe input_pipe = {{-1, -1}};
    run_pipe output = {{-1, -1}};
    run_pipe error = {{-1, -1}};
    run_pipe report = {{-1, -1}};
    naula_status status = NAULA_OK;
    if ((input != NULL && !fill_input(input, input_length, &input_pipe)) || !open_pipe(&output) ||
        !open_pipe(&error) || !open_pipe(&report)) {
        status = refuse_system_failure("open a pipe for", arguments[0], refusal);
    } else {
        pid_t child = fork();
        if (child < 0) {
            status = refuse_system_failure("start", arguments[0], refusal);
        } else if (child == 0) {
            run_child(arguments, directory, &program, &input_pipe, &output, &error, &report, limits != NULL);
        } else {
            status = run_started(child, &output, &error, &report, arguments, directory, limits, result, refusal);
        }
    }
    close_pipe(&input_pipe);
    close_pipe(&output);
    close_pipe(&error);
    close_pipe(&report);
    release_program(&program);
    return status;
}

#endif
