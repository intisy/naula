#include "naula/memory.h"
#include "naula/status.h"
#include "naula/transport.h"
#include "transport_internal.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>
#include <shlwapi.h>
#include <winhttp.h>

#include "transport_hooks.h"

#define READ_CHUNK 65536
#define CONNECT_MILLISECONDS 15000u
#define MAXIMUM_REDIRECTS 5
#define HEADER_TEXT_LIMIT 2048
#define URL_LIMIT 4096
#define METHOD_CAPACITY 17
#define BODY_LIMIT 0x7FFFFFFFu

/**
 * @implNote The automatic access type (Windows 8.1 and later) takes the proxy the user's system settings
 * name, a PAC script or WPAD included, which is what lets a machine behind a corporate proxy fetch.
 */
static const DWORD session_access_type = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;

#ifdef NAULA_TEST_HOOKS
unsigned long naula_fetch_windows_access_type(void) {
    return session_access_type;
}
#endif

typedef struct winhttp_handles {
    HINTERNET session;
    HINTERNET connection;
    HINTERNET request;
} winhttp_handles;

typedef struct url_parts {
    wchar_t *wide_url;
    wchar_t *host;
    wchar_t *path;
    INTERNET_PORT port;
    int secure;
} url_parts;

/** text as UTF-16, freed; NULL when it is not UTF-8, or with *out_of_memory set to 1 when memory ran out. */
static wchar_t *to_wide_reporting(const char *text, int *out_of_memory) {
    *out_of_memory = 0;
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (length <= 0) {
        return NULL;
    }
    wchar_t *wide = naula_memory_allocate((size_t)length * sizeof(wchar_t));
    *out_of_memory = wide == NULL;
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length) <= 0) {
        naula_memory_release(wide);
        return NULL;
    }
    return wide;
}

static char *to_utf8(const wchar_t *wide) {
    int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (length <= 0) {
        return NULL;
    }
    char *narrow = naula_memory_allocate((size_t)length);
    if (narrow != NULL && WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, length, NULL, NULL) <= 0) {
        naula_memory_release(narrow);
        return NULL;
    }
    return narrow;
}

static wchar_t *copy_wide(const wchar_t *text, size_t length) {
    wchar_t *copy = naula_memory_allocate((length + 1) * sizeof(wchar_t));
    if (copy != NULL) {
        wmemcpy(copy, text, length);
        copy[length] = 0;
    }
    return copy;
}

static void free_parts(url_parts *parts) {
    naula_memory_release(parts->wide_url);
    naula_memory_release(parts->host);
    naula_memory_release(parts->path);
}

static size_t without_fragment(const wchar_t *path, size_t length) {
    for (size_t index = 0; index < length; index++) {
        if (path[index] == L'#') {
            return index;
        }
    }
    return length;
}

/**
 * Splits wide_url, which parts then owns: NAULA_MALFORMED when it is not an https url, or an http one plain http
 * allows, NAULA_OUT_OF_MEMORY when memory runs out.
 */
static naula_status split_url(int allow_plain_http, wchar_t *wide_url, url_parts *parts) {
    memset(parts, 0, sizeof *parts);
    parts->wide_url = wide_url;
    URL_COMPONENTS components;
    memset(&components, 0, sizeof components);
    components.dwStructSize = sizeof components;
    components.dwHostNameLength = (DWORD)-1;
    components.dwUrlPathLength = (DWORD)-1;
    components.dwExtraInfoLength = (DWORD)-1;
    if (parts->wide_url == NULL || !WinHttpCrackUrl(parts->wide_url, 0, 0, &components)) {
        return NAULA_MALFORMED;
    }
    int allowed = components.nScheme == INTERNET_SCHEME_HTTPS ||
                  (allow_plain_http && components.nScheme == INTERNET_SCHEME_HTTP);
    if (!allowed || components.dwHostNameLength == 0) {
        return NAULA_MALFORMED;
    }
    size_t path_length = without_fragment(components.lpszUrlPath,
                                          (size_t)components.dwUrlPathLength + components.dwExtraInfoLength);
    parts->host = copy_wide(components.lpszHostName, components.dwHostNameLength);
    parts->path = path_length == 0 ? copy_wide(L"/", 1) : copy_wide(components.lpszUrlPath, path_length);
    parts->port = components.nPort;
    parts->secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return parts->host != NULL && parts->path != NULL ? NAULA_OK : NAULA_OUT_OF_MEMORY;
}

