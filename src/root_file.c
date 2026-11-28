#include <stddef.h>
#include <string.h>

#include "cJSON.h"
#include "layout_apply_internal.h"
#include "layout_internal.h"
#include "naula/json.h"
#include "naula/memory.h"
#include "repo_file_internal.h"

#define ROOT_FILE_NAME ".naula.json"

static const char *const ROOT_FILE_FIX = "write .naula.json as naula hold and naula release do: an object with top, "
                                         "and optionally added, dropped and extra";
static const char *const ROOT_MEMBERS[] = {"top", "added", "dropped", "extra", NULL};
static const char *const REFERENCE_MEMBERS[] = {"id", "url", "at", NULL};
static const char *const EXTRA_MEMBERS[] = {"repo", "org", NULL};

static void start(naula_root_file *file) {
    memset(file, 0, sizeof *file);
    file->size = sizeof *file;
    file->top.size = sizeof file->top;
}

void naula_root_file_release(naula_root_file *file) {
    if (file == NULL) {
        return;
    }
    repo_file_release_reference(&file->top);
    for (size_t index = 0; index < file->added_count; index++) {
        repo_file_release_reference(&file->added[index]);
    }
    naula_memory_release(file->added);
    for (size_t index = 0; index < file->dropped_count; index++) {
        naula_memory_release(file->dropped[index]);
    }
    naula_memory_release(file->dropped);
    for (size_t index = 0; index < file->extra_count; index++) {
        repo_file_release_reference(&file->extra[index]);
        naula_memory_release(file->extra_orgs == NULL ? NULL : file->extra_orgs[index]);
    }
    naula_memory_release(file->extra);
    naula_memory_release(file->extra_orgs);
    start(file);
}

static naula_status read_reference(const cJSON *json, const member_path *path, naula_reference *reference,
                                   naula_refusal *refusal) {
    reference->size = sizeof *reference;
    naula_status status = repo_file_check_members(json, path, REFERENCE_MEMBERS, refusal);
    return status == NAULA_OK ? repo_file_read_reference(json, path, reference, refusal) : status;
}

static naula_status require_array(const cJSON *json, const member_path *path, const char *name, size_t *count,
                                  naula_refusal *refusal) {
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(json, name);
    *count = 0;
    if (member == NULL) {
        return NAULA_OK;
    }
    if (!cJSON_IsArray(member)) {
        member_path child = member_path_child(path, name);
        return repo_file_refuse(refusal, &child, ROOT_FILE_FIX, "is not an array");
    }
    *count = (size_t)cJSON_GetArraySize(member);
    return NAULA_OK;
}

