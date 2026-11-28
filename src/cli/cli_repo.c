#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "disk.h"
#include "naula/git.h"
#include "naula/memory.h"

char *cli_repository(const cli_arguments *arguments) {
    return cli_absolute_path(arguments->repo == NULL ? "." : arguments->repo);
}

static void trim(char *text) {
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
        text[--length] = '\0';
    }
}

naula_status cli_working_branch(const char *repository, char **branch, naula_refusal *refusal) {
    const char *development[] = {"rev-parse", "--verify", "--quiet", "refs/heads/development", NULL};
    const char *current[] = {"symbolic-ref", "--quiet", "--short", "HEAD", NULL};
    naula_run_result result;
    *branch = NULL;
    naula_status status = naula_git_run(repository, development, NULL, 0, &result, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    int has_development = result.exit_code == 0;
    naula_run_result_release(&result);
    if (has_development) {
        *branch = naula_memory_duplicate_text("development");
        return *branch == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    }
    status = cli_git(repository, current, &result, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    trim(result.output);
    *branch = naula_memory_duplicate_text(result.output);
    naula_run_result_release(&result);
    return *branch == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}

naula_status cli_read_repo_file(const char *repository, naula_repo_file *file, naula_refusal *refusal) {
    memset(file, 0, sizeof *file);
    file->size = sizeof *file;
    char *path = cli_join(repository, "repo.json");
    uint8_t *bytes = NULL;
    size_t length = 0;
    naula_status status = path == NULL ? NAULA_OUT_OF_MEMORY : cli_read_file(path, &bytes, &length, refusal);
    if (status == NAULA_OK) {
        status = naula_repo_file_parse(bytes, length, file, refusal);
    }
    naula_memory_release(bytes);
    naula_memory_release(path);
    return status;
}

naula_status cli_write_repo_file(const char *repository, const naula_repo_file *file, int *changed,
                                 naula_refusal *refusal) {
    char *text = NULL;
    size_t length = 0;
    *changed = 0;
    naula_status status = naula_repo_file_write(file, &text, &length);
    if (status != NAULA_OK) {
        if (status != NAULA_OUT_OF_MEMORY) {
            naula_refuse(refusal, "repo-file-unwritable", "fix the members the file would hold by hand",
                         "the repo.json naula made is not one the format allows");
        }
        return status;
    }
    char *path = cli_join(repository, "repo.json");
    uint8_t *before = NULL;
    size_t before_length = 0;
    naula_refusal ignored = cli_fresh_refusal();
    status = path == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    int same = status == NAULA_OK && cli_read_file(path, &before, &before_length, &ignored) == NAULA_OK &&
               before_length == length && memcmp(before, text, length) == 0;
    if (status == NAULA_OK && !same) {
        status = cli_refuse_links_below(repository, "repo.json", refusal);
    }
    if (status == NAULA_OK && !same) {
        unsigned long error = 0;
        if (disk_file_replace(repository, "repo.json", (const uint8_t *)text, length, DISK_FOR_PLAN_STEP, &error) !=
            NAULA_OK) {
            char why[256];
            disk_error_text(error, why, sizeof why);
            naula_refuse(refusal, "repo-file-unwritable", "check the folder can be written", "cannot write %s: %s",
                         path, why);
            status = NAULA_REFUSED;
        }
        *changed = status == NAULA_OK;
    }
    naula_memory_release(before);
    naula_memory_release(path);
    naula_memory_release(text);
    return status;
}

naula_status cli_remote_tip(const char *url, const char *branch, char tip[65], naula_refusal *refusal) {
    tip[0] = '\0';
    if (!cli_clone_url_is_allowed(url)) {
        naula_refuse(refusal, "clone-url-refused", "ask the host for an https url",
                     "%s is not a url naula hands to git", url == NULL ? "(none)" : url);
        return NAULA_REFUSED;
    }
    char *ref = branch == NULL ? naula_memory_duplicate_text("HEAD") : cli_format("refs/heads/%s", branch);
    if (ref == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    const char *arguments[] = {"ls-remote", url, ref, NULL};
    naula_run_result result;
    naula_status status = cli_git(NULL, arguments, &result, refusal);
    naula_memory_release(ref);
    if (status != NAULA_OK) {
        return status;
    }
    size_t length = strspn(result.output, "0123456789abcdef");
    if (length == 40 || length == 64) {
        snprintf(tip, 65, "%.*s", (int)length, result.output);
    } else {
        naula_refuse(refusal, "branch-missing", "push the branch first", "%s holds no branch %s", url, branch == NULL ? "HEAD" : branch);
        status = NAULA_REFUSED;
    }
    naula_run_result_release(&result);
    return status;
}