static void close_handles(winhttp_handles *handles) {
    if (handles->request != NULL) {
        WinHttpCloseHandle(handles->request);
    }
    if (handles->connection != NULL) {
        WinHttpCloseHandle(handles->connection);
    }
    if (handles->session != NULL) {
        WinHttpCloseHandle(handles->session);
    }
}

static int set_timeouts(HINTERNET session, unsigned deadline) {
    int connect = (int)(deadline < CONNECT_MILLISECONDS ? deadline : CONNECT_MILLISECONDS);
    return WinHttpSetTimeouts(session, (int)deadline, connect, (int)deadline, (int)deadline) != 0;
}

/**
 * @implNote Redirects are followed by hand because WinHTTP answers a redirect loop and a redirect it will not follow with the one error, and every hop must be checked to be
 * https before it is taken.
 */
static int disable_redirects(HINTERNET request) {
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    return WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy) != 0;
}

/** The method as UTF-16 in method, which holds the 16 capital letters a request may name and a terminator. */
static void wide_method(const naula_fetch_request *request, wchar_t *method, size_t capacity) {
    const char *narrow = naula_fetch_request_method(request);
    size_t index = 0;
    for (; narrow[index] != 0 && index + 1 < capacity; index++) {
        method[index] = (wchar_t)narrow[index];
    }
    method[index] = 0;
}

