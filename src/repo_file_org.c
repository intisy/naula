#include <string.h>

#include "home_internal.h"
#include "naula/id.h"
#include "naula/memory.h"
#include "repo_file_internal.h"
#include "repo_file_rules.h"

static const char *const REFERENCE_FIX =
    "write { \"id\": \"git:<hex>\", \"at\": [ \"<provider>:<number>\" ] }, or { \"url\": \"https://...\" } alone";
static const char *const ORG_NAME_FIX = "use lowercase words joined by single hyphens or dots, for example \"spisor\"";
static const char *const PROJECT_FIX = "describe each project in one line, keyed by a lowercase project folder name";

static naula_status refuse_with_inner(naula_status status, const naula_refusal *inner, const member_path *path,
                                      const char *fallback_fix, naula_refusal *refusal) {
    if (status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    return repo_file_refuse(refusal, path, inner->fix != NULL ? inner->fix : fallback_fix, "%s", inner->detail);
}

static naula_status refuse_id(const char *id, const member_path *path, naula_refusal *refusal) {
    naula_id parsed;
    memset(&parsed, 0, sizeof parsed);
    parsed.size = sizeof parsed;
    naula_refusal inner;
    memset(&inner, 0, sizeof inner);
    inner.size = sizeof inner;
    naula_status status = naula_id_parse(id, strlen(id), &parsed, &inner);
    if (status == NAULA_OK) {
        naula_id_release(&parsed);
        return repo_file_refuse(refusal, path, REFERENCE_FIX, "is not an id");
    }
    return refuse_with_inner(status, &inner, path, REFERENCE_FIX, refusal);
}

static naula_status read_id(const cJSON *id, const member_path *path, naula_reference *reference,
                            naula_refusal *refusal) {
    if (!cJSON_IsString(id) || id->valuestring == NULL) {
        return repo_file_refuse(refusal, path, REFERENCE_FIX, "is not a string");
    }
    if (!repo_file_is_reference_id(id->valuestring)) {
        return refuse_id(id->valuestring, path, refusal);
    }
    reference->id = naula_memory_duplicate_text(id->valuestring);
    return reference->id == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}

static naula_status read_place(const cJSON *text, const member_path *path, naula_place *place,
                               naula_refusal *refusal) {
    place->size = sizeof *place;
    if (!cJSON_IsString(text) || text->valuestring == NULL) {
        return repo_file_refuse(refusal, path, REFERENCE_FIX, "is not a string");
    }
    naula_refusal inner;
    memset(&inner, 0, sizeof inner);
    inner.size = sizeof inner;
    naula_status status = naula_place_parse(text->valuestring, place, &inner);
    return status == NAULA_OK ? NAULA_OK : refuse_with_inner(status, &inner, path, REFERENCE_FIX, refusal);
}

static naula_status read_at(const cJSON *at, const member_path *path, naula_reference *reference,
                            naula_refusal *refusal) {
    int count = cJSON_GetArraySize(at);
    if (!cJSON_IsArray(at) || count < REPO_FILE_LEAST_PLACES) {
        return repo_file_refuse(refusal, path, REFERENCE_FIX, "is not a list of at least one place");
    }
    reference->at = repo_file_allocate_array((size_t)count, sizeof *reference->at);
    if (reference->at == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    reference->at_count = (size_t)count;
    size_t index = 0;
    for (const cJSON *place = at->child; place != NULL; place = place->next, index++) {
        member_path place_path = member_path_index(path, index);
        naula_status status = read_place(place, &place_path, &reference->at[index], refusal);
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

static naula_status read_url(const cJSON *url, const member_path *path, naula_reference *reference,
                             naula_refusal *refusal) {
    if (!cJSON_IsString(url) || url->valuestring == NULL) {
        return repo_file_refuse(refusal, path, REFERENCE_FIX, "is not a string");
    }
    if (!naula_home_is_git_url(url->valuestring)) {
        return repo_file_refuse(refusal, path, NAULA_HOME_PLACE_FIX, "\"%.200s\" is not an https or ssh git url",
                                url->valuestring);
    }
    reference->url = naula_memory_duplicate_text(url->valuestring);
    return reference->url == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}

static naula_status check_at_presence(const cJSON *at, const naula_reference *reference, const member_path *path,
                                      naula_refusal *refusal) {
    member_path at_path = member_path_child(path, "at");
    int names_git = reference->id != NULL && repo_file_names_git(reference->id);
    if (names_git && at == NULL) {
        return repo_file_refuse(refusal, &at_path, REFERENCE_FIX,
                                "is missing: a git id needs the places to start from");
    }
    if (!names_git && at != NULL) {
        return repo_file_refuse(refusal, &at_path, REFERENCE_FIX, "is given for a repo with no home to look in");
    }
    return NAULA_OK;
}

naula_status repo_file_read_reference(const cJSON *object, const member_path *path, naula_reference *reference,
                                      naula_refusal *refusal) {
    reference->size = sizeof *reference;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(object, "id");
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(object, "url");
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(object, "at");
    if ((id == NULL) == (url == NULL)) {
        return repo_file_refuse(refusal, path, REFERENCE_FIX, "names %s", id == NULL ? "neither an id nor a url"
                                                                                     : "both an id and a url");
    }
    member_path child = member_path_child(path, id != NULL ? "id" : "url");
    naula_status status =
        id != NULL ? read_id(id, &child, reference, refusal) : read_url(url, &child, reference, refusal);
    if (status == NAULA_OK) {
        status = check_at_presence(at, reference, path, refusal);
    }
    if (status == NAULA_OK && at != NULL) {
        member_path at_path = member_path_child(path, "at");
        status = read_at(at, &at_path, reference, refusal);
    }
    return status;
}

static naula_status read_reference_object(const cJSON *json, const member_path *path, naula_reference *reference,
                                          naula_refusal *refusal) {
    naula_status status = repo_file_check_schema_members(json, path, REPO_FILE_ORG_REFERENCE, refusal);
    if (status == NAULA_OK) {
        status = repo_file_read_reference(json, path, reference, refusal);
    }
    if (status != NAULA_OK || reference->id == NULL || repo_file_is_org_reference_id(reference->id)) {
        return status;
    }
    member_path id_path = member_path_child(path, "id");
    if (repo_file_names_local(reference->id)) {
        return repo_file_refuse(refusal, &id_path, REFERENCE_FIX, "is local:, which can never hold an org's rules");
    }
    return repo_file_refuse(refusal, &id_path, REFERENCE_FIX,
                            "names a part of a repo, where an org's rules are a whole repo");
}

int repo_file_is_one_line(const char *text) {
    return text[0] != '\0' && strpbrk(text, "\r\n") == NULL;
}

naula_status repo_file_read_projects(const cJSON *projects, const member_path *path, naula_org_declaration *org,
                                     naula_refusal *refusal) {
    naula_status status = repo_file_check_members(projects, path, NULL, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    size_t count = (size_t)cJSON_GetArraySize(projects);
    org->project_names = repo_file_allocate_array(count, sizeof *org->project_names);
    org->project_descriptions = repo_file_allocate_array(count, sizeof *org->project_descriptions);
    if (count > 0 && (org->project_names == NULL || org->project_descriptions == NULL)) {
        return NAULA_OUT_OF_MEMORY;
    }
    org->project_count = count;
    size_t index = 0;
    for (const cJSON *project = projects->child; project != NULL; project = project->next, index++) {
        member_path project_path = member_path_child(path, project->string);
        if (!repo_file_is_import_name(project->string) || !cJSON_IsString(project) ||
            !repo_file_is_one_line(project->valuestring)) {
            return repo_file_refuse(refusal, &project_path, PROJECT_FIX, "is out of shape");
        }
        org->project_names[index] = naula_memory_duplicate_text(project->string);
        org->project_descriptions[index] = naula_memory_duplicate_text(project->valuestring);
        if (org->project_names[index] == NULL || org->project_descriptions[index] == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
    }
    return NAULA_OK;
}

static naula_status read_account(const cJSON *json, const member_path *path, naula_home *account,
                                 naula_refusal *refusal) {
    account->size = sizeof *account;
    naula_status checked = repo_file_check_members(json, path, NULL, refusal);
    if (checked != NAULA_OK) {
        return checked;
    }
    naula_refusal inner;
    memset(&inner, 0, sizeof inner);
    inner.size = sizeof inner;
    naula_status status = naula_home_from_json(json, account, &inner);
    if (status != NAULA_OK) {
        return refuse_with_inner(status, &inner, path, REFERENCE_FIX, refusal);
    }
    if (account->kind != NAULA_HOME_PROVIDER) {
        return repo_file_refuse(refusal, path, "name an account as { \"provider\", \"account\" }",
                                "an org owns accounts on a provider, not a domain or a url");
    }
    return NAULA_OK;
}

static naula_status read_accounts(const cJSON *accounts, const member_path *path, naula_org_declaration *org,
                                  naula_refusal *refusal) {
    int count = cJSON_GetArraySize(accounts);
    if (!cJSON_IsArray(accounts)) {
        return repo_file_refuse(refusal, path, "list the accounts as { \"provider\", \"account\" } objects",
                                "is not a list");
    }
    org->accounts = repo_file_allocate_array((size_t)count, sizeof *org->accounts);
    if (count > 0 && org->accounts == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    org->account_count = (size_t)count;
    size_t index = 0;
    for (const cJSON *account = accounts->child; account != NULL; account = account->next, index++) {
        member_path account_path = member_path_index(path, index);
        naula_status status = read_account(account, &account_path, &org->accounts[index], refusal);
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

static naula_status read_hold(const cJSON *hold, const member_path *path, naula_org_declaration *org,
                              naula_refusal *refusal) {
    int count = cJSON_GetArraySize(hold);
    if (!cJSON_IsArray(hold) || org->parent != NULL) {
        return repo_file_refuse(refusal, path, "keep hold in the top org only, as a list of { id, at }",
                                org->parent != NULL ? "is given in an org that has a parent" : "is not a list");
    }
    org->hold = repo_file_allocate_array((size_t)count, sizeof *org->hold);
    if (count > 0 && org->hold == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    org->hold_count = (size_t)count;
    size_t index = 0;
    for (const cJSON *held = hold->child; held != NULL; held = held->next, index++) {
        member_path held_path = member_path_index(path, index);
        naula_status status = read_reference_object(held, &held_path, &org->hold[index], refusal);
        if (status != NAULA_OK) {
            return status;
        }
    }
    return NAULA_OK;
}

static naula_status read_parent(const cJSON *parent, const member_path *path, naula_org_declaration *org,
                                naula_refusal *refusal) {
    org->parent = repo_file_allocate_array(1, sizeof *org->parent);
    if (org->parent == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    org->parent->size = sizeof *org->parent;
    return read_reference_object(parent, path, org->parent, refusal);
}

static naula_status read_suborgs(const cJSON *suborgs, const member_path *path, naula_org_declaration *org,
                                 naula_refusal *refusal) {
    naula_status status = repo_file_check_members(suborgs, path, NULL, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    size_t count = (size_t)cJSON_GetArraySize(suborgs);
    org->suborgs = repo_file_allocate_array(count, sizeof *org->suborgs);
    if (count > 0 && org->suborgs == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    org->suborg_count = count;
    size_t index = 0;
    for (const cJSON *suborg = suborgs->child; suborg != NULL && status == NAULA_OK; suborg = suborg->next, index++) {
        member_path suborg_path = member_path_child(path, suborg->string);
        org->suborgs[index].size = sizeof org->suborgs[index];
        status = repo_file_read_org(suborg, &suborg_path, suborg->string, &org->suborgs[index], refusal);
    }
    return status;
}

typedef naula_status (*org_member_reader)(const cJSON *member, const member_path *path, naula_org_declaration *org,
                                          naula_refusal *refusal);

static naula_status read_optional(const cJSON *json, const member_path *path, const char *name,
                                  org_member_reader reader, naula_org_declaration *org, naula_refusal *refusal) {
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(json, name);
    if (member == NULL) {
        return NAULA_OK;
    }
    member_path child = member_path_child(path, name);
    return reader(member, &child, org, refusal);
}

static naula_status read_org_name(const cJSON *json, const member_path *path, const char *suborg_name,
                                  naula_org_declaration *org, naula_refusal *refusal) {
    if (suborg_name == NULL) {
        naula_status status = repo_file_require(json, path, "name", ORG_NAME_FIX, refusal);
        return status != NAULA_OK ? status
                                  : repo_file_read_text(json, path, "name", repo_file_is_import_name, ORG_NAME_FIX,
                                                        &org->name, refusal);
    }
    if (!repo_file_is_import_name(suborg_name)) {
        return repo_file_refuse(refusal, path, ORG_NAME_FIX, "is not an org name");
    }
    org->name = naula_memory_duplicate_text(suborg_name);
    return org->name == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}

naula_status repo_file_read_org(const cJSON *json, const member_path *path, const char *suborg_name,
                                naula_org_declaration *org, naula_refusal *refusal) {
    naula_status status =
        repo_file_check_schema_members(json, path, suborg_name == NULL ? REPO_FILE_ORG : REPO_FILE_SUBORG, refusal);
    if (status == NAULA_OK) {
        status = read_org_name(json, path, suborg_name, org, refusal);
    }
    if (status == NAULA_OK) {
        status = read_optional(json, path, "parent", read_parent, org, refusal);
    }
    if (status == NAULA_OK) {
        status = read_optional(json, path, "accounts", read_accounts, org, refusal);
    }
    if (status == NAULA_OK) {
        status = repo_file_read_text(json, path, "rules", repo_file_is_relative_path,
                                     "write the rules file's path inside this repo, for example \"RULES.md\"",
                                     &org->rules, refusal);
    }
    if (status == NAULA_OK) {
        status = read_optional(json, path, "projects", repo_file_read_projects, org, refusal);
    }
    if (status == NAULA_OK) {
        status = read_optional(json, path, "suborgs", read_suborgs, org, refusal);
    }
    if (status == NAULA_OK) {
        status = read_optional(json, path, "hold", read_hold, org, refusal);
    }
    return status;
}

int repo_file_is_reference_id(const char *text) {
    return naula_id_is_valid(text, strlen(text));
}

int repo_file_is_org_reference_id(const char *text) {
    return repo_file_names_git(text) && repo_file_is_own_id(text);
}
