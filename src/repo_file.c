#include <math.h>
#include <string.h>

#include "naula/id.h"
#include "naula/memory.h"
#include "repo_file_internal.h"
#include "repo_file_rules.h"

static const char *const NAME_FIX = "write the repo's name: letters, digits, dots, hyphens and underscores";
static const char *const IMPORT_NAME_FIX =
    "use lowercase words joined by single hyphens or dots, for example \"config-ledger\" or \"api.http\"";
static const char *const HOMES_FIX =
    "list where the repo lives, primary first, as { \"provider\", \"account\" }, { \"provider\": \"web\", \"domain\" } "
    "or { \"url\" }; only a local: repo has none";
static const char *const ID_FIX =
    "leave id out, or state git:<hex> of the fork's first own commit, or local:<name> for a repo with no home";
static const char *const DEPENDENCY_FIX = "give each dependency an id with at, or a url, then importName and commit";
static const char *const REGISTRY_FIX =
    "name the npm scope and the maven group, for example \"@spisor\" and \"com.spisor\"";

int repo_file_is_repo_name(const char *text) {
    return strlen(text) <= REPO_FILE_NAME_LENGTH_LIMIT && repo_file_is_folder_name(text);
}

int repo_file_is_registry(const char *text) {
    if (text[0] == '\0') {
        return 0;
    }
    for (const char *character = text; *character != '\0'; character++) {
        if ((unsigned char)*character <= 0x20 || (unsigned char)*character == 0x7F) {
            return 0;
        }
    }
    return 1;
}