static int open_handles(const naula_fetch_request *request, const url_parts *parts, winhttp_handles *handles) {
    wchar_t method[METHOD_CAPACITY];
    wide_method(request, method, METHOD_CAPACITY);
    memset(handles, 0, sizeof *handles);
    handles->session = WinHttpOpen(L"naula", session_access_type, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
    if (handles->session == NULL || !set_timeouts(handles->session, request->deadline_milliseconds)) {
        return 0;
    }
    handles->connection = WinHttpConnect(handles->session, parts->host, parts->port, 0);
    if (handles->connection == NULL) {
        return 0;
    }
    handles->request = WinHttpOpenRequest(handles->connection, method, parts->path, NULL, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, parts->secure ? WINHTTP_FLAG_SECURE : 0);
    return handles->request != NULL && disable_redirects(handles->request);
}

static wchar_t *request_headers(const naula_fetch_request *request, int is_authorized) {
    const char *authorization = is_authorized ? request->authorization : NULL;
    const char *const *extra = naula_fetch_request_extra_headers(request);
    const char *content_type = naula_fetch_request_body(request).content_type;
    size_t size = 128 + (request->etag == NULL ? 0 : strlen(request->etag)) +
                  (request->last_modified == NULL ? 0 : strlen(request->last_modified)) +
                  (authorization == NULL ? 0 : strlen(authorization)) +
                  (content_type == NULL ? 0 : strlen(content_type));
    for (size_t index = 0; extra != NULL && extra[index] != NULL; index++) {
        size += strlen(extra[index]) + 2;
    }
    char *headers = naula_memory_allocate(size);
    if (headers == NULL) {
        return NULL;
    }
    int length = 0;
    headers[0] = 0;
    if (request->etag != NULL) {
        length += snprintf(headers + length, size - (size_t)length, "If-None-Match: %s\r\n", request->etag);
    }
    if (request->last_modified != NULL) {
        length += snprintf(headers + length, size - (size_t)length, "If-Modified-Since: %s\r\n", request->last_modified);
    }
    if (authorization != NULL) {
        length += snprintf(headers + length, size - (size_t)length, "Authorization: %s\r\n", authorization);
    }
    if (content_type != NULL) {
        length += snprintf(headers + length, size - (size_t)length, "Content-Type: %s\r\n", content_type);
    }
    for (size_t index = 0; extra != NULL && extra[index] != NULL; index++) {
        length += snprintf(headers + length, size - (size_t)length, "%s\r\n", extra[index]);
    }
    int out_of_memory;
    wchar_t *wide = to_wide_reporting(headers, &out_of_memory);
    naula_overwrite(headers, size);
    naula_memory_release(headers);
    return wide;
}

/** Sends the request and reads its answer's head: 1 when sent, 0 when it failed, -1 when memory ran out first. */
static int send_request(const naula_fetch_request *request, HINTERNET handle, int is_authorized) {
    naula_fetch_body body = naula_fetch_request_body(request);
    if (body.length > BODY_LIMIT) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    wchar_t *headers = request_headers(request, is_authorized);
    if (headers == NULL) {
        return -1;
    }
    LPVOID data = body.length == 0 ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.bytes;
    int sent = WinHttpSendRequest(handle, headers[0] == 0 ? WINHTTP_NO_ADDITIONAL_HEADERS : headers,
                                  headers[0] == 0 ? 0 : (DWORD)-1L, data, (DWORD)body.length, (DWORD)body.length,
                                  0) &&
               WinHttpReceiveResponse(handle, NULL);
    naula_overwrite(headers, wcslen(headers) * sizeof *headers);
    naula_memory_release(headers);
    return sent;
}

/** The header's text, NULL when the answer carries none; *out_of_memory is set when it does but cannot be copied. */
static char *header_text(HINTERNET handle, DWORD query, int *out_of_memory) {
    wchar_t value[HEADER_TEXT_LIMIT];
    DWORD size = sizeof value;
    if (!WinHttpQueryHeaders(handle, query, WINHTTP_HEADER_NAME_BY_INDEX, value, &size, WINHTTP_NO_HEADER_INDEX)) {
        return NULL;
    }
    char *text = to_utf8(value);
    *out_of_memory = *out_of_memory || text == NULL;
    return text;
}

static long status_of(HINTERNET handle) {
    DWORD status = 0;
    DWORD size = sizeof status;
    int known = WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    return known ? (long)status : 0;
}

static int declared_length(HINTERNET handle, uint64_t *declared, int *out_of_memory) {
    char *text = header_text(handle, WINHTTP_QUERY_CONTENT_LENGTH, out_of_memory);
    int known = text != NULL && text[0] != 0 && strspn(text, "0123456789") == strlen(text) && strlen(text) < 20;
    if (known) {
        *declared = strtoull(text, NULL, 10);
    }
    naula_memory_release(text);
    return known;
}

static void refuse_error(const naula_fetch_request *request, DWORD error, naula_fetch_answer *answer) {
    if (error == ERROR_WINHTTP_TIMEOUT) {
        naula_fetch_deadline_passed(answer, request);
    } else {
        naula_fetch_refuse(answer, NAULA_FETCH_NETWORK_FIX, "%s could not be fetched: WinHTTP error %lu",
                             request->url, (unsigned long)error);
        if (error == ERROR_WINHTTP_NAME_NOT_RESOLVED) {
            naula_fetch_mark_host_unresolved(answer);
        }
    }
}

static int is_past(ULONGLONG deadline) {
    return GetTickCount64() > deadline;
}

typedef struct body_reading {
    int declared_known;
    uint64_t declared;
    ULONGLONG deadline;
} body_reading;

static int is_redirect(long status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/** The url a redirect names, NULL when it names none it can be resolved to, with *out_of_memory set if memory ran out. */
static wchar_t *redirect_target(HINTERNET handle, const wchar_t *current, int *out_of_memory) {
    wchar_t location[URL_LIMIT];
    DWORD size = sizeof location;
    if (!WinHttpQueryHeaders(handle, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, location, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return NULL;
    }
    wchar_t *target = naula_memory_allocate(URL_LIMIT * sizeof(wchar_t));
    *out_of_memory = target == NULL;
    DWORD length = URL_LIMIT;
    if (target != NULL && UrlCombineW(current, location, target, &length, 0) != S_OK) {
        naula_memory_release(target);
        return NULL;
    }
    return target;
}

typedef enum hop_outcome {
    HOP_ANSWERED,
    HOP_REDIRECTED
} hop_outcome;

/**
 * Closes a request handle once the deadline passes, which is how WinHTTP cancels a synchronous call: its own timeouts
 * bound each send and receive alone, so an answer that trickles in could otherwise run far past the deadline.
 *
 * @implNote `lock` guards `request` and `fired`. Every call that does not block (the header queries) runs holding it,
 * so the callback never closes the handle under one and none reads a value already closed. The blocking calls (send,
 * receive, read) cannot hold it, since closing the handle under them is what cancels them: each takes the value under
 * the lock just before the call. The one window no test can reach is the callback closing the handle between that
 * read and the call, where WinHTTP may be handed a closed value another thread has meanwhile been given again.
 */
typedef struct deadline_watch {
    PTP_TIMER timer;
    SRWLOCK lock;
    HINTERNET request;
    int fired;
} deadline_watch;

static void close_now(deadline_watch *watch) {
    AcquireSRWLockExclusive(&watch->lock);
    if (!watch->fired) {
        watch->fired = 1;
        WinHttpCloseHandle(watch->request);
        watch->request = NULL;
    }
    ReleaseSRWLockExclusive(&watch->lock);
}

static VOID CALLBACK close_at_deadline(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_TIMER timer) {
    (void)instance;
    (void)timer;
    close_now(context);
}

#ifdef NAULA_TEST_HOOKS
static int test_close_point;

void naula_fetch_windows_close_at(int point) {
    test_close_point = point;
}

static void close_at_test_point(deadline_watch *watch, int point) {
    if (test_close_point == point) {
        close_now(watch);
    }
}
#else
static void close_at_test_point(deadline_watch *watch, int point) {
    (void)watch;
    (void)point;
}
#endif

/** The request handle with the lock held, NULL once the deadline closed it; release_request ends the hold. */
static HINTERNET hold_request(deadline_watch *watch) {
    AcquireSRWLockShared(&watch->lock);
    return watch->request;
}

static void release_request(deadline_watch *watch) {
    ReleaseSRWLockShared(&watch->lock);
}

/** The handle a blocking call is made on, NULL once the deadline closed it. */
static HINTERNET blocking_request(deadline_watch *watch) {
    HINTERNET handle = hold_request(watch);
    release_request(watch);
    return handle;
}

static int start_watch(deadline_watch *watch, HINTERNET request, ULONGLONG deadline) {
    InitializeSRWLock(&watch->lock);
    watch->request = request;
    watch->fired = 0;
    watch->timer = CreateThreadpoolTimer(close_at_deadline, watch, NULL);
    if (watch->timer == NULL) {
        return 0;
    }
    ULONGLONG now = GetTickCount64();
    ULARGE_INTEGER relative;
    relative.QuadPart = (ULONGLONG)(-(LONGLONG)((deadline > now ? deadline - now : 0) * 10000u));
    FILETIME due;
    due.dwLowDateTime = relative.LowPart;
    due.dwHighDateTime = relative.HighPart;
    SetThreadpoolTimer(watch->timer, &due, 0, 0);
    return 1;
}

/** Disarms the timer and waits out a callback in flight, so nothing runs on the watch after; 1 if it fired. */
static int stop_watch(deadline_watch *watch) {
    SetThreadpoolTimer(watch->timer, NULL, 0, 0);
    WaitForThreadpoolTimerCallbacks(watch->timer, TRUE);
    CloseThreadpoolTimer(watch->timer);
    return watch->fired;
}

/** 1 when the body was read to its end or stopped by the sink or memory, 0 when a call failed or the time ran out. */
static int read_body(const naula_fetch_request *request, deadline_watch *watch, const body_reading *reading,
                     naula_fetch_sink *sink, naula_fetch_answer *answer) {
    uint8_t *chunk = naula_memory_allocate(READ_CHUNK);
    if (chunk == NULL) {
        naula_fetch_out_of_memory(answer);
        return 1;
    }
    DWORD read = 1;
    int failed = 0;
    int closed = 0;
    while (!failed && read > 0 && !is_past(reading->deadline)) {
        HINTERNET handle = blocking_request(watch);
        closed = handle == NULL;
        failed = closed || !WinHttpReadData(handle, chunk, READ_CHUNK, &read);
        if (!failed && read > 0 && !naula_fetch_sink_take(sink, chunk, read)) {
            naula_memory_release(chunk);
            answer->outcome = NAULA_FETCH_STOPPED_BY_SINK;
            return 1;
        }
    }
    DWORD error = closed ? ERROR_WINHTTP_OPERATION_CANCELLED
                         : (failed ? GetLastError() : (read > 0 ? ERROR_WINHTTP_TIMEOUT : ERROR_SUCCESS));
    naula_memory_release(chunk);
    int cancelled = error == ERROR_WINHTTP_OPERATION_CANCELLED || error == ERROR_INVALID_HANDLE;
    if (reading->declared_known && sink->bytes < reading->declared && error != ERROR_WINHTTP_TIMEOUT && !cancelled) {
        answer->outcome = NAULA_FETCH_CUT_OFF;
        answer->declared = reading->declared;
        return 1;
    }
    if (error != ERROR_SUCCESS) {
        refuse_error(request, error, answer);
        return 0;
    }
    answer->outcome = NAULA_FETCH_RECEIVED;
    return 1;
}

typedef struct response_head {
    long status;
    char *etag;
    char *last_modified;
    body_reading reading;
    wchar_t *next;
    int out_of_memory;
} response_head;

/** Reads the head holding the lock; 0 when the deadline closed the handle first. */
static int read_head(deadline_watch *watch, const wchar_t *current, int follows, response_head *head) {
    HINTERNET handle = hold_request(watch);
    if (handle != NULL) {
        head->status = status_of(handle);
        if (is_redirect(head->status) && follows) {
            head->next = redirect_target(handle, current, &head->out_of_memory);
        } else {
            head->etag = header_text(handle, WINHTTP_QUERY_ETAG, &head->out_of_memory);
            head->last_modified = header_text(handle, WINHTTP_QUERY_LAST_MODIFIED, &head->out_of_memory);
            head->reading.declared_known =
                declared_length(handle, &head->reading.declared, &head->out_of_memory);
        }
    }
    release_request(watch);
    return handle != NULL;
}

/** Settles an answer from its head and body; 1 when the answer is the host's own, 0 when the hop has not finished. */
static int take_response(const naula_fetch_request *request, deadline_watch *watch, response_head *head,
                         naula_fetch_sink *sink, naula_fetch_answer *answer) {
    answer->status = head->status;
    answer->etag = head->etag;
    answer->last_modified = head->last_modified;
    if (head->out_of_memory) {
        naula_fetch_out_of_memory(answer);
        return 1;
    }
    if (answer->status == 304) {
        answer->outcome = NAULA_FETCH_NOT_MODIFIED;
        return 1;
    }
    if (answer->status < 200 || answer->status > 299) {
        answer->outcome = NAULA_FETCH_STATUS;
        return 1;
    }
    if (head->reading.declared_known && !naula_fetch_sink_admits(sink, head->reading.declared)) {
        answer->outcome = NAULA_FETCH_STOPPED_BY_SINK;
        return 1;
    }
    return read_body(request, watch, &head->reading, sink, answer);
}

/** One request and its answer; 1 when the answer is complete, 0 when a call failed or the hop redirected. */
static int exchange(const naula_fetch_request *request, const url_parts *parts, deadline_watch *watch,
                    int is_authorized, ULONGLONG deadline, naula_fetch_sink *sink, naula_fetch_answer *answer,
                    wchar_t **next, hop_outcome *outcome) {
    HINTERNET handle = blocking_request(watch);
    int sent = handle == NULL ? 0 : send_request(request, handle, is_authorized);
    if (sent < 0) {
        naula_fetch_out_of_memory(answer);
        return 1;
    }
    if (sent == 0) {
        refuse_error(request, handle == NULL ? ERROR_WINHTTP_OPERATION_CANCELLED : GetLastError(), answer);
        return 0;
    }
    close_at_test_point(watch, NAULA_FETCH_CLOSE_AFTER_RECEIVE);
    int follows = naula_fetch_request_follows_redirects(request);
    response_head head;
    memset(&head, 0, sizeof head);
    head.reading.deadline = deadline;
    if (!read_head(watch, parts->wide_url, follows, &head)) {
        refuse_error(request, ERROR_WINHTTP_OPERATION_CANCELLED, answer);
        return 0;
    }
    if (is_redirect(head.status) && follows) {
        if (head.out_of_memory) {
            naula_fetch_out_of_memory(answer);
            return 1;
        }
        *next = head.next;
        *outcome = HOP_REDIRECTED;
        return 0;
    }
    return take_response(request, watch, &head, sink, answer);
}

static hop_outcome take_hop(const naula_fetch_request *request, const url_parts *parts, int is_authorized,
                            ULONGLONG deadline, naula_fetch_sink *sink, naula_fetch_answer *answer,
                            wchar_t **next) {
    winhttp_handles handles;
    deadline_watch watch;
    hop_outcome outcome = HOP_ANSWERED;
    if (!open_handles(request, parts, &handles)) {
        refuse_error(request, GetLastError(), answer);
    } else if (!start_watch(&watch, handles.request, deadline)) {
        naula_fetch_out_of_memory(answer);
    } else {
        int completed = exchange(request, parts, &watch, is_authorized, deadline, sink, answer, next, &outcome);
        close_at_test_point(&watch, NAULA_FETCH_CLOSE_AFTER_EXCHANGE);
        if (stop_watch(&watch) && !completed) {
            naula_memory_release(*next);
            *next = NULL;
            outcome = HOP_ANSWERED;
            naula_fetch_deadline_passed(answer, request);
        }
        handles.request = watch.request;
    }
    close_handles(&handles);
    return outcome;
}

static void refuse_redirect(const naula_fetch_request *request, naula_fetch_answer *answer) {
    naula_fetch_refuse(answer, NAULA_FETCH_PUBLISHED_FIX, "%s redirected to a url it may not follow", request->url);
}

typedef struct origin {
    wchar_t *host;
    INTERNET_PORT port;
} origin;

/** 0 when memory ran out, which leaves no origin to send the authorization to. */
static int remember_origin(origin *known, const url_parts *parts) {
    known->host = copy_wide(parts->host, wcslen(parts->host));
    known->port = parts->port;
    return known->host != NULL;
}

static int is_origin(const origin *known, const url_parts *parts) {
    return known->host != NULL && known->port == parts->port && _wcsicmp(known->host, parts->host) == 0;
}

static void take_hops(const naula_fetch_request *request, origin *first, naula_fetch_sink *sink,
                      naula_fetch_answer *answer) {
    ULONGLONG deadline = GetTickCount64() + request->deadline_milliseconds;
    int out_of_memory;
    wchar_t *current = to_wide_reporting(request->url, &out_of_memory);
    if (out_of_memory) {
        naula_fetch_out_of_memory(answer);
        return;
    }
    for (int hop = 0; hop <= MAXIMUM_REDIRECTS; hop++) {
        url_parts parts;
        naula_status split = split_url(naula_fetch_plain_http_is_allowed(), current, &parts);
        if (split == NAULA_OUT_OF_MEMORY) {
            free_parts(&parts);
            naula_fetch_out_of_memory(answer);
            return;
        }
        if (split != NAULA_OK) {
            free_parts(&parts);
            if (hop == 0) {
                naula_fetch_refuse(answer, "name the file by a complete https url", "%s is not a usable https url",
                                     request->url);
            } else {
                refuse_redirect(request, answer);
            }
            return;
        }
        current = NULL;
        if (hop == 0 && !remember_origin(first, &parts)) {
            free_parts(&parts);
            naula_fetch_out_of_memory(answer);
            return;
        }
        hop_outcome outcome = take_hop(request, &parts, is_origin(first, &parts), deadline, sink, answer, &current);
        free_parts(&parts);
        if (outcome == HOP_ANSWERED) {
            return;
        }
        if (current == NULL) {
            refuse_redirect(request, answer);
            return;
        }
    }
    naula_memory_release(current);
    naula_fetch_refuse(answer, NAULA_FETCH_PUBLISHED_FIX, "%s redirected more than %d times", request->url,
                         MAXIMUM_REDIRECTS);
}

void naula_fetch_over_network(const naula_fetch_request *request, naula_fetch_sink *sink,
                                naula_fetch_answer *answer) {
    if (!naula_fetch_request_is_usable(request, answer)) {
        return;
    }
    origin first = {NULL, 0};
    take_hops(request, &first, sink, answer);
    naula_memory_release(first.host);
}
