#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "naula/home.h"
#include "naula/memory.h"
#include "repo_file_internal.h"

void cli_reference_release(naula_reference *reference) {
    repo_file_release_reference(reference);
}

static naula_status copy_place(const naula_place *from, naula_place *to) {
    memset(to, 0, sizeof *to);
    to->size = sizeof *to;
    to->kind = from->kind;
    to->number = from->number;
    const char *const texts[] = {from->provider, from->host, from->domain, from->url};
    char **copies[] = {&to->provider, &to->host, &to->domain, &to->url};
    for (size_t index = 0; index < 4; index++) {
        if (texts[index] != NULL && (*copies[index] = naula_memory_duplicate_text(texts[index])) == NULL) {
            naula_place_release(to);
            return NAULA_OUT_OF_MEMORY;
        }
    }
    return NAULA_OK;
}

naula_status cli_reference_copy(const naula_reference *from, naula_reference *to) {
    memset(to, 0, sizeof *to);
    to->size = sizeof *to;
    to->id = from->id == NULL ? NULL : naula_memory_duplicate_text(from->id);
    to->url = from->url == NULL ? NULL : naula_memory_duplicate_text(from->url);
    to->at = from->at_count == 0 ? NULL : naula_memory_allocate(from->at_count * sizeof *to->at);
    naula_status status = (from->id != NULL && to->id == NULL) || (from->url != NULL && to->url == NULL) ||
                                  (from->at_count > 0 && to->at == NULL)
                              ? NAULA_OUT_OF_MEMORY
                              : NAULA_OK;
    for (size_t index = 0; status == NAULA_OK && index < from->at_count; index++) {
        status = copy_place(&from->at[index], &to->at[index]);
        to->at_count += status == NAULA_OK;
    }
    if (status != NAULA_OK) {
        cli_reference_release(to);
    }
    return status;
}

static naula_status parse_places(const char *const *at, size_t at_count, naula_reference *reference,
                                 naula_refusal *refusal) {
    reference->at = at_count == 0 ? NULL : naula_memory_allocate(at_count * sizeof *reference->at);
    if (at_count > 0 && reference->at == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < at_count; index++) {
        naula_place *place = &reference->at[index];
        memset(place, 0, sizeof *place);
        place->size = sizeof *place;
        naula_status status = naula_place_parse(at[index], place, refusal);
        if (status != NAULA_OK) {
            return status;
        }
        reference->at_count++;
    }
    return NAULA_OK;
}

static naula_status from_id(const char *text, const char *const *at, size_t at_count, naula_reference *reference,
                            naula_refusal *refusal) {
    if (strncmp(text, "git:", 4) == 0 && at_count == 0) {
        naula_refuse(refusal, "at-missing", "name where to start looking with --at <place>",
                     "a git id needs at least one place to look for it");
        return NAULA_REFUSED;
    }
    reference->id = naula_memory_duplicate_text(text);
    if (reference->id == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    return parse_places(at, at_count, reference, refusal);
}

/** The repo.json `id` member of the home, NULL when it states none; the home's file is read on `branch`. */
static naula_status stated_id_of(cli_context *context, const naula_place *place, const char *branch, char **stated,
                                 naula_refusal *refusal) {
    uint8_t *bytes = NULL;
    size_t length = 0;
    *stated = NULL;
    naula_status status = cli_read_home_file(context, place, branch, &bytes, &length, refusal);
    if (status == NAULA_NOT_FOUND) {
        return NAULA_OK;
    }
    if (status != NAULA_OK) {
        return status;
    }
    naula_repo_file file;
    memset(&file, 0, sizeof file);
    file.size = sizeof file;
    status = naula_repo_file_parse(bytes, length, &file, refusal);
    naula_memory_release(bytes);
    if (status == NAULA_OK && file.id != NULL) {
        *stated = naula_memory_duplicate_text(file.id);
        status = *stated == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    }
    naula_repo_file_release(&file);
    return status;
}

/** The id of the repo at a provider place: its history's first-parent root, or the fork exception its file states. */
static naula_status derive_id(cli_context *context, const naula_place *place, char **id_text, naula_refusal *refusal) {
    naula_repository repository;
    naula_status status = cli_history_lookup(context->history, place, &repository, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    naula_id id;
    char *branch = NULL;
    char *stated = NULL;
    status = cli_history_id(context->history, repository.clone_url, repository.default_branch, NULL, &id, &branch,
                            refusal);
    if (status == NAULA_OK) {
        status = stated_id_of(context, place, branch, &stated, refusal);
    }
    if (status == NAULA_OK && stated != NULL) {
        naula_id_release(&id);
        naula_memory_release(branch);
        branch = NULL;
        status = cli_history_id(context->history, repository.clone_url, repository.default_branch, stated, &id,
                                &branch, refusal);
    }
    if (status == NAULA_OK) {
        *id_text = stated != NULL ? naula_memory_duplicate_text(stated) : cli_format("git:%s", id.commit);
        status = *id_text == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
        naula_id_release(&id);
    }
    naula_memory_release(stated);
    naula_memory_release(branch);
    naula_repository_release(&repository);
    return status;
}

naula_status cli_reference_from_arguments(cli_context *context, const char *text, const char *const *at,
                                          size_t at_count, naula_reference *reference, naula_refusal *refusal) {
    memset(reference, 0, sizeof *reference);
    reference->size = sizeof *reference;
    naula_status status = NAULA_OK;
    if (naula_id_is_valid(text, strlen(text))) {
        status = from_id(text, at, at_count, reference, refusal);
    } else if (at_count > 0) {
        naula_refuse(refusal, "at-without-id", "pass --at only after an id", "%s is not an id, so --at has nothing to narrow",
                     text);
        status = NAULA_REFUSED;
    } else {
        status = parse_places(&text, 1, reference, refusal);
        if (status == NAULA_OK && reference->at[0].kind != NAULA_HOME_PROVIDER) {
            naula_refuse(refusal, "home-not-a-number", "name the home by its provider number, as github:<number>",
                         "%s names no provider number, so naula cannot ask a host which repo it is", text);
            status = NAULA_REFUSED;
        }
        if (status == NAULA_OK) {
            status = derive_id(context, &reference->at[0], &reference->id, refusal);
        }
    }
    if (status != NAULA_OK) {
        cli_reference_release(reference);
    }
    return status;
}
