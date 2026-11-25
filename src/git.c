#include "naula/git.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "naula/memory.h"
#include "naula/run.h"

#define MINIMUM_GIT_MAJOR 2
#define MINIMUM_GIT_MINOR 20
#define SHA1_HEX_LENGTH 40
#define SHA256_HEX_LENGTH 64

static const char *const git_program = "git";

static void refuse_git_failed(naula_refusal *refusal, const char *what, const naula_run_result *result) {
    naula_refuse(refusal, "git-failed", "run the same git command by hand to see why", "git %s exited %d: %s", what,
                 result->exit_code, result->error);
}

/** Runs git with `arguments` after the program name; a git that cannot start is the `git-missing` refusal. */
static naula_status run_git(const char *repository, const char *const *arguments, naula_run_result *result,
                            naula_refusal *refusal) {
    const char *command[16];
    size_t count = 0;
    command[count++] = git_program;
    for (size_t index = 0; arguments[index] != NULL && count < 15; index++) {
        command[count++] = arguments[index];
    }
    command[count] = NULL;
    naula_status status = naula_run(command, repository, result, refusal);
    if (status == NAULA_NOT_FOUND) {
        naula_refuse(refusal, "git-missing", "install git 2.20 or newer and put it on PATH", "no git program was found");
        return NAULA_REFUSED;
    }
    return status;
}

