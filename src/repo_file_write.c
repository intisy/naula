#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "naula/memory.h"
#include "repo_file_internal.h"
#include "repo_file_rules.h"

#define INITIAL_CAPACITY 1024u

typedef struct text_builder {
    char *text;
    size_t length;
    size_t capacity;
    int out_of_memory;
} text_builder;

/** A JSON object or array being written: its entries sit one level deeper than `level`. */
typedef struct container {
    text_builder *builder;
    int level;
    size_t entries;
    char close;
} container;

static void append(text_builder *builder, const char *text, size_t length) {
    if (builder->out_of_memory) {
        return;
    }
    if (builder->length + length + 1 > builder->capacity) {
        size_t capacity = builder->capacity == 0 ? INITIAL_CAPACITY : builder->capacity;
        while (builder->length + length + 1 > capacity) {
            capacity *= 2;
        }
        char *grown = naula_memory_reallocate(builder->text, capacity);
        if (grown == NULL) {
            builder->out_of_memory = 1;
            return;
        }
        builder->text = grown;
        builder->capacity = capacity;
    }
    memcpy(builder->text + builder->length, text, length);
    builder->length += length;
    builder->text[builder->length] = '\0';
}

static void append_text(text_builder *builder, const char *text) {
    append(builder, text, strlen(text));
}

static void append_indent(text_builder *builder, int level) {
    for (int step = 0; step < level; step++) {
        append(builder, "  ", 2);
    }
}

static void append_escaped_character(text_builder *builder, unsigned char character) {
    char escaped[8];
    switch (character) {
    case '"': append_text(builder, "\\\""); return;
    case '\\': append_text(builder, "\\\\"); return;
    case '\b': append_text(builder, "\\b"); return;
    case '\f': append_text(builder, "\\f"); return;
    case '\n': append_text(builder, "\\n"); return;
    case '\r': append_text(builder, "\\r"); return;
    case '\t': append_text(builder, "\\t"); return;
    default:
        snprintf(escaped, sizeof escaped, "\\u%04x", (unsigned)character);
        append_text(builder, escaped);
    }
}

static void append_string(text_builder *builder, const char *text) {
    append(builder, "\"", 1);
    const char *run = text;
    for (const char *character = text; *character != '\0'; character++) {
        unsigned char byte = (unsigned char)*character;
        if (byte < 0x20 || byte == '"' || byte == '\\') {
            append(builder, run, (size_t)(character - run));
            append_escaped_character(builder, byte);
            run = character + 1;
        }
    }
    append_text(builder, run);
    append(builder, "\"", 1);
}

/** As cJSON prints a number: the shortest of 15 or 17 digits that reads back as the same double. */
static void append_number(text_builder *builder, double value) {
    char digits[32];
    snprintf(digits, sizeof digits, "%1.15g", value);
    if (strtod(digits, NULL) != value) {
        snprintf(digits, sizeof digits, "%1.17g", value);
    }
    append_text(builder, digits);
}

static container open_container(text_builder *builder, int level, char open) {
    container opened = {builder, level, 0, open == '{' ? '}' : ']'};
    append(builder, &open, 1);
    return opened;
}

static void next_entry(container *into) {
    append_text(into->builder, into->entries == 0 ? "\n" : ",\n");
    append_indent(into->builder, into->level + 1);
    into->entries++;
}

static void next_key(container *into, const char *key) {
    next_entry(into);
    append_string(into->builder, key);
    append_text(into->builder, ": ");
}

static void close_container(container *into) {
    if (into->entries > 0) {
        append(into->builder, "\n", 1);
        append_indent(into->builder, into->level);
    }
    append(into->builder, &into->close, 1);
}

static void write_text_member(container *into, const char *key, const char *text) {
    if (text != NULL) {
        next_key(into, key);
        append_string(into->builder, text);
    }
}

static void write_value(text_builder *builder, int level, const cJSON *value);

static void write_children(text_builder *builder, int level, const cJSON *value, char open) {
    container children = open_container(builder, level, open);
    for (const cJSON *child = value->child; child != NULL; child = child->next) {
        if (open == '{') {
            next_key(&children, child->string);
        } else {
            next_entry(&children);
        }
        write_value(builder, level + 1, child);
    }
    close_container(&children);
}

static void write_value(text_builder *builder, int level, const cJSON *value) {
    if (cJSON_IsObject(value)) {
        write_children(builder, level, value, '{');
    } else if (cJSON_IsArray(value)) {
        write_children(builder, level, value, '[');
    } else if (cJSON_IsString(value)) {
        append_string(builder, value->valuestring);
    } else if (cJSON_IsNumber(value)) {
        append_number(builder, value->valuedouble);
    } else {
        append_text(builder, cJSON_IsTrue(value) ? "true" : cJSON_IsFalse(value) ? "false" : "null");
    }
}

