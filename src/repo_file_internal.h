#ifndef NAULA_REPO_FILE_INTERNAL_H
#define NAULA_REPO_FILE_INTERNAL_H

#include <stddef.h>

#include "cJSON.h"
#include "naula/repo_file.h"

#define MEMBER_PATH_LIMIT 256u

/** Where in repo.json a member sits, as `dependencies[0].at[1]`, for a refusal's detail. */
typedef struct member_path {
    char text[MEMBER_PATH_LIMIT];
} member_path;

member_path member_path_root(void);
member_path member_path_child(const member_path *parent, const char *name);
member_path member_path_index(const member_path *parent, size_t index);

naula_status repo_file_refuse(naula_refusal *refusal, const member_path *path, const char *fix, const char *format,
                              ...);

/** Refuses a non-object, a member written twice and a member not in the NULL-terminated `allowed`. */
naula_status repo_file_check_members(const cJSON *object, const member_path *path, const char *const *allowed,
                                     naula_refusal *refusal);

/** A string member through `is_valid`, copied into `out`; an absent member leaves `out` NULL. */
naula_status repo_file_read_text(const cJSON *object, const member_path *path, const char *name,
                                 int (*is_valid)(const char *text), const char *fix, char **out,
                                 naula_refusal *refusal);
naula_status repo_file_require(const cJSON *object, const member_path *path, const char *name, const char *fix,
                               naula_refusal *refusal);

int repo_file_is_import_name(const char *text);
int repo_file_is_commit(const char *text);
int repo_file_is_relative_path(const char *text);
int repo_file_is_any_text(const char *text);

/** Allocates `count` zeroed elements; NULL for a count of zero, and for no memory, which `count` tells apart. */
void *repo_file_allocate_array(size_t count, size_t element_size);

naula_status repo_file_read_metadata(const cJSON *object, naula_metadata *metadata, naula_refusal *refusal);
naula_status repo_file_read_projects(const cJSON *projects, const member_path *path, naula_org_declaration *org,
                                     naula_refusal *refusal);
naula_status repo_file_read_org(const cJSON *json, const member_path *path, const char *suborg_name,
                                naula_org_declaration *org, naula_refusal *refusal);
naula_status repo_file_read_reference(const cJSON *object, const member_path *path, naula_reference *reference,
                                      naula_refusal *refusal);

naula_org_declaration *repo_file_new_org(void);
void repo_file_release_reference(naula_reference *reference);
void repo_file_release_org(naula_org_declaration *org);
void repo_file_start(naula_repo_file *file);

int repo_file_size_holds_first_shape(const naula_repo_file *file);

#endif
