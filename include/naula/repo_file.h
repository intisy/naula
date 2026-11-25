#ifndef NAULA_REPO_FILE_H
#define NAULA_REPO_FILE_H

/**
 * repo.json, format 2. Every struct here is read only up to its `size`; the caller sets the outer `size` and the
 * parser sets every nested one. Adoption is gradual: a repo with no repo.json is unadopted, which only the caller can
 * see, so naula_repo_file_parse never answers NAULA_NOT_FOUND and that answer stays free to mean "no file".
 */

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "naula/home.h"
#include "naula/metadata.h"
#include "naula/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Another repo: an `id` with the places `at` to start looking, or a `url` alone. In a file read by
 * naula_repo_file_parse_format_one, `id` is the format 1 text verbatim, segments included, and `at` its forge number.
 * First shape ends at `at_count`.
 */
typedef struct naula_reference {
    uint32_t size;
    char *id;
    char *url;
    naula_place *at;
    size_t at_count;
} naula_reference;

/** `path` is NULL when the dependency sits at the default `siblings/<importName>`. First shape ends at `path`. */
typedef struct naula_dependency {
    uint32_t size;
    naula_reference reference;
    char *import_name;
    char *commit;
    char *path;
} naula_dependency;

/**
 * The `org` member, or one of its `suborgs` (whose `name` is its key and which has no `parent` or `hold`).
 * `project_names[i]` is described by `project_descriptions[i]`. First shape ends at `hold_count`.
 */
typedef struct naula_org_declaration {
    uint32_t size;
    char *name;
    naula_reference *parent;
    naula_home *accounts;
    size_t account_count;
    char *rules;
    char **project_names;
    char **project_descriptions;
    size_t project_count;
    struct naula_org_declaration *suborgs;
    size_t suborg_count;
    naula_reference *hold;
    size_t hold_count;
} naula_org_declaration;

/**
 * `format` is the format the bytes were in; the writer always writes 2. `id` is set only for a fork taking its own
 * identity or a repo that is `local:`. `access` is kept opaque so a rewrite never drops it. First shape ends at
 * `access`.
 */
typedef struct naula_repo_file {
    uint32_t size;
    int format;
    char *name;
    char *import_name;
    naula_home *homes;
    size_t home_count;
    char *id;
    naula_metadata metadata;
    char *registry_npm;
    char *registry_maven;
    naula_dependency *dependencies;
    size_t dependency_count;
    naula_org_declaration *org;
    cJSON *access;
} naula_repo_file;

/**
 * Reads format 2. Bytes that are not JSON answer NAULA_MALFORMED (`repo-file-not-json`); a format 1 file is refused
 * as `repo-file-format-one`, a member out of shape as `repo-file-invalid` with the member's path in `detail`.
 */
naula_status naula_repo_file_parse(const uint8_t *bytes, size_t length, naula_repo_file *file, naula_refusal *refusal);

/**
 * Writes `file` as format 2: members in the order of the format's table, two-space indent, LF, a trailing newline,
 * so writing what was read changes no byte. Answers NAULA_MALFORMED, and no text, for a file the parser would refuse.
 */
naula_status naula_repo_file_write(const naula_repo_file *file, char **json, size_t *length);

void naula_repo_file_release(naula_repo_file *file);

/**
 * Reads format 1 for `naula adopt` alone: `hosts` become `homes`, `projects` an `org` with no `name`, `place` and
 * `claims` are dropped, and `name` is left NULL for adopt to take from the host.
 */
naula_status naula_repo_file_parse_format_one(const uint8_t *bytes, size_t length, naula_repo_file *file,
                                              naula_refusal *refusal);

#ifdef __cplusplus
}
#endif

#endif