static void write_text_list(container *into, const char *key, char *const *texts, size_t count) {
    next_key(into, key);
    container list = open_container(into->builder, into->level + 1, '[');
    for (size_t index = 0; index < count; index++) {
        next_entry(&list);
        append_string(into->builder, texts[index] == NULL ? "" : texts[index]);
    }
    close_container(&list);
}

static void write_home(text_builder *builder, int level, const naula_home *home) {
    container object = open_container(builder, level, '{');
    write_text_member(&object, REPO_FILE_HOME_PROVIDER, home->provider);
    write_text_member(&object, REPO_FILE_HOME_HOST, home->host);
    write_text_member(&object, REPO_FILE_HOME_ACCOUNT, home->account);
    write_text_member(&object, REPO_FILE_HOME_DOMAIN, home->domain);
    write_text_member(&object, REPO_FILE_HOME_URL, home->url);
    close_container(&object);
}

static void write_homes(container *into, const char *key, const naula_home *homes, size_t count) {
    if (count == 0) {
        return;
    }
    next_key(into, key);
    container list = open_container(into->builder, into->level + 1, '[');
    for (size_t index = 0; index < count; index++) {
        next_entry(&list);
        write_home(into->builder, into->level + 2, &homes[index]);
    }
    close_container(&list);
}

static void place_text(const naula_place *place, char *text, size_t capacity) {
    if (place->kind == NAULA_HOME_WEB) {
        snprintf(text, capacity, "%s:%s", REPO_FILE_WEB_PROVIDER, place->domain == NULL ? "" : place->domain);
    } else if (place->kind == NAULA_HOME_URL) {
        snprintf(text, capacity, "%s", place->url == NULL ? "" : place->url);
    } else if (place->host != NULL) {
        snprintf(text, capacity, "%s@%s:%" PRIu64, place->provider, place->host, place->number);
    } else {
        snprintf(text, capacity, "%s:%" PRIu64, place->provider == NULL ? "" : place->provider, place->number);
    }
}

static void write_reference_members(container *into, const naula_reference *reference) {
    write_text_member(into, "id", reference->id);
    write_text_member(into, "url", reference->url);
    if (reference->at_count == 0) {
        return;
    }
    next_key(into, "at");
    container list = open_container(into->builder, into->level + 1, '[');
    for (size_t index = 0; index < reference->at_count; index++) {
        char text[2400];
        place_text(&reference->at[index], text, sizeof text);
        next_entry(&list);
        append_string(into->builder, text);
    }
    close_container(&list);
}

static void write_reference(text_builder *builder, int level, const naula_reference *reference) {
    container object = open_container(builder, level, '{');
    write_reference_members(&object, reference);
    close_container(&object);
}

static void write_settings(container *into, const cJSON *settings) {
    static const char *const order[] = {"visibility", "defaultBranch", "issues", "wiki"};
    if (settings == NULL) {
        return;
    }
    next_key(into, "settings");
    container object = open_container(into->builder, into->level + 1, '{');
    for (size_t index = 0; index < sizeof order / sizeof order[0]; index++) {
        const cJSON *setting = cJSON_GetObjectItemCaseSensitive(settings, order[index]);
        if (setting != NULL) {
            next_key(&object, order[index]);
            write_value(into->builder, into->level + 2, setting);
        }
    }
    close_container(&object);
}

static void write_metadata(container *into, const naula_metadata *metadata) {
    if ((metadata->present & NAULA_METADATA_DESCRIPTION) != 0) {
        write_text_member(into, "description", metadata->description);
    }
    if ((metadata->present & NAULA_METADATA_TOPICS) != 0) {
        write_text_list(into, "topics", metadata->topics, metadata->topic_count);
    }
    if ((metadata->present & NAULA_METADATA_HOMEPAGE) != 0) {
        write_text_member(into, "homepage", metadata->homepage);
    }
    if ((metadata->present & NAULA_METADATA_SETTINGS) != 0) {
        write_settings(into, metadata->settings);
    }
}

static void write_registries(container *into, const naula_repo_file *file) {
    if (file->registry_npm == NULL && file->registry_maven == NULL) {
        return;
    }
    next_key(into, "registries");
    container object = open_container(into->builder, into->level + 1, '{');
    write_text_member(&object, "npm", file->registry_npm);
    write_text_member(&object, "maven", file->registry_maven);
    close_container(&object);
}