static naula_status run_git_succeeding(const char *repository, const char *const *arguments, const char *what,
                                       naula_run_result *result, naula_refusal *refusal) {
    naula_status status = run_git(repository, arguments, result, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    if (result->exit_code != 0) {
        refuse_git_failed(refusal, what, result);
        naula_run_result_release(result);
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

static int is_revision_name(const char *text) {
    return text != NULL && text[0] != '\0' && text[0] != '-';
}

static naula_status refuse_revision(const char *text, naula_refusal *refusal) {
    naula_refuse(refusal, "git-revision-invalid", "name a branch, tag or commit", "\"%s\" is not a revision name",
                 text == NULL ? "" : text);
    return NAULA_MALFORMED;
}

static int is_lowercase_hex(const char *text, size_t length) {
    for (size_t index = 0; index < length; index++) {
        char letter = text[index];
        if (!((letter >= '0' && letter <= '9') || (letter >= 'a' && letter <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static size_t object_id_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0' && text[length] != '\n' && text[length] != '\r' && text[length] != ' ') {
        length++;
    }
    return (length == SHA1_HEX_LENGTH || length == SHA256_HEX_LENGTH) && is_lowercase_hex(text, length) ? length : 0;
}

naula_status naula_git_check_version(naula_refusal *refusal) {
    const char *arguments[] = {"--version", NULL};
    naula_run_result result;
    naula_status status = run_git(NULL, arguments, &result, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    int major = 0;
    int minor = 0;
    int read = result.exit_code == 0 ? sscanf(result.output, "git version %d.%d", &major, &minor) : 0;
    int recent = major > MINIMUM_GIT_MAJOR || (major == MINIMUM_GIT_MAJOR && minor >= MINIMUM_GIT_MINOR);
    if (read != 2 || !recent) {
        naula_refuse(refusal, "git-too-old", "install git 2.20 or newer", "this git is too old or unreadable: %.200s",
                     result.output);
        status = NAULA_REFUSED;
    }
    naula_run_result_release(&result);
    return status;
}

naula_status naula_git_first_parent_root(const char *repository, const char *ref, char root[65], naula_refusal *refusal) {
    if (!is_revision_name(ref)) {
        return refuse_revision(ref, refusal);
    }
    const char *arguments[] = {"rev-list", "--first-parent", "--max-parents=0", ref, NULL};
    naula_run_result result;
    naula_status status = run_git_succeeding(repository, arguments, "rev-list", &result, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    size_t length = object_id_length(result.output);
    if (length == 0) {
        naula_refuse(refusal, "git-failed", "run the same git command by hand to see why",
                     "git rev-list answered no root commit for %s", ref);
        status = NAULA_REFUSED;
    } else {
        memcpy(root, result.output, length);
        root[length] = '\0';
    }
    naula_run_result_release(&result);
    return status;
}

naula_status naula_git_is_ancestor(const char *repository, const char *commit, const char *ref, int *ancestor,
                                   naula_refusal *refusal) {
    if (!is_revision_name(commit)) {
        return refuse_revision(commit, refusal);
    }
    if (!is_revision_name(ref)) {
        return refuse_revision(ref, refusal);
    }
    const char *arguments[] = {"merge-base", "--is-ancestor", commit, ref, NULL};
    naula_run_result result;
    naula_status status = run_git(repository, arguments, &result, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    if (result.exit_code == 0 || result.exit_code == 1) {
        *ancestor = result.exit_code == 0;
    } else {
        refuse_git_failed(refusal, "merge-base", &result);
        status = NAULA_REFUSED;
    }
    naula_run_result_release(&result);
    return status;
}

naula_status naula_git_parent_count(const char *repository, const char *commit, size_t *count, naula_refusal *refusal) {
    if (!is_revision_name(commit)) {
        return refuse_revision(commit, refusal);
    }
    const char *arguments[] = {"rev-list", "--parents", "-n", "1", commit, NULL};
    naula_run_result result;
    naula_status status = run_git_succeeding(repository, arguments, "rev-list", &result, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    size_t spaces = 0;
    for (const char *cursor = result.output; *cursor != '\0' && *cursor != '\n'; cursor++) {
        spaces += *cursor == ' ';
    }
    *count = spaces;
    naula_run_result_release(&result);
    return NAULA_OK;
}

static int line_equals(const char *lines, const char *wanted, size_t length) {
    for (const char *line = lines; *line != '\0';) {
        const char *end = strchr(line, '\n');
        size_t line_length = end == NULL ? strlen(line) : (size_t)(end - line);
        if (line_length > 0 && line[line_length - 1] == '\r') {
            line_length--;
        }
        if (line_length == length && memcmp(line, wanted, length) == 0) {
            return 1;
        }
        line = end == NULL ? line + strlen(line) : end + 1;
    }
    return 0;
}

static naula_status is_on_first_parent_line(const char *repository, const char *ref, const char *commit, int *on_line,
                                            naula_refusal *refusal) {
    const char *arguments[] = {"rev-list", "--first-parent", ref, NULL};
    naula_run_result result;
    naula_status status = run_git_succeeding(repository, arguments, "rev-list", &result, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    *on_line = line_equals(result.output, commit, strlen(commit));
    naula_run_result_release(&result);
    return NAULA_OK;
}

static naula_status check_fork_exception(const char *repository, const char *ref, const naula_id *stated,
                                         naula_refusal *refusal) {
    if (stated->kind != NAULA_ID_GIT) {
        naula_refuse(refusal, "fork-id-not-on-line", "state the id of one of this repository's own commits",
                     "a local id names no commit");
        return NAULA_REFUSED;
    }
    char root[65];
    naula_status status = naula_git_first_parent_root(repository, ref, root, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    if (strcmp(root, stated->commit) == 0) {
        naula_refuse(refusal, "fork-id-is-root", "state the first commit of your own that the original does not contain",
                     "the stated id %s is the root commit, which the original shares", stated->commit);
        return NAULA_REFUSED;
    }
    int on_line = 0;
    status = is_on_first_parent_line(repository, ref, stated->commit, &on_line, refusal);
    if (status == NAULA_OK && !on_line) {
        naula_refuse(refusal, "fork-id-not-on-line", "state a commit on the first-parent line of the working branch",
                     "the stated id %s is not on the first-parent line of %s", stated->commit, ref);
        return NAULA_REFUSED;
    }
    return status;
}

static naula_status derived_id(const char *repository, const char *ref, naula_id *id, naula_refusal *refusal) {
    char root[65];
    naula_status status = naula_git_first_parent_root(repository, ref, root, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    char text[70];
    int written = snprintf(text, sizeof text, "git:%s", root);
    return naula_id_parse(text, (size_t)written, id, refusal);
}

naula_status naula_git_repository_id(const char *repository, const char *ref, const char *stated_id, naula_id *id,
                                     naula_refusal *refusal) {
    if (!is_revision_name(ref)) {
        return refuse_revision(ref, refusal);
    }
    if (stated_id == NULL) {
        return derived_id(repository, ref, id, refusal);
    }
    naula_status status = naula_id_parse(stated_id, strlen(stated_id), id, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    status = check_fork_exception(repository, ref, id, refusal);
    if (status != NAULA_OK) {
        naula_id_release(id);
    }
    return status;
}

naula_status naula_git_check_id_kept(const char *repository, const char *previous_tip, const char *new_tip,
                                     const char *stated_id, naula_refusal *refusal) {
    char previous_root[65];
    naula_status status = naula_git_first_parent_root(repository, previous_tip, previous_root, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    naula_id id;
    status = naula_git_repository_id(repository, new_tip, stated_id, &id, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    int kept = stated_id != NULL || strcmp(id.commit, previous_root) == 0;
    if (!kept) {
        naula_refuse(refusal, "id-changed", "keep the first-parent root, or state the fork exception in repo.json",
                     "the id was git:%s and would become git:%s", previous_root, id.commit);
    }
    naula_id_release(&id);
    return kept ? NAULA_OK : NAULA_REFUSED;
}

naula_status naula_git_fetch_commits(const char *repository, const char *url, const char *ref, naula_refusal *refusal) {
    if (!is_revision_name(ref)) {
        return refuse_revision(ref, refusal);
    }
    if (url == NULL || url[0] == '\0' || url[0] == '-') {
        naula_refuse(refusal, "git-url-invalid", "pass the repository's URL", "\"%s\" is not a repository URL",
                     url == NULL ? "" : url);
        return NAULA_MALFORMED;
    }
    const char *arguments[] = {"fetch", "--quiet", "--no-tags", "--filter=tree:0", url, ref, NULL};
    naula_run_result result;
    naula_status status = run_git_succeeding(repository, arguments, "fetch", &result, refusal);
    if (status == NAULA_OK) {
        naula_run_result_release(&result);
    }
    return status;
}
