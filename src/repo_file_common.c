#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "naula/json.h"
#include "naula/memory.h"
#include "repo_file_internal.h"
#include "repo_file_rules.h"

static const char *const UNKNOWN_MEMBER_FIX = "remove the member, or spell it as repo.json format 2 does";
static const char *const TOPIC_FIX = "list at most " REPO_FILE_TEXT_OF(REPO_FILE_TOPIC_COUNT_LIMIT)
                                     " different topics of lowercase letters, digits and hyphens, "
                                     REPO_FILE_TEXT_OF(REPO_FILE_TOPIC_LENGTH_LIMIT) " characters each";
static const char *const SETTINGS_FIX =
    "keep settings to visibility (\"public\" or \"private\"), defaultBranch (a name), issues and wiki (true or false)";

static void path_append(member_path *path, const char *text) {
    size_t used = strlen(path->text);
    size_t length = strlen(text);
    size_t room = sizeof path->text - 1 - used;
    if (length > room) {
        length = room;
    }
    memcpy(path->text + used, text, length);
    path->text[used + length] = '\0';
}

member_path member_path_root(void) {
    member_path path;
    path.text[0] = '\0';
    path_append(&path, "repo.json");
    return path;
}

member_path member_path_child(const member_path *parent, const char *name) {
    member_path path = *parent;
    if (strcmp(parent->text, "repo.json") == 0) {
        path.text[0] = '\0';
    } else {
        path_append(&path, ".");
    }
    path_append(&path, name);
    return path;
}

member_path member_path_index(const member_path *parent, size_t index) {
    member_path path = *parent;
    char subscript[24];
    snprintf(subscript, sizeof subscript, "[%zu]", index);
    path_append(&path, subscript);
    return path;
}

naula_status repo_file_refuse(naula_refusal *refusal, const member_path *path, const char *fix, const char *format,
                              ...) {
    char reason[768];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(reason, sizeof reason, format, arguments);
    va_end(arguments);
    naula_refuse(refusal, "repo-file-invalid", fix, "%s: %s", path->text, reason);
    return NAULA_REFUSED;
}

static int is_allowed(const char *name, const char *const *allowed) {
    for (size_t index = 0; allowed[index] != NULL; index++) {
        if (strcmp(name, allowed[index]) == 0) {
            return 1;
        }
    }
    return 0;
}

typedef int (*member_test)(const char *name, const void *context);

static int is_listed(const char *name, const void *context) {
    return context == NULL || is_allowed(name, context);
}

static int is_schema_member(const char *name, const void *context) {
    return repo_file_rules_allow_member(context, name);
}

static naula_status check_members_with(const cJSON *object, const member_path *path, member_test is_member,
                                       const void *context, naula_refusal *refusal) {
    if (!cJSON_IsObject(object)) {
        return repo_file_refuse(refusal, path, "write it as a JSON object", "is not an object");
    }
    for (const cJSON *member = object->child; member != NULL; member = member->next) {
        if (cJSON_GetObjectItemCaseSensitive(object, member->string) != member) {
            return repo_file_refuse(refusal, path, "write each member once", "\"%s\" is written twice",
                                    member->string);
        }
        if (!is_member(member->string, context)) {
            return repo_file_refuse(refusal, path, UNKNOWN_MEMBER_FIX, "\"%s\" is not a member here", member->string);
        }
    }
    return NAULA_OK;
}

naula_status repo_file_check_members(const cJSON *object, const member_path *path, const char *const *allowed,
                                     naula_refusal *refusal) {
    return check_members_with(object, path, is_listed, allowed, refusal);
}

naula_status repo_file_check_schema_members(const cJSON *object, const member_path *path, const char *location,
                                            naula_refusal *refusal) {
    return check_members_with(object, path, is_schema_member, location, refusal);
}

