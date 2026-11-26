#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * A libcurl stand-in that reports a transfer the write callback stopped as CURLE_RECV_ERROR (56), as the libcurl
 * macOS ships does over HTTP/2, where the libcurl of Linux distributions reports CURLE_WRITE_ERROR (23). A url ending
 * in "/refused" answers 401 with a declared body; any other answers 200 with an undeclared one.
 */

typedef size_t (*data_function)(char *, size_t, size_t, void *);

typedef struct fake_handle {
    char url[512];
    data_function header_function;
    void *header_data;
    data_function body_function;
    void *body_data;
} fake_handle;

typedef struct curl_slist {
    char *data;
    struct curl_slist *next;
} curl_slist;

enum { OPTION_WRITEDATA = 10001, OPTION_URL = 10002, OPTION_HEADERDATA = 10029, OPTION_WRITEFUNCTION = 20011,
       OPTION_HEADERFUNCTION = 20079 };
enum { RECEIVE_ERROR = 56 };

void *curl_easy_init(void) {
    return calloc(1, sizeof(fake_handle));
}

int curl_easy_setopt(void *curl, int option, ...) {
    fake_handle *handle = curl;
    va_list arguments;
    va_start(arguments, option);
    if (option == OPTION_URL) {
        strncpy(handle->url, va_arg(arguments, const char *), sizeof handle->url - 1);
    } else if (option == OPTION_HEADERFUNCTION) {
        handle->header_function = va_arg(arguments, data_function);
    } else if (option == OPTION_HEADERDATA) {
        handle->header_data = va_arg(arguments, void *);
    } else if (option == OPTION_WRITEFUNCTION) {
        handle->body_function = va_arg(arguments, data_function);
    } else if (option == OPTION_WRITEDATA) {
        handle->body_data = va_arg(arguments, void *);
    }
    va_end(arguments);
    return 0;
}

static int send_header(fake_handle *handle, const char *line) {
    size_t length = strlen(line);
    return handle->header_function((char *)line, 1, length, handle->header_data) == length;
}

int curl_easy_perform(void *curl) {
    fake_handle *handle = curl;
    size_t url_length = strlen(handle->url);
    int refused = url_length >= 8 && strcmp(handle->url + url_length - 8, "/refused") == 0;
    char body[] = "nope!";
    int headers_taken = refused ? send_header(handle, "HTTP/2 401 \r\n") && send_header(handle, "content-length: 5\r\n")
                                : send_header(handle, "HTTP/2 200 \r\n");
    if (!headers_taken || !send_header(handle, "\r\n")) {
        return RECEIVE_ERROR;
    }
    return handle->body_function(body, 1, 5, handle->body_data) == 5 ? 0 : RECEIVE_ERROR;
}

typedef enum { CURLVERSION_FIRST = 0 } CURLversion;

typedef struct curl_version_info_data {
    CURLversion age;
    const char *version;
    unsigned int version_num;
} curl_version_info_data;

/** Reports the version NAULA_FAKE_CURL_VERSION names as major.minor.patch, 8.5.0 when it is not set. */
curl_version_info_data *curl_version_info(CURLversion age) {
    static char text[32];
    static curl_version_info_data info;
    const char *named = getenv("NAULA_FAKE_CURL_VERSION");
    unsigned major = 8;
    unsigned minor = 5;
    unsigned patch = 0;
    (void)age;
    if (named != NULL) {
        char *end = NULL;
        major = (unsigned)strtoul(named, &end, 10);
        minor = end != NULL && *end == '.' ? (unsigned)strtoul(end + 1, &end, 10) : 0;
        patch = end != NULL && *end == '.' ? (unsigned)strtoul(end + 1, &end, 10) : 0;
    }
    snprintf(text, sizeof text, "%u.%u.%u", major, minor, patch);
    info.age = CURLVERSION_FIRST;
    info.version = text;
    info.version_num = (major << 16) | (minor << 8) | patch;
    return &info;
}

void curl_easy_cleanup(void *curl) {
    free(curl);
}

const char *curl_easy_strerror(int code) {
    return code == RECEIVE_ERROR ? "Failure when receiving data from the peer" : "No error";
}

curl_slist *curl_slist_append(curl_slist *list, const char *text) {
    curl_slist *item = calloc(1, sizeof *item);
    if (item == NULL) {
        return NULL;
    }
    item->data = strdup(text);
    if (list == NULL) {
        return item;
    }
    curl_slist *last = list;
    while (last->next != NULL) {
        last = last->next;
    }
    last->next = item;
    return list;
}

void curl_slist_free_all(curl_slist *list) {
    while (list != NULL) {
        curl_slist *next = list->next;
        free(list->data);
        free(list);
        list = next;
    }
}
