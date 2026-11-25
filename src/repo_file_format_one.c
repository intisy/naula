#include <string.h>

#include "naula/memory.h"
#include "repo_file_internal.h"

static const char *const FORMAT_ONE_FIX = "fix the format 1 file by hand, then run naula adopt again";

static naula_status read_host(const cJSON *json, const member_path *path, naula_home *home, naula_refusal *refusal) {
    home->size = sizeof *home;
    home->kind = NAULA_HOME_PROVIDER;
    const char *text = cJSON_IsString(json) ? json->valuestring : NULL;
    const char *colon = text == NULL ? NULL : strrchr(text, ':');
    if (colon == NULL || colon == text || colon[1] == '\0') {
        return repo_file_refuse(refusal, path, FORMAT_ONE_FIX, "is not <provider>[@host]:<account>");
    }
    const char *at = memchr(text, '@', (size_t)(colon - text));
    size_t word_length = (size_t)((at != NULL ? at : colon) - text);
    home->provider = naula_memory_allocate(word_length + 1);
    home->account = naula_memory_duplicate_text(colon + 1);
    if (at != NULL) {
        home->host = naula_memory_allocate((size_t)(colon - at));
    }
    if (home->provider == NULL || home->account == NULL || (at != NULL && home->host == NULL)) {
        return NAULA_OUT_OF_MEMORY;
    }
    memcpy(home->provider, text, word_length);
    home->provider[word_length] = '\0';
    if (at != NULL) {
        memcpy(home->host, at + 1, (size_t)(colon - at - 1));
        home->host[colon - at - 1] = '\0';
    }
    return NAULA_OK;
}