static void write_dependencies(container *into, const naula_repo_file *file) {
    if (file->dependency_count == 0) {
        return;
    }
    next_key(into, "dependencies");
    container list = open_container(into->builder, into->level + 1, '[');
    for (size_t index = 0; index < file->dependency_count; index++) {
        const naula_dependency *dependency = &file->dependencies[index];
        next_entry(&list);
        container object = open_container(into->builder, into->level + 2, '{');
        write_reference_members(&object, &dependency->reference);
        write_text_member(&object, "importName", dependency->import_name);
        write_text_member(&object, "commit", dependency->commit);
        write_text_member(&object, "path", dependency->path);
        close_container(&object);
    }
    close_container(&list);
}

static void write_org(text_builder *builder, int level, const naula_org_declaration *org, int is_suborg);

static void write_projects(container *into, const naula_org_declaration *org) {
    if (org->project_count == 0) {
        return;
    }
    next_key(into, "projects");
    container object = open_container(into->builder, into->level + 1, '{');
    for (size_t index = 0; index < org->project_count; index++) {
        write_text_member(&object, org->project_names[index], org->project_descriptions[index]);
    }
    close_container(&object);
}

static void write_suborgs(container *into, const naula_org_declaration *org) {
    if (org->suborg_count == 0) {
        return;
    }
    next_key(into, "suborgs");
    container object = open_container(into->builder, into->level + 1, '{');
    for (size_t index = 0; index < org->suborg_count; index++) {
        next_key(&object, org->suborgs[index].name == NULL ? "" : org->suborgs[index].name);
        write_org(into->builder, into->level + 2, &org->suborgs[index], 1);
    }
    close_container(&object);
}

static void write_hold(container *into, const naula_org_declaration *org) {
    if (org->hold_count == 0) {
        return;
    }
    next_key(into, "hold");
    container list = open_container(into->builder, into->level + 1, '[');
    for (size_t index = 0; index < org->hold_count; index++) {
        next_entry(&list);
        write_reference(into->builder, into->level + 2, &org->hold[index]);
    }
    close_container(&list);
}

static void write_org(text_builder *builder, int level, const naula_org_declaration *org, int is_suborg) {
    container object = open_container(builder, level, '{');
    if (!is_suborg) {
        write_text_member(&object, "name", org->name);
    }
    if (org->parent != NULL) {
        next_key(&object, "parent");
        write_reference(builder, level + 1, org->parent);
    }
    write_homes(&object, "accounts", org->accounts, org->account_count);
    write_text_member(&object, "rules", org->rules);
    write_projects(&object, org);
    write_suborgs(&object, org);
    write_hold(&object, org);
    close_container(&object);
}

static void write_file(text_builder *builder, const naula_repo_file *file) {
    container object = open_container(builder, 0, '{');
    next_key(&object, "format");
    append_text(builder, "2");
    write_text_member(&object, "name", file->name);
    write_text_member(&object, "importName", file->import_name);
    write_homes(&object, "homes", file->homes, file->home_count);
    write_text_member(&object, "id", file->id);
    write_metadata(&object, &file->metadata);
    write_registries(&object, file);
    write_dependencies(&object, file);
    if (file->org != NULL) {
        next_key(&object, "org");
        write_org(builder, 1, file->org, 0);
    }
    if (file->access != NULL) {
        next_key(&object, "access");
        write_value(builder, 1, file->access);
    }
    close_container(&object);
    append(builder, "\n", 1);
}

/** Reading the text back is what keeps the writer from ever emitting a file the parser refuses. */
static naula_status check_written(const text_builder *builder) {
    naula_repo_file reread;
    memset(&reread, 0, sizeof reread);
    reread.size = sizeof reread;
    naula_refusal refusal;
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    naula_status status = naula_repo_file_parse((const uint8_t *)builder->text, builder->length, &reread, &refusal);
    naula_repo_file_release(&reread);
    if (status == NAULA_OK || status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    return NAULA_MALFORMED;
}

naula_status naula_repo_file_write(const naula_repo_file *file, char **json, size_t *length) {
    *json = NULL;
    *length = 0;
    if (!repo_file_size_holds_first_shape(file)) {
        return NAULA_REFUSED;
    }
    text_builder builder = {NULL, 0, 0, 0};
    write_file(&builder, file);
    naula_status status = builder.out_of_memory ? NAULA_OUT_OF_MEMORY : check_written(&builder);
    if (status != NAULA_OK) {
        naula_memory_release(builder.text);
        return status;
    }
    *json = builder.text;
    *length = builder.length;
    return NAULA_OK;
}
