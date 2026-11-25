#include <string.h>

#include <windows.h>

#include "naula/memory.h"
#include "run_internal.h"

#define POLL_MILLISECONDS 10
#define PIPE_BYTES (1024 * 1024)

typedef struct run_handles {
    HANDLE null_device;
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
    handles->inherited[0] = handles->null_device;
    handles->inherited[1] = handles->output_write;
    handles->inherited[2] = handles->error_write;
    return UpdateProcThreadAttribute(handles->attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles->inherited,
                                     sizeof handles->inherited, NULL, NULL) != 0;
}

static int prepare(run_handles *handles) {
    memset(handles, 0, sizeof *handles);
    handles->null_device = open_null_device();
    if (handles->null_device == INVALID_HANDLE_VALUE) {
        handles->null_device = NULL;
        return 0;
    }
    return open_pipe(&handles->output_read, &handles->output_write) && open_pipe(&handles->error_read, &handles->error_write) &&
           prepare_attributes(handles);
}

static int create(wchar_t *line, const wchar_t *directory, run_handles *handles, PROCESS_INFORMATION *information) {
    STARTUPINFOEXW startup;
    memset(&startup, 0, sizeof startup);
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = handles->null_device;
    startup.StartupInfo.hStdOutput = handles->output_write;
    startup.StartupInfo.hStdError = handles->error_write;
    startup.lpAttributeList = handles->attributes;
    return CreateProcessW(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, directory,
                          &startup.StartupInfo, information) != 0;
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
static int collect(HANDLE process, run_handles *handles, run_buffer *output, run_buffer *error, int *out_of_memory) {
    for (;;) {
        int read_any = 0;
        drain(handles->output_read, output, &read_any, out_of_memory);
        drain(handles->error_read, error, &read_any, out_of_memory);
        if (!read_any && WaitForSingleObject(process, POLL_MILLISECONDS) == WAIT_OBJECT_0) {
            drain(handles->output_read, output, &read_any, out_of_memory);
            drain(handles->error_read, error, &read_any, out_of_memory);
            if (!read_any) {
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

static naula_status run_prepared(wchar_t *line, const wchar_t *wide_directory, const char *const *arguments,
                                 const char *directory, run_handles *handles, naula_run_result *result,
                                 naula_refusal *refusal) {
    PROCESS_INFORMATION information = {NULL, NULL, 0, 0};
    if (!create(line, wide_directory, handles, &information)) {
        return refuse_create_failure(GetLastError(), arguments[0], directory, refusal);
    }
    close_handle(&handles->output_write);
    close_handle(&handles->error_write);
    CloseHandle(information.hThread);
    run_buffer output = {NULL, 0, 0};
    run_buffer error = {NULL, 0, 0};
    int out_of_memory = 0;
    int exit_code = collect(information.hProcess, handles, &output, &error, &out_of_memory);
    CloseHandle(information.hProcess);
    if (out_of_memory) {
        run_buffer_discard(&output);
        run_buffer_discard(&error);
        return NAULA_OUT_OF_MEMORY;
    }
    return run_finish(result, &output, &error, exit_code);
}

static naula_status run_with_command_line(const char *const *arguments, const char *directory, naula_run_result *result,
                                          naula_refusal *refusal) {
    run_buffer text = {NULL, 0, 0};
    if (!build_command_line(arguments, &text)) {
        run_buffer_discard(&text);
        return NAULA_OUT_OF_MEMORY;
    }
    int out_of_memory = 0;
    wchar_t *line = widen(text.bytes, &out_of_memory);
    wchar_t *wide_directory = directory == NULL || out_of_memory ? NULL : widen(directory, &out_of_memory);
    run_buffer_discard(&text);
    naula_status status = NAULA_OUT_OF_MEMORY;
    if (!out_of_memory && line != NULL && (directory == NULL || wide_directory != NULL)) {
        run_handles handles;
        if (prepare(&handles)) {
            status = run_prepared(line, wide_directory, arguments, directory, &handles, result, refusal);
        } else {
            naula_refuse(refusal, "run-failed", "close some programs and try again", "cannot prepare to run %s",
                         arguments[0]);
            status = NAULA_REFUSED;
        }
        release_handles(&handles);
    } else if (!out_of_memory) {
        naula_refuse(refusal, "run-failed", "pass UTF-8 text", "an argument or the directory is not UTF-8");
        status = NAULA_MALFORMED;
    }
    naula_memory_release(line);
    naula_memory_release(wide_directory);
    return status;
}

naula_status naula_run(const char *const *arguments, const char *directory, naula_run_result *result,
                       naula_refusal *refusal) {
    naula_status begun = run_begin(arguments, result, refusal);
    return begun != NAULA_OK ? begun : run_with_command_line(arguments, directory, result, refusal);
}
