#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "naula/memory.h"
#include "run_internal.h"

#define POLL_MILLISECONDS 10
#define PIPE_BYTES (1024 * 1024)

typedef struct run_handles {
    HANDLE null_device;
    HANDLE input_read;
    HANDLE output_read;
    HANDLE output_write;
    HANDLE error_read;
    HANDLE error_write;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes;
    HANDLE inherited[3];
} run_handles;

static void close_handle(HANDLE *handle) {
    if (*handle != NULL && *handle != INVALID_HANDLE_VALUE) {
        CloseHandle(*handle);
    }
    *handle = NULL;
}

static void release_handles(run_handles *handles) {
    if (handles->attributes != NULL) {
        DeleteProcThreadAttributeList(handles->attributes);
        naula_memory_release(handles->attributes);
    }
    close_handle(&handles->null_device);
    close_handle(&handles->input_read);
    close_handle(&handles->output_read);
    close_handle(&handles->output_write);
    close_handle(&handles->error_read);
    close_handle(&handles->error_write);
}

/** The text as UTF-16 in a block to release, NULL when it is not UTF-8 or memory ran out (*out_of_memory says which). */
static wchar_t *widen(const char *text, int *out_of_memory) {
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (needed <= 0) {
        return NULL;
    }
    wchar_t *wide = naula_memory_allocate((size_t)needed * sizeof *wide);
    if (wide == NULL) {
        *out_of_memory = 1;
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, needed) <= 0) {
        naula_memory_release(wide);
        return NULL;
    }
    return wide;
}

static int needs_quotes(const char *argument) {
    return argument[0] == '\0' || strpbrk(argument, " \t\n\v\"") != NULL;
}

static int append_repeated(run_buffer *line, char character, size_t count) {
    for (size_t index = 0; index < count; index++) {
        if (!run_buffer_append(line, &character, 1)) {
            return 0;
        }
    }
    return 1;
}

/** Appends one argument quoted so the MSVC runtime reads it back unchanged: backslashes count only before a quote. */
static int append_argument(run_buffer *line, const char *argument) {
    if (!needs_quotes(argument)) {
        return run_buffer_append(line, argument, strlen(argument));
    }
    if (!run_buffer_append(line, "\"", 1)) {
        return 0;
    }
    size_t backslashes = 0;
    for (const char *cursor = argument; *cursor != '\0'; cursor++) {
        if (*cursor == '\\') {
            backslashes++;
            continue;
        }
        size_t doubled = *cursor == '"' ? backslashes * 2 + 1 : backslashes;
        if (!append_repeated(line, '\\', doubled) || !run_buffer_append(line, cursor, 1)) {
            return 0;
        }
        backslashes = 0;
    }
    return append_repeated(line, '\\', backslashes * 2) && run_buffer_append(line, "\"", 1);
}

static int build_command_line(const char *const *arguments, run_buffer *line) {
    for (size_t index = 0; arguments[index] != NULL; index++) {
        if ((index > 0 && !run_buffer_append(line, " ", 1)) || !append_argument(line, arguments[index])) {
            return 0;
        }
    }
    return 1;
}

static int open_pipe(HANDLE *read_end, HANDLE *write_end) {
    SECURITY_ATTRIBUTES inheritable = {sizeof inheritable, NULL, TRUE};
    if (!CreatePipe(read_end, write_end, &inheritable, PIPE_BYTES)) {
        *read_end = NULL;
        *write_end = NULL;
        return 0;
    }
    return SetHandleInformation(*read_end, HANDLE_FLAG_INHERIT, 0) != 0;
}