naula_status repo_file_read_text(const cJSON *object, const member_path *path, const char *name,
                                 int (*is_valid)(const char *text), const char *fix, char **out,
                                 naula_refusal *refusal) {
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(object, name);
    if (member == NULL) {
        return NAULA_OK;
    }
    member_path child = member_path_child(path, name);
    if (!cJSON_IsString(member) || member->valuestring == NULL) {
        return repo_file_refuse(refusal, &child, fix, "is not a string");
    }
    if (!is_valid(member->valuestring)) {
        return repo_file_refuse(refusal, &child, fix, "\"%.200s\" is out of shape", member->valuestring);
    }
    *out = naula_memory_duplicate_text(member->valuestring);
    return *out == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}

naula_status repo_file_require(const cJSON *object, const member_path *path, const char *name, const char *fix,
                               naula_refusal *refusal) {
    if (cJSON_GetObjectItemCaseSensitive(object, name) != NULL) {
        return NAULA_OK;
    }
    member_path child = member_path_child(path, name);
    return repo_file_refuse(refusal, &child, fix, "is missing");
}

static int is_lowercase_or_digit(char character) {
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
}

int repo_file_is_import_name(const char *text) {
    size_t length = strlen(text);
    if (!repo_file_is_folder_name(text)) {
        return 0;
    }
    if (length == 0 || !is_lowercase_or_digit(text[0]) || !is_lowercase_or_digit(text[length - 1])) {
        return 0;
    }
    for (size_t index = 1; index < length; index++) {
        int is_joiner = text[index] == '-' || text[index] == '.';
        if (!is_lowercase_or_digit(text[index]) && !(is_joiner && is_lowercase_or_digit(text[index + 1]))) {
            return 0;
        }
    }
    return 1;
}

