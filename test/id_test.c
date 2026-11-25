#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "cJSON.h"
#include "naula/id.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#endif

#ifndef NAULA_IDENTITY_VECTORS
#error "NAULA_IDENTITY_VECTORS names the conformance kit's standard/identity folder"
#endif

#define VECTOR_LIMIT 128
#define NAME_LIMIT 256
#define ROOT_COMMIT_HEX "4b825dc642cb6eb9a060e54bf8d69288fbee4904"

typedef struct vector_names {
    char names[VECTOR_LIMIT][NAME_LIMIT];
    size_t count;
} vector_names;

static int is_json_name(const char *name) {
    size_t length = strlen(name);
    return length > 5 && strcmp(name + length - 5, ".json") == 0 && length < NAME_LIMIT;
}

static void add_name(vector_names *found, const char *name) {
    if (is_json_name(name) && found->count < VECTOR_LIMIT) {
        strcpy(found->names[found->count++], name);
    }
}

static void list_vectors(vector_names *found) {
    found->count = 0;
#if defined(_WIN32)
    WIN32_FIND_DATAA entry;
    HANDLE handle = FindFirstFileA(NAULA_IDENTITY_VECTORS "/*.json", &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        add_name(found, entry.cFileName);
    } while (FindNextFileA(handle, &entry));
    FindClose(handle);
#else
    DIR *directory = opendir(NAULA_IDENTITY_VECTORS);
    struct dirent *entry;
    if (directory == NULL) {
        return;
    }
    while ((entry = readdir(directory)) != NULL) {
        add_name(found, entry->d_name);
    }
    closedir(directory);
#endif
}

static char *read_file(const char *name) {
    char path[NAME_LIMIT + 512];
    FILE *file;
    long size;
    char *text;
    snprintf(path, sizeof path, "%s/%s", NAULA_IDENTITY_VECTORS, name);
    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    text = malloc((size_t)size + 1);
    if (text != NULL) {
        text[fread(text, 1, (size_t)size, file)] = '\0';
    }
    fclose(file);
    return text;
}

static const char *string_at(const cJSON *object, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static long refusal_offset(const naula_refusal *refusal) {
    const char *marker = strstr(refusal->detail, "offset ");
    return marker == NULL ? -1 : strtol(marker + 7, NULL, 10);
}

static int sign_of(int value) {
    return value < 0 ? -1 : (value > 0 ? 1 : 0);
}

static void expect_order(const char *name, const char *left_text, const char *right_text, int expected) {
    naula_id left;
    naula_id right;
    naula_refusal refusal;
    int parsed = naula_id_parse(left_text, strlen(left_text), &left, &refusal) == NAULA_OK;
    int parsed_other = naula_id_parse(right_text, strlen(right_text), &right, &refusal) == NAULA_OK;
    CHECK(parsed && parsed_other);
    if (parsed && parsed_other) {
        int actual = naula_id_compare(&left, &right);
        if (sign_of(actual) != expected) {
            fprintf(stderr, "%s: expected order %d, got %d\n", name, expected, actual);
            check_failures++;
        }
        CHECK(sign_of(naula_id_compare(&right, &left)) == -expected);
    }
    naula_id_release(&left);
    naula_id_release(&right);
}

static void expect_parse(const char *name, const char *text, const cJSON *then) {
    naula_id id;
    naula_refusal refusal;
    naula_status status;
    int valid = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(then, "valid"));
    memset(&refusal, 0, sizeof refusal);
    status = naula_id_parse(text, strlen(text), &id, &refusal);
    if (valid) {
        if (status != NAULA_OK) {
            fprintf(stderr, "%s: refused: %s\n", name, refusal.detail);
        }
        CHECK(status == NAULA_OK);
    } else {
        const cJSON *offset = cJSON_GetObjectItemCaseSensitive(then, "offset");
        CHECK(status == NAULA_REFUSED);
        CHECK(strcmp(refusal.code, "invalid_id") == 0);
        CHECK(refusal.fix != NULL && refusal.fix[0] != '\0');
        if (cJSON_IsNumber(offset) && refusal_offset(&refusal) != (long)offset->valuedouble) {
            fprintf(stderr, "%s: expected offset %d, detail \"%s\"\n", name, (int)offset->valuedouble,
                    refusal.detail);
            check_failures++;
        }
        CHECK(id.kind == 0 && id.local_name == NULL && id.segments == NULL);
    }
    naula_id_release(&id);
}

static void run_vector(const char *file_name) {
    char *text = read_file(file_name);
    cJSON *vector = text == NULL ? NULL : cJSON_Parse(text);
    const cJSON *given = vector == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(vector, "given");
    const cJSON *then = vector == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(vector, "then");
    const char *value = given == NULL ? NULL : string_at(given, "value");
    const char *other = given == NULL ? NULL : string_at(given, "other");
    const cJSON *order = then == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(then, "order");
    CHECK(value != NULL && then != NULL);
    if (value != NULL && then != NULL) {
        if (other != NULL && cJSON_IsNumber(order)) {
            expect_order(file_name, value, other, (int)order->valuedouble);
        } else {
            expect_parse(file_name, value, then);
        }
    }
    cJSON_Delete(vector);
    free(text);
}

static void every_kit_vector_holds(void) {
    vector_names found;
    size_t at;
    list_vectors(&found);
    CHECK(found.count >= 17);
    for (at = 0; at < found.count; at++) {
        run_vector(found.names[at]);
    }
}

static void folder_is_the_first_twelve_hex(void) {
    naula_id id;
    char folder[13];
    char text[] = "git:" ROOT_COMMIT_HEX "/kernel";
    CHECK(naula_id_parse(text, strlen(text), &id, NULL) == NAULA_OK);
    naula_id_folder(&id, folder);
    check_text(folder, "4b825dc642cb", "git folder");
    CHECK(id.segment_count == 1);
    check_text(id.segments[0], "kernel", "segment");
    naula_id_release(&id);
    CHECK(naula_id_parse("local:stub", 10, &id, NULL) == NAULA_OK);
    naula_id_folder(&id, folder);
    check_text(folder, "", "local folder");
    check_text(id.local_name, "stub", "local name");
    naula_id_release(&id);
}

static void length_bounds_the_text_read(void) {
    naula_id id;
    CHECK(naula_id_parse("local:stubby", 10, &id, NULL) == NAULA_OK);
    check_text(id.local_name, "stub", "length-bounded name");
    naula_id_release(&id);
}

int main(void) {
    every_kit_vector_holds();
    folder_is_the_first_twelve_hex();
    length_bounds_the_text_read();
    return check_summary("id_test");
}