static HANDLE open_null_device(void) {
    SECURITY_ATTRIBUTES inheritable = {sizeof inheritable, NULL, TRUE};
    return CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

/** Lists the three handles the child inherits and no other, whatever else this process holds inheritable. */
static int prepare_attributes(run_handles *handles) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    handles->attributes = naula_memory_allocate(size);
    if (handles->attributes == NULL) {
        return 0;
    }
    if (!InitializeProcThreadAttributeList(handles->attributes, 1, 0, &size)) {
        naula_memory_release(handles->attributes);
        handles->attributes = NULL;
        return 0;
    }
    handles->inherited[0] = handles->input_read != NULL ? handles->input_read : handles->null_device;
    handles->inherited[1] = handles->output_write;
    handles->inherited[2] = handles->error_write;
    return UpdateProcThreadAttribute(handles->attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles->inherited,
                                     sizeof handles->inherited, NULL, NULL) != 0;
}

/** A pipe already holding all of `input` and closed for writing, its read end inheritable. */
static int open_input(const char *input, size_t input_length, HANDLE *read_end) {
    SECURITY_ATTRIBUTES inheritable = {sizeof inheritable, NULL, TRUE};
    HANDLE write_end = NULL;
    if (!CreatePipe(read_end, &write_end, &inheritable, PIPE_BYTES)) {
        *read_end = NULL;
        return 0;
    }
    DWORD written = 0;
    int complete = SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0) &&
                   (input_length == 0 || (WriteFile(write_end, input, (DWORD)input_length, &written, NULL) &&
                                          written == (DWORD)input_length));
    CloseHandle(write_end);
    return complete;
}

static int prepare(run_handles *handles, const char *input, size_t input_length) {
    memset(handles, 0, sizeof *handles);
    if (input != NULL && !open_input(input, input_length, &handles->input_read)) {
        return 0;
    }
    handles->null_device = open_null_device();
    if (handles->null_device == INVALID_HANDLE_VALUE) {
        handles->null_device = NULL;
        return 0;
    }
    return open_pipe(&handles->output_read, &handles->output_write) && open_pipe(&handles->error_read, &handles->error_write) &&
           prepare_attributes(handles);
}

typedef struct run_inputs {
    wchar_t *line;
    wchar_t *application;
    wchar_t *environment;
    wchar_t *wide_directory;
} run_inputs;

static int create(const run_inputs *inputs, run_handles *handles, int suspended, PROCESS_INFORMATION *information) {
    STARTUPINFOEXW startup;
    memset(&startup, 0, sizeof startup);
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = handles->inherited[0];
    startup.StartupInfo.hStdOutput = handles->output_write;
    startup.StartupInfo.hStdError = handles->error_write;
    startup.lpAttributeList = handles->attributes;
    DWORD flags = CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                  (suspended ? CREATE_SUSPENDED : 0);
    return CreateProcessW(inputs->application, inputs->line, NULL, NULL, TRUE, flags, inputs->environment,
                          inputs->wide_directory, &startup.StartupInfo, information) != 0;
}

/** Reads what the pipe holds now into buffer; sets *read_any when it took bytes. */
static void drain(HANDLE pipe, run_buffer *buffer, int *read_any, int *out_of_memory) {
    DWORD available = 0;
    char chunk[4096];
    while (PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL) && available > 0) {
        DWORD got = 0;
        if (!ReadFile(pipe, chunk, available < sizeof chunk ? available : (DWORD)sizeof chunk, &got, NULL) || got == 0) {
            return;
        }
        *read_any = 1;
        *out_of_memory = *out_of_memory || !run_buffer_append(buffer, chunk, got);
    }
}

/**
 * The child's exit code, its output read to the end meanwhile.
 *
 * @implNote Output is read only until the child has exited and nothing is left to read, so a grandchild that
 * inherited a pipe, as a detached git maintenance run does, never holds the run open.
 */
/** A job that ends every process in it once its last handle closes, so a stopped run leaves no program behind. */
static HANDLE open_job(void) {
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION information;
    memset(&information, 0, sizeof information);
    information.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job != NULL &&
        !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &information, sizeof information)) {
        CloseHandle(job);
        job = NULL;
    }
    return job;
}

typedef struct run_watch {
    run_limits_clock clock;
    HANDLE job;
    run_stop stopped;
} run_watch;