int repo_file_is_commit(const char *text) {
    size_t length = strlen(text);
    if (length != REPO_FILE_SHORT_HASH_DIGITS && length != REPO_FILE_LONG_HASH_DIGITS) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        if (!((text[index] >= '0' && text[index] <= '9') || (text[index] >= 'a' && text[index] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int is_path_character(char character) {
    return is_lowercase_or_digit(character) || (character >= 'A' && character <= 'Z') || character == '.' ||
           character == '_' || character == '-';
}

static int is_named(const char *segment, size_t length, const char *name) {
    if (length != strlen(name)) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        char character = segment[index] >= 'A' && segment[index] <= 'Z' ? (char)(segment[index] - 'A' + 'a')
                                                                          : segment[index];
        if (character != name[index]) {
            return 0;
        }
    }
    return 1;
}

/** Windows opens the device for these names with any extension, so such a folder can never be created there. */
static int is_device_name(const char *segment, size_t length) {
    const char *dot = memchr(segment, '.', length);
    size_t base = dot == NULL ? length : (size_t)(dot - segment);
    if (is_named(segment, base, "con") || is_named(segment, base, "prn") || is_named(segment, base, "aux") ||
        is_named(segment, base, "nul")) {
        return 1;
    }
    int is_port = base == 4 && (is_named(segment, 3, "com") || is_named(segment, 3, "lpt"));
    return is_port && segment[3] >= '1' && segment[3] <= '9';
}

static int is_path_segment(const char *segment, size_t length) {
    if (length == 0 || segment[length - 1] == '.' || is_named(segment, length, ".git") ||
        is_device_name(segment, length)) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        if (!is_path_character(segment[index])) {
            return 0;
        }
    }
    return 1;
}

int repo_file_is_folder_name(const char *text) {
    return is_path_segment(text, strlen(text));
}

int repo_file_is_relative_path(const char *text) {
    size_t length = strlen(text);
    if (length == 0 || length > REPO_FILE_PATH_LENGTH_LIMIT) {
        return 0;
    }
    const char *segment = text;
    for (const char *slash = strchr(segment, '/'); slash != NULL; slash = strchr(segment, '/')) {
        if (!is_path_segment(segment, (size_t)(slash - segment))) {
            return 0;
        }
        segment = slash + 1;
    }
    return is_path_segment(segment, strlen(segment));
}

int repo_file_is_any_text(const char *text) {
    (void)text;
    return 1;
}

void *repo_file_allocate_array(size_t count, size_t element_size) {
    if (count == 0 || count > SIZE_MAX / element_size) {
        return NULL;
    }
    void *block = naula_memory_allocate(count * element_size);
    if (block != NULL) {
        memset(block, 0, count * element_size);
    }
    return block;
}

int repo_file_is_topic(const char *text) {
    size_t length = strlen(text);
    if (length == 0 || length > REPO_FILE_TOPIC_LENGTH_LIMIT || !is_lowercase_or_digit(text[0])) {
        return 0;
    }
    for (size_t index = 1; index < length; index++) {
        if (!is_lowercase_or_digit(text[index]) && text[index] != '-') {
            return 0;
        }
    }
    return 1;
}

/**
 * Whether topic repeats one before it in the list. A host keeps each topic once (Gitea drops a repeat and answers
 * 204), so a file that repeats one would differ from its host on every sync and be written back for ever.
 */
static int is_listed_before(const cJSON *topics, const cJSON *topic) {
    for (const cJSON *earlier = topics->child; earlier != topic; earlier = earlier->next) {
        if (strcmp(earlier->valuestring, topic->valuestring) == 0) {
            return 1;
        }
    }
    return 0;
}

static naula_status read_topics(const cJSON *topics, const member_path *path, naula_metadata *metadata,
                                naula_refusal *refusal) {
    int count = cJSON_GetArraySize(topics);
    if (!cJSON_IsArray(topics) || count > REPO_FILE_TOPIC_COUNT_LIMIT) {
        return repo_file_refuse(refusal, path, TOPIC_FIX, "is not a list of at most %d topics",
                                REPO_FILE_TOPIC_COUNT_LIMIT);
    }
    metadata->present |= NAULA_METADATA_TOPICS;
    metadata->topics = repo_file_allocate_array((size_t)count, sizeof *metadata->topics);
    if (count > 0 && metadata->topics == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    metadata->topic_count = (size_t)count;
    size_t index = 0;
    for (const cJSON *topic = topics->child; topic != NULL; topic = topic->next, index++) {
        member_path topic_path = member_path_index(path, index);
        if (!cJSON_IsString(topic) || topic->valuestring == NULL || !repo_file_is_topic(topic->valuestring)) {
            return repo_file_refuse(refusal, &topic_path, TOPIC_FIX, "is not a topic");
        }
        if (is_listed_before(topics, topic)) {
            return repo_file_refuse(refusal, &topic_path, TOPIC_FIX, "repeats a topic listed before it");
        }
        metadata->topics[index] = naula_memory_duplicate_text(topic->valuestring);
        if (metadata->topics[index] == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
    }
    return NAULA_OK;
}

static int is_setting_valid(const cJSON *setting) {
    if (strcmp(setting->string, "visibility") == 0) {
        return cJSON_IsString(setting) && (strcmp(setting->valuestring, "public") == 0 ||
                                           strcmp(setting->valuestring, "private") == 0);
    }
    if (strcmp(setting->string, "defaultBranch") == 0) {
        return cJSON_IsString(setting) && setting->valuestring[0] != '\0';
    }
    return cJSON_IsBool(setting);
}

static naula_status read_settings(const cJSON *settings, const member_path *path, naula_metadata *metadata,
                                  naula_refusal *refusal) {
    naula_status status = repo_file_check_schema_members(settings, path, REPO_FILE_SETTINGS, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    for (const cJSON *setting = settings->child; setting != NULL; setting = setting->next) {
        if (!is_setting_valid(setting)) {
            member_path setting_path = member_path_child(path, setting->string);
            return repo_file_refuse(refusal, &setting_path, SETTINGS_FIX, "is out of shape");
        }
    }
    metadata->present |= NAULA_METADATA_SETTINGS;
    metadata->settings = cJSON_Duplicate(settings, 1);
    return metadata->settings == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}

naula_status repo_file_read_metadata(const cJSON *object, naula_metadata *metadata, naula_refusal *refusal) {
    member_path root = member_path_root();
    naula_status status = repo_file_read_text(object, &root, "description", repo_file_is_any_text,
                                              "write one short sentence", &metadata->description, refusal);
    if (status == NAULA_OK) {
        status = repo_file_read_text(object, &root, "homepage", repo_file_is_any_text,
                                     "write a full address, for example \"https://example.org\"", &metadata->homepage,
                                     refusal);
    }
    metadata->present |= (metadata->description != NULL ? NAULA_METADATA_DESCRIPTION : 0u) |
                         (metadata->homepage != NULL ? NAULA_METADATA_HOMEPAGE : 0u);
    const cJSON *topics = cJSON_GetObjectItemCaseSensitive(object, "topics");
    if (status == NAULA_OK && topics != NULL) {
        member_path path = member_path_child(&root, "topics");
        status = read_topics(topics, &path, metadata, refusal);
    }
    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(object, "settings");
    if (status == NAULA_OK && settings != NULL) {
        member_path path = member_path_child(&root, "settings");
        status = read_settings(settings, &path, metadata, refusal);
    }
    return status;
}

void repo_file_release_reference(naula_reference *reference) {
    naula_memory_release(reference->id);
    naula_memory_release(reference->url);
    for (size_t index = 0; index < reference->at_count; index++) {
        naula_place_release(&reference->at[index]);
    }
    naula_memory_release(reference->at);
    memset(reference, 0, sizeof *reference);
    reference->size = sizeof *reference;
}

static void release_org_members(naula_org_declaration *org) {
    naula_memory_release(org->name);
    if (org->parent != NULL) {
        repo_file_release_reference(org->parent);
        naula_memory_release(org->parent);
    }
    for (size_t index = 0; index < org->account_count; index++) {
        naula_home_release(&org->accounts[index]);
    }
    naula_memory_release(org->accounts);
    naula_memory_release(org->rules);
    for (size_t index = 0; index < org->project_count; index++) {
        naula_memory_release(org->project_names[index]);
        naula_memory_release(org->project_descriptions[index]);
    }
    naula_memory_release(org->project_names);
    naula_memory_release(org->project_descriptions);
    for (size_t index = 0; index < org->suborg_count; index++) {
        release_org_members(&org->suborgs[index]);
    }
    naula_memory_release(org->suborgs);
    for (size_t index = 0; index < org->hold_count; index++) {
        repo_file_release_reference(&org->hold[index]);
    }
    naula_memory_release(org->hold);
}

void repo_file_release_org(naula_org_declaration *org) {
    if (org != NULL) {
        release_org_members(org);
        naula_memory_release(org);
    }
}

naula_org_declaration *repo_file_new_org(void) {
    naula_org_declaration *org = repo_file_allocate_array(1, sizeof *org);
    if (org != NULL) {
        org->size = sizeof *org;
    }
    return org;
}

void repo_file_start(naula_repo_file *file) {
    uint32_t size = file->size;
    memset(file, 0, sizeof *file);
    file->size = size;
    file->metadata.size = sizeof file->metadata;
}

int repo_file_size_holds_first_shape(const naula_repo_file *file) {
    return file->size >= offsetof(naula_repo_file, access) + sizeof file->access;
}

void naula_repo_file_release(naula_repo_file *file) {
    naula_memory_release(file->name);
    naula_memory_release(file->import_name);
    for (size_t index = 0; index < file->home_count; index++) {
        naula_home_release(&file->homes[index]);
    }
    naula_memory_release(file->homes);
    naula_memory_release(file->id);
    naula_metadata_release(&file->metadata);
    naula_memory_release(file->registry_npm);
    naula_memory_release(file->registry_maven);
    for (size_t index = 0; index < file->dependency_count; index++) {
        repo_file_release_reference(&file->dependencies[index].reference);
        naula_memory_release(file->dependencies[index].import_name);
        naula_memory_release(file->dependencies[index].commit);
        naula_memory_release(file->dependencies[index].path);
    }
    naula_memory_release(file->dependencies);
    repo_file_release_org(file->org);
    cJSON_Delete(file->access);
    repo_file_start(file);
}

naula_status repo_file_parse_json(const uint8_t *bytes, size_t length, cJSON **json, naula_refusal *refusal) {
    const char *reason = NULL;
    naula_status status = naula_json_parse(bytes, length, json, &reason);
    if (status == NAULA_MALFORMED) {
        naula_refuse(refusal, "repo-file-not-json", "write repo.json as one JSON object in UTF-8", "repo.json %s",
                     reason);
    }
    return status;
}