static naula_status read_added(const cJSON *json, const member_path *path, naula_root_file *file,
                               naula_refusal *refusal) {
    size_t count = 0;
    naula_status status = require_array(json, path, "added", &count, refusal);
    if (status != NAULA_OK || count == 0) {
        return status;
    }
    file->added = repo_file_allocate_array(count, sizeof *file->added);
    if (file->added == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    member_path list = member_path_child(path, "added");
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(json, "added")) {
        member_path child = member_path_index(&list, file->added_count);
        status = read_reference(item, &child, &file->added[file->added_count], refusal);
        file->added_count++;
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

static naula_status read_org_name(const cJSON *json, const member_path *path, char **name, naula_refusal *refusal) {
    if (!cJSON_IsString(json) || !repo_file_is_folder_name(json->valuestring)) {
        return repo_file_refuse(refusal, path, "name an org as its folder is named, with no / and no . or ..",
                                "is not an org name");
    }
    *name = naula_memory_duplicate_text(json->valuestring);
    return *name == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}

static naula_status read_dropped(const cJSON *json, const member_path *path, naula_root_file *file,
                                 naula_refusal *refusal) {
    size_t count = 0;
    naula_status status = require_array(json, path, "dropped", &count, refusal);
    if (status != NAULA_OK || count == 0) {
        return status;
    }
    file->dropped = repo_file_allocate_array(count, sizeof *file->dropped);
    if (file->dropped == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    member_path list = member_path_child(path, "dropped");
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(json, "dropped")) {
        member_path child = member_path_index(&list, file->dropped_count);
        status = read_org_name(item, &child, &file->dropped[file->dropped_count], refusal);
        if (status != NAULA_OK) {
            return status;
        }
        file->dropped_count++;
    }
    return NAULA_OK;
}

static naula_status read_extra_item(const cJSON *item, const member_path *path, naula_root_file *file,
                                    naula_refusal *refusal) {
    size_t index = file->extra_count;
    naula_status status = repo_file_check_members(item, path, EXTRA_MEMBERS, refusal);
    if (status == NAULA_OK) {
        status = repo_file_require(item, path, "repo", ROOT_FILE_FIX, refusal);
    }
    member_path repo = member_path_child(path, "repo");
    if (status == NAULA_OK) {
        status = read_reference(cJSON_GetObjectItemCaseSensitive(item, "repo"), &repo, &file->extra[index], refusal);
    }
    file->extra_count++;
    const cJSON *org = cJSON_GetObjectItemCaseSensitive(item, "org");
    if (status == NAULA_OK && org != NULL) {
        member_path org_path = member_path_child(path, "org");
        status = read_org_name(org, &org_path, &file->extra_orgs[index], refusal);
    }
    return status;
}

static naula_status read_extra(const cJSON *json, const member_path *path, naula_root_file *file,
                               naula_refusal *refusal) {
    size_t count = 0;
    naula_status status = require_array(json, path, "extra", &count, refusal);
    if (status != NAULA_OK || count == 0) {
        return status;
    }
    file->extra = repo_file_allocate_array(count, sizeof *file->extra);
    file->extra_orgs = repo_file_allocate_array(count, sizeof *file->extra_orgs);
    if (file->extra == NULL || file->extra_orgs == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    member_path list = member_path_child(path, "extra");
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(json, "extra")) {
        member_path child = member_path_index(&list, file->extra_count);
        status = read_extra_item(item, &child, file, refusal);
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

static naula_status read_root_file(const cJSON *json, naula_root_file *file, naula_refusal *refusal) {
    member_path path = member_path_root();
    naula_status status = repo_file_check_members(json, &path, ROOT_MEMBERS, refusal);
    if (status == NAULA_OK) {
        status = repo_file_require(json, &path, "top", ROOT_FILE_FIX, refusal);
    }
    if (status == NAULA_OK) {
        member_path top = member_path_child(&path, "top");
        status = read_reference(cJSON_GetObjectItemCaseSensitive(json, "top"), &top, &file->top, refusal);
    }
    if (status == NAULA_OK) {
        status = read_added(json, &path, file, refusal);
    }
    if (status == NAULA_OK) {
        status = read_dropped(json, &path, file, refusal);
    }
    if (status == NAULA_OK) {
        status = read_extra(json, &path, file, refusal);
    }
    return status;
}

static naula_status root_file_path(const char *root, char **path, naula_refusal *refusal) {
    *path = NULL;
    naula_status status = apply_check_root(root, 0, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    *path = layout_join(root, ROOT_FILE_NAME);
    if (*path == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    disk_kind kind = DISK_MISSING;
    unsigned long error = 0;
    status = disk_kind_of(*path, &kind, &error);
    if (status == NAULA_REFUSED) {
        return apply_refuse_disk(refusal, "root-unreadable", ROOT_FILE_FIX, "cannot read", *path, error);
    }
    if (status == NAULA_OK && kind != DISK_MISSING && kind != DISK_FILE) {
        naula_refuse(refusal, "record-is-link", "replace it with the regular file naula writes", "%s is not a regular "
                                                                                               "file",
                     *path);
        return NAULA_REFUSED;
    }
    return status == NAULA_OK && kind == DISK_MISSING ? NAULA_NOT_FOUND : status;
}

naula_status naula_root_file_read(const char *root, naula_root_file *file, naula_refusal *refusal) {
    start(file);
    char *path = NULL;
    naula_status status = root_file_path(root, &path, refusal);
    uint8_t *bytes = NULL;
    size_t length = 0;
    unsigned long error = 0;
    if (status == NAULA_OK) {
        status = disk_file_read(path, &bytes, &length, &error);
        if (status == NAULA_REFUSED || status == NAULA_NOT_FOUND) {
            status = apply_refuse_disk(refusal, "root-unreadable", ROOT_FILE_FIX, "cannot read", path, error);
        }
    }
    cJSON *json = NULL;
    if (status == NAULA_OK) {
        const char *reason = NULL;
        status = naula_json_parse(bytes, length, &json, &reason);
        if (status == NAULA_MALFORMED) {
            naula_refuse(refusal, "root-file-invalid", ROOT_FILE_FIX, "%s %s", path, reason);
            status = NAULA_REFUSED;
        }
    }
    if (status == NAULA_OK) {
        status = read_root_file(json, file, refusal);
        if (status == NAULA_MALFORMED) {
            status = NAULA_REFUSED;
        }
    }
    cJSON_Delete(json);
    naula_memory_release(bytes);
    naula_memory_release(path);
    if (status != NAULA_OK) {
        naula_root_file_release(file);
    }
    return status;
}

static cJSON *reference_json(const naula_reference *reference) {
    cJSON *object = cJSON_CreateObject();
    if (object == NULL) {
        return NULL;
    }
    int built = (reference->id == NULL || cJSON_AddStringToObject(object, "id", reference->id) != NULL) &&
                (reference->url == NULL || cJSON_AddStringToObject(object, "url", reference->url) != NULL);
    cJSON *at = built && reference->at_count > 0 ? cJSON_AddArrayToObject(object, "at") : NULL;
    built = built && (reference->at_count == 0 || at != NULL);
    for (size_t index = 0; built && index < reference->at_count; index++) {
        char *text = naula_place_text(&reference->at[index]);
        cJSON *item = text == NULL ? NULL : cJSON_CreateString(text);
        naula_memory_release(text);
        built = item != NULL && cJSON_AddItemToArray(at, item);
        if (!built) {
            cJSON_Delete(item);
        }
    }
    if (!built) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static int add_references(cJSON *json, const char *name, const naula_reference *references, size_t count) {
    if (count == 0) {
        return 1;
    }
    cJSON *list = cJSON_AddArrayToObject(json, name);
    for (size_t index = 0; list != NULL && index < count; index++) {
        cJSON *item = reference_json(&references[index]);
        if (item == NULL || !cJSON_AddItemToArray(list, item)) {
            cJSON_Delete(item);
            return 0;
        }
    }
    return list != NULL;
}

static int add_extra(cJSON *json, const naula_root_file *file) {
    if (file->extra_count == 0) {
        return 1;
    }
    cJSON *list = cJSON_AddArrayToObject(json, "extra");
    for (size_t index = 0; list != NULL && index < file->extra_count; index++) {
        cJSON *item = cJSON_CreateObject();
        cJSON *repo = item == NULL ? NULL : reference_json(&file->extra[index]);
        const char *org = file->extra_orgs == NULL ? NULL : file->extra_orgs[index];
        int built = repo != NULL && cJSON_AddItemToObject(item, "repo", repo) &&
                    (org == NULL || cJSON_AddStringToObject(item, "org", org) != NULL) &&
                    cJSON_AddItemToArray(list, item);
        if (!built) {
            if (repo != NULL && cJSON_GetObjectItemCaseSensitive(item, "repo") == NULL) {
                cJSON_Delete(repo);
            }
            cJSON_Delete(item);
            return 0;
        }
    }
    return list != NULL;
}

static cJSON *root_file_json(const naula_root_file *file) {
    cJSON *json = cJSON_CreateObject();
    cJSON *top = json == NULL ? NULL : reference_json(&file->top);
    int built = top != NULL && cJSON_AddItemToObject(json, "top", top);
    if (!built) {
        cJSON_Delete(top);
    }
    built = built && add_references(json, "added", file->added, file->added_count);
    cJSON *dropped = built && file->dropped_count > 0 ? cJSON_AddArrayToObject(json, "dropped") : NULL;
    built = built && (file->dropped_count == 0 || dropped != NULL);
    for (size_t index = 0; built && index < file->dropped_count; index++) {
        cJSON *name = cJSON_CreateString(file->dropped[index]);
        built = name != NULL && cJSON_AddItemToArray(dropped, name);
    }
    built = built && add_extra(json, file);
    if (!built) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static naula_status check_written_file(const naula_root_file *file, naula_refusal *refusal) {
    if (file == NULL || file->size < offsetof(naula_root_file, extra_count) + sizeof file->extra_count ||
        (file->top.id == NULL) == (file->top.url == NULL)) {
        naula_refuse(refusal, "root-file-invalid", ROOT_FILE_FIX, "the root file names no top org");
        return NAULA_REFUSED;
    }
    for (size_t index = 0; index < file->dropped_count; index++) {
        if (!repo_file_is_folder_name(file->dropped[index])) {
            naula_refuse(refusal, "root-file-invalid", ROOT_FILE_FIX, "dropped[%zu] is not an org name", index);
            return NAULA_REFUSED;
        }
    }
    return NAULA_OK;
}

static naula_status write_root_file(const char *root, const naula_root_file *file, naula_refusal *refusal) {
    char *path = NULL;
    naula_status status = root_file_path(root, &path, refusal);
    status = status == NAULA_NOT_FOUND ? NAULA_OK : status;
    cJSON *json = status == NAULA_OK ? root_file_json(file) : NULL;
    char *text = json == NULL ? NULL : cJSON_Print(json);
    if (status == NAULA_OK && text == NULL) {
        status = NAULA_OUT_OF_MEMORY;
    }
    char *terminated = text == NULL ? NULL : layout_concatenate(text, "\n");
    if (status == NAULA_OK && terminated == NULL) {
        status = NAULA_OUT_OF_MEMORY;
    }
    unsigned long error = 0;
    if (status == NAULA_OK) {
        status = disk_file_replace(root, ROOT_FILE_NAME, (const uint8_t *)terminated, strlen(terminated),
                                   DISK_FOR_NAULA_FILE, &error);
        if (status == NAULA_REFUSED) {
            status = apply_refuse_disk(refusal, "root-file-unwritable", ROOT_FILE_FIX, "cannot write", path, error);
        }
    }
    naula_memory_release(terminated);
    cJSON_free(text);
    cJSON_Delete(json);
    naula_memory_release(path);
    return status;
}

naula_status naula_root_file_write(const char *root, const naula_root_file *file, naula_refusal *refusal) {
    naula_status status = check_written_file(file, refusal);
    if (status == NAULA_OK) {
        status = apply_check_root(root, 1, refusal);
    }
    disk_owned_file *lock = NULL;
    if (status == NAULA_OK) {
        status = root_lock_take(root, &lock, refusal);
    }
    if (status == NAULA_OK) {
        status = write_root_file(root, file, refusal);
    }
    root_lock_release(lock);
    return status;
}