/** @implNote GetTickCount64 moves in steps of about 16 ms, too coarse to judge a limit of a few milliseconds by. */
static uint64_t now_milliseconds(void) {
    LARGE_INTEGER count;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return (uint64_t)count.QuadPart / (uint64_t)frequency.QuadPart * 1000u +
           (uint64_t)count.QuadPart % (uint64_t)frequency.QuadPart * 1000u / (uint64_t)frequency.QuadPart;
}

static void enforce_limits(HANDLE process, run_watch *watch) {
    if (watch->clock.limits == NULL || watch->stopped != RUN_NOT_STOPPED) {
        return;
    }
    watch->stopped = run_limits_judge(&watch->clock, now_milliseconds());
    if (watch->stopped != RUN_NOT_STOPPED) {
        TerminateJobObject(watch->job, 1);
        TerminateProcess(process, 1);
    }
}

#define JOB_END_WAIT_MILLISECONDS 10000u

/**
 * Waits, a bounded time, until no process is left in the stopped run's job, since ending a job is asynchronous: a
 * program still dying holds its files open, so its caller could not yet remove what it left.
 */
static void wait_for_empty_job(HANDLE job) {
    uint64_t until = now_milliseconds() + JOB_END_WAIT_MILLISECONDS;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting;
    while (now_milliseconds() < until) {
        memset(&accounting, 0, sizeof accounting);
        if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof accounting,
                                       NULL) ||
            accounting.ActiveProcesses == 0) {
            return;
        }
        Sleep(5);
    }
}