static naula_status read_home(const cJSON *json, const member_path *path, naula_home *home, naula_refusal *refusal) {
    home->size = sizeof *home;
    if (cJSON_IsObject(json)) {
        naula_status checked = repo_file_check_members(json, path, NULL, refusal);
        if (checked != NAULA_OK) {
            return checked;
        }
    }
    naula_refusal inner;
    memset(&inner, 0, sizeof inner);
    inner.size = sizeof inner;
    naula_status status = naula_home_from_json(json, home, &inner);
    if (status == NAULA_OK || status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    return repo_file_refuse(refusal, path, inner.fix != NULL ? inner.fix : HOMES_FIX, "%s", inner.detail);
}

static naula_status read_homes(const cJSON *homes, naula_repo_file *file, naula_refusal *refusal) {
    member_path root = member_path_root();
    member_path path = member_path_child(&root, "homes");
    int count = cJSON_GetArraySize(homes);
    if (!cJSON_IsArray(homes) || count < REPO_FILE_LEAST_HOMES) {
        return repo_file_refuse(refusal, &path, HOMES_FIX, "is not a list of at least one home");
    }
    file->homes = repo_file_allocate_array((size_t)count, sizeof *file->homes);
    if (file->homes == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    file->home_count = (size_t)count;
    size_t index = 0;
    for (const cJSON *home = homes->child; home != NULL; home = home->next, index++) {
        member_path home_path = member_path_index(&path, index);
        naula_status status = read_home(home, &home_path, &file->homes[index], refusal);
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

static naula_status refuse_own_id(const char *id, naula_refusal *refusal) {
    member_path root = member_path_root();
    member_path path = member_path_child(&root, "id");
    naula_id parsed;
    memset(&parsed, 0, sizeof parsed);
    parsed.size = sizeof parsed;
    naula_refusal inner;
    memset(&inner, 0, sizeof inner);
    inner.size = sizeof inner;
    naula_status status = naula_id_parse(id, strlen(id), &parsed, &inner);
    if (status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    if (status != NAULA_OK) {
        return repo_file_refuse(refusal, &path, ID_FIX, "%s", inner.detail);
    }
    naula_id_release(&parsed);
    return repo_file_refuse(refusal, &path, ID_FIX, "names a path inside a repo, not a repo");
}

static naula_status read_own_id(const cJSON *object, naula_repo_file *file, naula_refusal *refusal) {
    member_path root = member_path_root();
    naula_status status = repo_file_read_text(object, &root, "id", repo_file_is_any_text, ID_FIX, &file->id, refusal);
    if (status != NAULA_OK || file->id == NULL || repo_file_is_own_id(file->id)) {
        return status;
    }
    return refuse_own_id(file->id, refusal);
}

static naula_status read_where(const cJSON *object, naula_repo_file *file, naula_refusal *refusal) {
    naula_status status = read_own_id(object, file, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    int is_local = file->id != NULL && repo_file_names_local(file->id);
    const cJSON *homes = cJSON_GetObjectItemCaseSensitive(object, "homes");
    member_path root = member_path_root();
    member_path path = member_path_child(&root, "homes");
    if (is_local && homes != NULL) {
        return repo_file_refuse(refusal, &path, HOMES_FIX, "is given for a local: repo, which has none");
    }
    if (is_local) {
        return NAULA_OK;
    }
    if (homes == NULL) {
        return repo_file_refuse(refusal, &path, HOMES_FIX, "is missing");
    }
    return read_homes(homes, file, refusal);
}

static naula_status read_registries(const cJSON *object, naula_repo_file *file, naula_refusal *refusal) {
    const cJSON *registries = cJSON_GetObjectItemCaseSensitive(object, "registries");
    if (registries == NULL) {
        return NAULA_OK;
    }
    member_path root = member_path_root();
    member_path path = member_path_child(&root, "registries");
    naula_status status = repo_file_check_schema_members(registries, &path, REPO_FILE_REGISTRIES, refusal);
    if (status == NAULA_OK) {
        status = repo_file_read_text(registries, &path, "npm", repo_file_is_registry, REGISTRY_FIX,
                                     &file->registry_npm, refusal);
    }
    if (status == NAULA_OK) {
        status = repo_file_read_text(registries, &path, "maven", repo_file_is_registry, REGISTRY_FIX,
                                     &file->registry_maven, refusal);
    }
    return status;
}

static naula_status read_dependency(const cJSON *json, const member_path *path, naula_dependency *dependency,
                                    naula_refusal *refusal) {
    dependency->size = sizeof *dependency;
    naula_status status = repo_file_check_schema_members(json, path, REPO_FILE_DEPENDENCY, refusal);
    if (status == NAULA_OK) {
        status = repo_file_read_reference(json, path, &dependency->reference, refusal);
    }
    if (status == NAULA_OK) {
        status = repo_file_require(json, path, "importName", DEPENDENCY_FIX, refusal);
    }
    if (status == NAULA_OK) {
        status = repo_file_read_text(json, path, "importName", repo_file_is_import_name, IMPORT_NAME_FIX,
                                     &dependency->import_name, refusal);
    }
    if (status == NAULA_OK) {
        status = repo_file_require(json, path, "commit", DEPENDENCY_FIX, refusal);
    }
    if (status == NAULA_OK) {
        status = repo_file_read_text(json, path, "commit", repo_file_is_commit,
                                     "write the full lowercase commit hash, 40 or 64 hex digits", &dependency->commit,
                                     refusal);
    }
    if (status == NAULA_OK) {
        status = repo_file_read_text(json, path, "path", repo_file_is_relative_path,
                                     "write a relative path inside the repo with no . or .. segment, for example "
                                     "\"siblings/kernel\"",
                                     &dependency->path, refusal);
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
        return repo_file_refuse(refusal, &path, DEPENDENCY_FIX, "is not a list");
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

static naula_status read_membership(const cJSON *org, naula_repo_file *file, naula_refusal *refusal) {
    file->member_of = repo_file_allocate_array(1, sizeof *file->member_of);
    if (file->member_of == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    member_path root = member_path_root();
    member_path path = member_path_child(&root, "org");
    return repo_file_read_membership(org, &path, file->member_of, refusal);
}

static naula_status read_org_member(const cJSON *object, naula_repo_file *file, naula_refusal *refusal) {
    const cJSON *org = cJSON_GetObjectItemCaseSensitive(object, "org");
    if (org == NULL) {
        return NAULA_OK;
    }
    if (cJSON_GetObjectItemCaseSensitive(org, "memberOf") != NULL) {
        return read_membership(org, file, refusal);
    }
    file->org = repo_file_new_org();
    if (file->org == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    member_path root = member_path_root();
    member_path path = member_path_child(&root, "org");
    return repo_file_read_org(org, &path, NULL, file->org, refusal);
}

static int holds_only_finite_numbers(const cJSON *value) {
    if (cJSON_IsNumber(value)) {
        return isfinite(value->valuedouble);
    }
    for (const cJSON *child = value->child; child != NULL; child = child->next) {
        if (!holds_only_finite_numbers(child)) {
            return 0;
        }
    }
    return 1;
}

static naula_status read_access(const cJSON *object, naula_repo_file *file, naula_refusal *refusal) {
    const cJSON *access = cJSON_GetObjectItemCaseSensitive(object, "access");
    if (access == NULL) {
        return NAULA_OK;
    }
    if (!holds_only_finite_numbers(access)) {
        member_path root = member_path_root();
        member_path path = member_path_child(&root, "access");
        return repo_file_refuse(refusal, &path, "write every number within the range a double holds",
                                "holds a number too large to read back");
    }
    file->access = cJSON_Duplicate(access, 1);
    return file->access == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}

static naula_status read_names(const cJSON *object, naula_repo_file *file, naula_refusal *refusal) {
    member_path root = member_path_root();
    naula_status status = repo_file_require(object, &root, "name", NAME_FIX, refusal);
    if (status == NAULA_OK) {
        status = repo_file_read_text(object, &root, "name", repo_file_is_repo_name, NAME_FIX, &file->name, refusal);
    }
    if (status == NAULA_OK) {
        status = repo_file_require(object, &root, "importName", IMPORT_NAME_FIX, refusal);
    }
    if (status == NAULA_OK) {
        status = repo_file_read_text(object, &root, "importName", repo_file_is_import_name, IMPORT_NAME_FIX,
                                     &file->import_name, refusal);
    }
    return status;
}

static naula_status check_format(const cJSON *object, naula_refusal *refusal) {
    member_path root = member_path_root();
    if (!cJSON_IsObject(object)) {
        return repo_file_refuse(refusal, &root, "write repo.json as one JSON object", "is not an object");
    }
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(object, "format");
    if (cJSON_IsNumber(format) && format->valuedouble == 1) {
        naula_refuse(refusal, "repo-file-format-one", "convert it with naula adopt",
                     "repo.json is format 1, which naula no longer reads");
        return NAULA_REFUSED;
    }
    if (!cJSON_IsNumber(format) || format->valuedouble != REPO_FILE_FORMAT) {
        naula_refuse(refusal, "repo-file-format", "set \"format\" to 2", "repo.json does not say \"format\": 2");
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

static naula_status read_file(const cJSON *object, naula_repo_file *file, naula_refusal *refusal) {
    member_path root = member_path_root();
    naula_status status = check_format(object, refusal);
    if (status == NAULA_OK) {
        status = repo_file_check_schema_members(object, &root, REPO_FILE_TOP, refusal);
    }
    file->format = REPO_FILE_FORMAT;
    if (status == NAULA_OK) {
        status = read_names(object, file, refusal);
    }
    if (status == NAULA_OK) {
        status = read_where(object, file, refusal);
    }
    if (status == NAULA_OK) {
        status = repo_file_read_metadata(object, &file->metadata, refusal);
    }
    if (status == NAULA_OK) {
        status = read_registries(object, file, refusal);
    }
    if (status == NAULA_OK) {
        status = read_dependencies(object, file, refusal);
    }
    if (status == NAULA_OK) {
        status = read_org_member(object, file, refusal);
    }
    return status == NAULA_OK ? read_access(object, file, refusal) : status;
}

naula_status naula_repo_file_parse(const uint8_t *bytes, size_t length, naula_repo_file *file,
                                   naula_refusal *refusal) {
    if (!repo_file_size_holds_first_shape(file)) {
        naula_refuse(refusal, "repo-file-invalid", "set size to sizeof(naula_repo_file)",
                     "the repo file struct is smaller than its first shape");
        return NAULA_REFUSED;
    }
    repo_file_start(file);
    cJSON *json = NULL;
    naula_status parsed = repo_file_parse_json(bytes, length, &json, refusal);
    if (parsed != NAULA_OK) {
        return parsed;
    }
    naula_status status = read_file(json, file, refusal);
    cJSON_Delete(json);
    if (status != NAULA_OK) {
        naula_repo_file_release(file);
    }
    return status;
}

int repo_file_is_own_id(const char *text) {
    return naula_id_is_valid(text, strlen(text)) && strchr(text, '/') == NULL;
}

int repo_file_names_local(const char *text) {
    return strncmp(text, "local:", 6) == 0;
}

int repo_file_names_git(const char *text) {
    return strncmp(text, "git:", 4) == 0;
}