static naula_status read_hosts(const cJSON *object, naula_repo_file *file, naula_refusal *refusal) {
    const cJSON *hosts = cJSON_GetObjectItemCaseSensitive(object, "hosts");
    if (hosts == NULL) {
        return NAULA_OK;
    }
    member_path root = member_path_root();
    member_path path = member_path_child(&root, "hosts");
    int count = cJSON_GetArraySize(hosts);
    if (!cJSON_IsArray(hosts)) {
        return repo_file_refuse(refusal, &path, FORMAT_ONE_FIX, "is not a list");
    }
    file->homes = repo_file_allocate_array((size_t)count, sizeof *file->homes);
    if (count > 0 && file->homes == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    file->home_count = (size_t)count;
    size_t index = 0;
    for (const cJSON *host = hosts->child; host != NULL; host = host->next, index++) {
        member_path host_path = member_path_index(&path, index);
        naula_status status = read_host(host, &host_path, &file->homes[index], refusal);
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

static naula_status read_projects(const cJSON *object, naula_repo_file *file, naula_refusal *refusal) {
    const cJSON *projects = cJSON_GetObjectItemCaseSensitive(object, "projects");
    if (projects == NULL) {
        return NAULA_OK;
    }
    file->org = repo_file_new_org();
    if (file->org == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    member_path root = member_path_root();
    member_path path = member_path_child(&root, "projects");
    return repo_file_read_projects(projects, &path, file->org, refusal);
}

/** A format 1 forge id is `<word>[@host]:<number>` then segments; its place is the text before the first segment. */
static naula_status read_forge_place(const char *id, const member_path *path, naula_reference *reference,
                                     naula_refusal *refusal) {
    const char *colon = strrchr(id, ':');
    const char *segment = colon == NULL ? NULL : strchr(colon, '/');
    size_t length = segment == NULL ? strlen(id) : (size_t)(segment - id);
    char *place_text = naula_memory_allocate(length + 1);
    reference->at = repo_file_allocate_array(1, sizeof *reference->at);
    if (place_text == NULL || reference->at == NULL) {
        naula_memory_release(place_text);
        return NAULA_OUT_OF_MEMORY;
    }
    memcpy(place_text, id, length);
    place_text[length] = '\0';
    reference->at_count = 1;
    reference->at[0].size = sizeof reference->at[0];
    naula_refusal inner;
    memset(&inner, 0, sizeof inner);
    inner.size = sizeof inner;
    naula_status status = naula_place_parse(place_text, &reference->at[0], &inner);
    naula_memory_release(place_text);
    if (status == NAULA_OK || status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    return repo_file_refuse(refusal, path, FORMAT_ONE_FIX, "%s", inner.detail);
}

static naula_status read_format_one_id(const cJSON *json, const member_path *path, naula_reference *reference,
                                       naula_refusal *refusal) {
    reference->size = sizeof *reference;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");
    member_path id_path = member_path_child(path, "id");
    if (!cJSON_IsString(id) || id->valuestring == NULL) {
        return repo_file_refuse(refusal, &id_path, FORMAT_ONE_FIX, "is missing or not a string");
    }
    reference->id = naula_memory_duplicate_text(id->valuestring);
    if (reference->id == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    return strncmp(reference->id, "local:", 6) == 0 ? NAULA_OK
                                                     : read_forge_place(reference->id, &id_path, reference, refusal);
}

static naula_status read_dependency(const cJSON *json, const member_path *path, naula_dependency *dependency,
                                    naula_refusal *refusal) {
    static const char *const allowed[] = {"id", "importName", "commit", "path", NULL};
    dependency->size = sizeof *dependency;
    naula_status status = repo_file_check_members(json, path, allowed, refusal);
    if (status == NAULA_OK) {
        status = read_format_one_id(json, path, &dependency->reference, refusal);
    }
    if (status == NAULA_OK) {
        status = repo_file_require(json, path, "importName", FORMAT_ONE_FIX, refusal);
    }
    if (status == NAULA_OK) {
        status = repo_file_read_text(json, path, "importName", repo_file_is_import_name, FORMAT_ONE_FIX,
                                     &dependency->import_name, refusal);
    }
    if (status == NAULA_OK) {
        status = repo_file_require(json, path, "commit", FORMAT_ONE_FIX, refusal);
    }
    if (status == NAULA_OK) {
        status = repo_file_read_text(json, path, "commit", repo_file_is_commit, FORMAT_ONE_FIX, &dependency->commit,
                                     refusal);
    }
    if (status == NAULA_OK) {
        status = repo_file_read_text(json, path, "path", repo_file_is_relative_path, FORMAT_ONE_FIX, &dependency->path,
                                     refusal);
    }
    return status;
}

static naula_status read_dependencies(const cJSON *object, naula_repo_file *file, naula_refusal *refusal) {
    const cJSON *dependencies = cJSON_GetObjectItemCaseSensitive(object, "dependencies");
    if (dependencies == NULL) {
        return NAULA_OK;
    }
    member_path root = member_path_root();
    member_path path = member_path_child(&root, "dependencies");
    int count = cJSON_GetArraySize(dependencies);
    if (!cJSON_IsArray(dependencies)) {
        return repo_file_refuse(refusal, &path, FORMAT_ONE_FIX, "is not a list");
    }
    file->dependencies = repo_file_allocate_array((size_t)count, sizeof *file->dependencies);
    if (count > 0 && file->dependencies == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    file->dependency_count = (size_t)count;
    size_t index = 0;
    for (const cJSON *dependency = dependencies->child; dependency != NULL; dependency = dependency->next, index++) {
        member_path dependency_path = member_path_index(&path, index);
        naula_status status = read_dependency(dependency, &dependency_path, &file->dependencies[index], refusal);
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

static naula_status read_format_one(const cJSON *object, naula_repo_file *file, naula_refusal *refusal) {
    static const char *const allowed[] = {"format",   "importName", "description", "topics", "homepage", "settings",
                                          "place",    "hosts",      "projects",    "claims", "dependencies", NULL};
    member_path root = member_path_root();
    const cJSON *format = cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, "format") : NULL;
    if (!cJSON_IsNumber(format) || format->valuedouble != 1) {
        naula_refuse(refusal, "repo-file-format", "read it with naula_repo_file_parse",
                     "repo.json does not say \"format\": 1");
        return NAULA_REFUSED;
    }
    file->format = 1;
    naula_status status = repo_file_check_members(object, &root, allowed, refusal);
    if (status == NAULA_OK) {
        status = repo_file_require(object, &root, "importName", FORMAT_ONE_FIX, refusal);
    }
    if (status == NAULA_OK) {
        status = repo_file_read_text(object, &root, "importName", repo_file_is_import_name, FORMAT_ONE_FIX,
                                     &file->import_name, refusal);
    }
    if (status == NAULA_OK) {
        status = repo_file_read_metadata(object, &file->metadata, refusal);
    }
    if (status == NAULA_OK) {
        status = read_hosts(object, file, refusal);
    }
    if (status == NAULA_OK) {
        status = read_projects(object, file, refusal);
    }
    return status == NAULA_OK ? read_dependencies(object, file, refusal) : status;
}

naula_status naula_repo_file_parse_format_one(const uint8_t *bytes, size_t length, naula_repo_file *file,
                                              naula_refusal *refusal) {
    if (!repo_file_size_holds_first_shape(file)) {
        naula_refuse(refusal, "repo-file-invalid", "set size to sizeof(naula_repo_file)",
                     "the repo file struct is smaller than its first shape");
        return NAULA_REFUSED;
    }
    repo_file_start(file);
    cJSON *json = length == 0 ? NULL : cJSON_ParseWithLength((const char *)bytes, length);
    if (json == NULL) {
        naula_refuse(refusal, "repo-file-not-json", "write repo.json as one JSON object", "repo.json is not JSON");
        return NAULA_MALFORMED;
    }
    naula_status status = read_format_one(json, file, refusal);
    cJSON_Delete(json);
    if (status != NAULA_OK) {
        naula_repo_file_release(file);
    }
    return status;
}
