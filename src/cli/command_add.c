#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "naula/memory.h"
#include "repo_file_internal.h"

static naula_status grow_dependencies(naula_repo_file *file, naula_dependency **added) {
    naula_dependency *grown =
        naula_memory_reallocate(file->dependencies, (file->dependency_count + 1) * sizeof *grown);
    if (grown == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    file->dependencies = grown;
    *added = &grown[file->dependency_count];
    memset(*added, 0, sizeof **added);
    (*added)->size = sizeof **added;
    (*added)->reference.size = sizeof (*added)->reference;
    return NAULA_OK;
}

static naula_status refuse_present(const naula_repo_file *file, const char *id, const char *url,
                                   const char *import_name, naula_refusal *refusal) {
    for (size_t index = 0; index < file->dependency_count; index++) {
        const naula_dependency *each = &file->dependencies[index];
        int same_repo = (id != NULL && each->reference.id != NULL && strcmp(each->reference.id, id) == 0) ||
                        (url != NULL && each->reference.url != NULL && strcmp(each->reference.url, url) == 0);
        if (same_repo && strcmp(each->import_name, import_name) == 0) {
            naula_refuse(refusal, "dependency-present", "edit the dependency's commit by hand instead",
                         "dependencies[%zu] already names %s as %s", index, id != NULL ? id : url, import_name);
            return NAULA_REFUSED;
        }
    }
    return NAULA_OK;
}

/** The commit at the tip of the working branch of the resolved primary. */
static naula_status primary_tip(cli_context *context, const naula_resolution *resolution, char tip[65],
                                naula_refusal *refusal) {
    const naula_repository *primary = &resolution->primary_repository;
    naula_id id;
    char *branch = NULL;
    naula_status status = cli_history_id(context->history, primary->clone_url, primary->default_branch,
                                         resolution->file.id, &id, &branch, refusal);
    if (status == NAULA_OK) {
        naula_id_release(&id);
        status = cli_remote_tip(primary->clone_url, branch, tip, refusal);
    }
    naula_memory_release(branch);
    return status;
}

/** `{ id, at, importName, commit }`, `at` written from every home the resolver confirmed. */
static naula_status add_by_home(cli_context *context, naula_repo_file *file, const char *text,
                                naula_refusal *refusal) {
    naula_reference reference;
    naula_status status = cli_reference_from_arguments(context, text, NULL, 0, &reference, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    naula_resolution resolution;
    status = cli_resolve(context, &reference, &resolution, refusal);
    char tip[65] = "";
    if (status == NAULA_OK && resolution.primary_repository.clone_url == NULL) {
        naula_refuse(refusal, "primary-unreadable", "add the dependency from a provider home",
                     "the primary of %s is a home naula cannot read history from", reference.id);
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK) {
        status = primary_tip(context, &resolution, tip, refusal);
    }
    if (status == NAULA_OK) {
        status = refuse_present(file, reference.id, NULL, resolution.file.import_name, refusal);
    }
    naula_dependency *added = NULL;
    if (status == NAULA_OK) {
        status = grow_dependencies(file, &added);
    }
    if (status == NAULA_OK) {
        added->reference.id = naula_memory_duplicate_text(reference.id);
        added->import_name = naula_memory_duplicate_text(resolution.file.import_name);
        added->commit = naula_memory_duplicate_text(tip);
        added->reference.at = naula_memory_allocate(resolution.home_count * sizeof *added->reference.at);
        file->dependency_count++;
        status = added->reference.id == NULL || added->import_name == NULL || added->commit == NULL ||
                         added->reference.at == NULL
                     ? NAULA_OUT_OF_MEMORY
                     : NAULA_OK;
    }
    for (size_t index = 0; status == NAULA_OK && index < resolution.home_count; index++) {
        naula_reference single;
        memset(&single, 0, sizeof single);
        single.size = sizeof single;
        single.at = &resolution.homes[index];
        single.at_count = 1;
        naula_reference copied;
        status = cli_reference_copy(&single, &copied);
        if (status == NAULA_OK) {
            added->reference.at[added->reference.at_count++] = copied.at[0];
            naula_memory_release(copied.at);
        }
    }
    if (status == NAULA_OK) {
        printf("add %s as %s at %s\n", reference.id, resolution.file.import_name, tip);
    } else {
        cli_print_verdicts(&resolution);
    }
    naula_resolution_release(&resolution);
    cli_reference_release(&reference);
    return status;
}

/** A dependency by url, the warned form: its import name is the url's last segment, its commit the one its HEAD names. */
static naula_status add_by_url(naula_repo_file *file, const char *url, naula_refusal *refusal) {
    const char *slash = strrchr(url, '/');
    size_t length = slash == NULL ? 0 : strlen(slash + 1);
    if (length > 4 && strcmp(slash + 1 + length - 4, ".git") == 0) {
        length -= 4;
    }
    char *import_name = slash == NULL ? NULL : cli_format("%.*s", (int)length, slash + 1);
    if (import_name == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    char tip[65] = "";
    naula_status status = repo_file_is_import_name(import_name) ? NAULA_OK : NAULA_REFUSED;
    if (status != NAULA_OK) {
        naula_refuse(refusal, "import-name-unknown", "add it by its provider home instead",
                     "%s does not end in a name naula can import", url);
    }
    if (status == NAULA_OK) {
        status = cli_remote_tip(url, NULL, tip, refusal);
    }
    if (status == NAULA_OK) {
        status = refuse_present(file, NULL, url, import_name, refusal);
    }
    naula_dependency *added = NULL;
    if (status == NAULA_OK) {
        status = grow_dependencies(file, &added);
    }
    if (status == NAULA_OK) {
        added->reference.url = naula_memory_duplicate_text(url);
        added->import_name = import_name;
        import_name = NULL;
        added->commit = naula_memory_duplicate_text(tip);
        file->dependency_count++;
        status = added->reference.url == NULL || added->commit == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    }
    if (status == NAULA_OK) {
        printf("add %s at %s\nwarning url-dependency: a rename or move of it cannot be followed\n", url, tip);
    }
    naula_memory_release(import_name);
    return status;
}

int cli_command_add(int count, char **values) {
    cli_arguments arguments;
    if (!cli_parse_arguments("add", count, values, CLI_ALLOW_REPO | CLI_ALLOW_ACCEPT, 1, 1, &arguments)) {
        return CLI_USAGE;
    }
    char *repository = cli_repository(&arguments);
    naula_refusal refusal = cli_fresh_refusal();
    naula_repo_file file;
    memset(&file, 0, sizeof file);
    file.size = sizeof file;
    naula_status status = repository == NULL ? NAULA_OUT_OF_MEMORY : cli_read_repo_file(repository, &file, &refusal);
    if (status == NAULA_NOT_FOUND || (status == NAULA_REFUSED && strcmp(refusal.code, "repo-file-format-one") == 0)) {
        naula_refuse(&refusal, "unadopted", "run naula adopt first", "the repo has no repo.json of format 2");
        status = NAULA_REFUSED;
    }
    const char *text = arguments.positional[0];
    int by_url = strncmp(text, "https://", 8) == 0 || strncmp(text, "file://", 7) == 0;
    cli_context context;
    int opened = 0;
    if (status == NAULA_OK && !by_url) {
        status = cli_context_open(&arguments, &context, &refusal);
        opened = status == NAULA_OK;
    }
    if (status == NAULA_OK) {
        status = by_url ? add_by_url(&file, text, &refusal) : add_by_home(&context, &file, text, &refusal);
    }
    int changed = 0;
    if (status == NAULA_OK) {
        status = cli_write_repo_file(repository, &file, &changed, &refusal);
    }
    if (opened) {
        cli_print_questions(&context);
        cli_context_close(&context);
    }
    naula_repo_file_release(&file);
    naula_memory_release(repository);
    return status == NAULA_OK ? CLI_DONE : cli_refused(status, &refusal);
}