static int collect(HANDLE process, run_handles *handles, run_buffer *output, run_buffer *error, int *out_of_memory,
                   run_watch *watch) {
    for (;;) {
        enforce_limits(process, watch);
        int read_any = 0;
        drain(handles->output_read, output, &read_any, out_of_memory);
        drain(handles->error_read, error, &read_any, out_of_memory);
        if (!read_any && WaitForSingleObject(process, POLL_MILLISECONDS) == WAIT_OBJECT_0) {
            drain(handles->output_read, output, &read_any, out_of_memory);
            drain(handles->error_read, error, &read_any, out_of_memory);
            if (!read_any) {
                if (watch->stopped == RUN_NOT_STOPPED) {
                    watch->stopped = run_limits_judge_ended(&watch->clock, now_milliseconds());
                }
                break;
            }
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    return (int)code;
}

static naula_status refuse_create_failure(DWORD error, const char *program, const char *directory, naula_refusal *refusal) {
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        naula_refuse(refusal, "program-not-found", "install it and put it on PATH", "%s was not found", program);
        return NAULA_NOT_FOUND;
    }
    if (error == ERROR_DIRECTORY) {
        naula_refuse(refusal, "run-directory-missing", "run it from a folder that exists", "cannot enter %s",
                     directory == NULL ? "" : directory);
        return NAULA_REFUSED;
    }
    naula_refuse(refusal, "run-failed", "check the program is executable", "cannot run %s: Windows error %lu", program,
                 (unsigned long)error);
    return NAULA_REFUSED;
}

static naula_status run_prepared(const run_inputs *inputs, const char *const *arguments, const char *directory,
                                 run_handles *handles, const naula_run_limits *limits, naula_run_result *result,
                                 naula_refusal *refusal) {
    PROCESS_INFORMATION information = {NULL, NULL, 0, 0};
    run_watch watch = {{limits, now_milliseconds()}, NULL, RUN_NOT_STOPPED};
    if (limits != NULL) {
        watch.job = open_job();
        if (watch.job == NULL) {
            naula_refuse(refusal, "run-failed", "close some programs and try again",
                         "cannot make a job object for %s: Windows error %lu", arguments[0],
                         (unsigned long)GetLastError());
            return NAULA_REFUSED;
        }
    }
    if (!create(inputs, handles, limits != NULL, &information)) {
        DWORD failure = GetLastError();
        close_handle(&watch.job);
        return refuse_create_failure(failure, arguments[0], directory, refusal);
    }
    if (watch.job != NULL && (!AssignProcessToJobObject(watch.job, information.hProcess) ||
                              ResumeThread(information.hThread) == (DWORD)-1)) {
        DWORD failure = GetLastError();
        TerminateProcess(information.hProcess, 1);
        WaitForSingleObject(information.hProcess, INFINITE);
        CloseHandle(information.hThread);
        CloseHandle(information.hProcess);
        close_handle(&watch.job);
        naula_refuse(refusal, "run-failed", "close some programs and try again",
                     "cannot start %s inside a job object: Windows error %lu", arguments[0], (unsigned long)failure);
        return NAULA_REFUSED;
    }
    watch.clock.started = now_milliseconds();
    close_handle(&handles->output_write);
    close_handle(&handles->error_write);
    CloseHandle(information.hThread);
    run_buffer output = {NULL, 0, 0};
    run_buffer error = {NULL, 0, 0};
    int out_of_memory = 0;
    int exit_code = collect(information.hProcess, handles, &output, &error, &out_of_memory, &watch);
    if (watch.stopped != RUN_NOT_STOPPED && watch.job != NULL) {
        TerminateJobObject(watch.job, 1);
        wait_for_empty_job(watch.job);
    }
    CloseHandle(information.hProcess);
    close_handle(&watch.job);
    if (out_of_memory || watch.stopped != RUN_NOT_STOPPED) {
        run_buffer_discard(&output);
        run_buffer_discard(&error);
        return out_of_memory ? NAULA_OUT_OF_MEMORY : run_refuse_stopped(watch.stopped, limits, arguments[0], refusal);
    }
    return run_finish(result, &output, &error, exit_code);
}

static int has_separator(const wchar_t *name) {
    return wcspbrk(name, L"\\/:") != NULL;
}

static wchar_t *copy_wide(const wchar_t *text) {
    wchar_t *copy = naula_memory_allocate((wcslen(text) + 1) * sizeof *copy);
    if (copy != NULL) {
        wcscpy(copy, text);
    }
    return copy;
}

static wchar_t *search_path(const wchar_t *path, const wchar_t *name, int *out_of_memory) {
    DWORD capacity = MAX_PATH;
    for (;;) {
        wchar_t *found = naula_memory_allocate(capacity * sizeof *found);
        if (found == NULL) {
            *out_of_memory = 1;
            return NULL;
        }
        DWORD length = SearchPathW(path, name, L".exe", capacity, found, NULL);
        if (length > 0 && length < capacity) {
            return found;
        }
        naula_memory_release(found);
        if (length == 0) {
            return NULL;
        }
        capacity = length + 1;
    }
}

/**
 * The full path of the program, searched on PATH only; NULL when absent.
 *
 * @implNote SearchPathW given an explicit path never looks in the current directory, which a bare CreateProcessW
 * does first, so a program planted beside the caller cannot stand in for a tool.
 */
static wchar_t *resolve_program(const wchar_t *name, int *out_of_memory) {
    if (has_separator(name)) {
        wchar_t *copy = copy_wide(name);
        *out_of_memory = copy == NULL;
        return copy;
    }
    DWORD path_length = GetEnvironmentVariableW(L"PATH", NULL, 0);
    wchar_t *path = path_length == 0 ? NULL : naula_memory_allocate(path_length * sizeof *path);
    *out_of_memory = path_length != 0 && path == NULL;
    wchar_t *found = NULL;
    if (path != NULL && GetEnvironmentVariableW(L"PATH", path, path_length) != 0) {
        found = search_path(path, name, out_of_memory);
    }
    naula_memory_release(path);
    return found;
}

static int is_removal(const wchar_t *addition) {
    return wcschr(addition, L'=') == NULL;
}

static int overrides(const wchar_t *entry, const wchar_t *addition) {
    size_t length = 0;
    while (addition[length] != L'\0' && addition[length] != L'=') {
        length++;
    }
    if (entry[0] == L'=') {
        return 0;
    }
    if (is_removal(addition) && length > 0 && addition[length - 1] == L'*') {
        const wchar_t *equals = wcschr(entry, L'=');
        size_t entry_name = equals == NULL ? wcslen(entry) : (size_t)(equals - entry);
        return entry_name >= length - 1 && _wcsnicmp(entry, addition, length - 1) == 0;
    }
    return wcslen(entry) > length && entry[length] == L'=' && _wcsnicmp(entry, addition, length) == 0;
}

static int is_overridden(const wchar_t *entry, wchar_t *const *additions, size_t count) {
    for (size_t index = 0; index < count; index++) {
        if (overrides(entry, additions[index])) {
            return 1;
        }
    }
    return 0;
}

static int append_wide(run_buffer *block, const wchar_t *text) {
    return run_buffer_append(block, (const char *)text, (wcslen(text) + 1) * sizeof *text);
}

static wchar_t fold(wchar_t letter) {
    return (wchar_t)(uintptr_t)CharUpperW((LPWSTR)(uintptr_t)letter);
}

/** Entries whose name starts with `=` (the per-drive directories) first, then by name ignoring case. */
static int compare_entries(const void *left, const void *right) {
    const wchar_t *first = *(const wchar_t *const *)left;
    const wchar_t *second = *(const wchar_t *const *)right;
    if ((first[0] == L'=') != (second[0] == L'=')) {
        return first[0] == L'=' ? -1 : 1;
    }
    for (;; first++, second++) {
        wchar_t a = first[0];
        wchar_t b = second[0];
        a = (a == L'\0' || a == L'=') ? L'\0' : fold(a);
        b = (b == L'\0' || b == L'=') ? L'\0' : fold(b);
        if (a != b || a == L'\0') {
            return a < b ? -1 : a > b;
        }
    }
}

/**
 * This process's environment with the additions replacing same-named entries, as a block to release.
 *
 * @implNote CreateProcessW documents the block as sorted by name, case-insensitively, so it is sorted here.
 */
static wchar_t *merged_environment(wchar_t *const *additions, size_t count) {
    wchar_t *inherited = GetEnvironmentStringsW();
    if (inherited == NULL) {
        return NULL;
    }
    size_t entries = count;
    for (const wchar_t *entry = inherited; inherited != NULL && *entry != L'\0'; entry += wcslen(entry) + 1) {
        entries++;
    }
    const wchar_t **sorted = naula_memory_allocate((entries + 1) * sizeof *sorted);
    size_t kept = 0;
    for (const wchar_t *entry = inherited; sorted != NULL && *entry != L'\0'; entry += wcslen(entry) + 1) {
        if (!is_overridden(entry, additions, count)) {
            sorted[kept++] = entry;
        }
    }
    for (size_t index = 0; sorted != NULL && index < count; index++) {
        if (!is_removal(additions[index])) {
            sorted[kept++] = additions[index];
        }
    }
    run_buffer block = {NULL, 0, 0};
    int complete = sorted != NULL;
    if (complete) {
        qsort(sorted, kept, sizeof *sorted, compare_entries);
    }
    for (size_t index = 0; complete && index < kept; index++) {
        complete = append_wide(&block, sorted[index]);
    }
    static const wchar_t terminator = L'\0';
    complete = complete && run_buffer_append(&block, (const char *)&terminator, sizeof terminator);
    naula_memory_release(sorted);
    if (inherited != NULL) {
        FreeEnvironmentStringsW(inherited);
    }
    if (!complete) {
        run_buffer_discard(&block);
        return NULL;
    }
    return (wchar_t *)block.bytes;
}

static size_t list_length(const char *const *list) {
    size_t count = 0;
    while (list != NULL && list[count] != NULL) {
        count++;
    }
    return count;
}

static void release_wide_list(wchar_t **list, size_t count) {
    for (size_t index = 0; list != NULL && index < count; index++) {
        naula_memory_release(list[index]);
    }
    naula_memory_release(list);
}

/** The entries as UTF-16; NULL with *out_of_memory set, or with *valid cleared when one is not UTF-8. */
static wchar_t **widen_list(const char *const *list, size_t count, int *out_of_memory, int *valid) {
    wchar_t **wide = naula_memory_allocate((count + 1) * sizeof *wide);
    size_t made = 0;
    *out_of_memory = wide == NULL;
    while (wide != NULL && made < count) {
        wide[made] = widen(list[made], out_of_memory);
        if (wide[made] == NULL) {
            *valid = *out_of_memory;
            release_wide_list(wide, made);
            return NULL;
        }
        made++;
    }
    return wide;
}

static void release_inputs(run_inputs *inputs) {
    naula_memory_release(inputs->line);
    naula_memory_release(inputs->application);
    naula_memory_release(inputs->environment);
    naula_memory_release(inputs->wide_directory);
}

static int build_environment(const char *const *environment, run_inputs *inputs, int *out_of_memory, int *valid) {
    size_t count = list_length(environment);
    wchar_t **additions = widen_list(environment, count, out_of_memory, valid);
    if (additions == NULL) {
        return 0;
    }
    inputs->environment = merged_environment(additions, count);
    release_wide_list(additions, count);
    *out_of_memory = inputs->environment == NULL;
    return inputs->environment != NULL;
}

static naula_status build_inputs(const char *const *arguments, const char *directory, const char *const *environment,
                                 run_inputs *inputs, naula_refusal *refusal) {
    memset(inputs, 0, sizeof *inputs);
    run_buffer text = {NULL, 0, 0};
    int out_of_memory = !build_command_line(arguments, &text);
    int valid = 1;
    inputs->line = out_of_memory ? NULL : widen(text.bytes, &out_of_memory);
    run_buffer_discard(&text);
    wchar_t *name = inputs->line == NULL ? NULL : widen(arguments[0], &out_of_memory);
    inputs->application = name == NULL ? NULL : resolve_program(name, &out_of_memory);
    naula_memory_release(name);
    if (!out_of_memory && name != NULL && inputs->application == NULL) {
        naula_refuse(refusal, "program-not-found", "install it and put it on PATH", "%s was not found", arguments[0]);
        return NAULA_NOT_FOUND;
    }
    if (!out_of_memory && directory != NULL) {
        inputs->wide_directory = widen(directory, &out_of_memory);
    }
    int built = !out_of_memory && build_environment(environment, inputs, &out_of_memory, &valid);
    if (built && (directory == NULL || inputs->wide_directory != NULL)) {
        return NAULA_OK;
    }
    if (!out_of_memory) {
        naula_refuse(refusal, "run-failed", "pass UTF-8 text", "an argument, the directory or an environment entry is not UTF-8");
    }
    return out_of_memory ? NAULA_OUT_OF_MEMORY : NAULA_MALFORMED;
}

naula_status naula_run_limited(const char *const *arguments, const char *directory, const char *const *environment,
                               const char *input, size_t input_length, const naula_run_limits *limits,
                               naula_run_result *result, naula_refusal *refusal) {
    naula_status status = run_begin(arguments, result, refusal);
    if (status == NAULA_OK) {
        status = run_check_input(input, input_length, refusal);
    }
    if (status != NAULA_OK) {
        return status;
    }
    run_inputs inputs;
    status = build_inputs(arguments, directory, environment, &inputs, refusal);
    if (status == NAULA_OK) {
        run_handles handles;
        if (prepare(&handles, input, input_length)) {
            status = run_prepared(&inputs, arguments, directory, &handles, limits, result, refusal);
        } else {
            naula_refuse(refusal, "run-failed", "close some programs and try again", "cannot prepare to run %s",
                         arguments[0]);
            status = NAULA_REFUSED;
        }
        release_handles(&handles);
    }
    release_inputs(&inputs);
    return status;
}
