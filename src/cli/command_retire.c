#include <stdio.h>
#include <string.h>

#include "naula/git.h"
#include "naula/memory.h"
#include "sync_internal.h"

typedef struct retirement {
    cli_context context;
    char id_text[80];
    naula_place place;
    naula_resolution resolution;
    size_t retiring;
    char local_primary[128];
} retirement;

static int same_place(const naula_place *left, const naula_place *right) {
    char *left_text = naula_place_text(left);
    char *right_text = naula_place_text(right);
    int same = left_text != NULL && right_text != NULL && strcmp(left_text, right_text) == 0;
    naula_memory_release(left_text);
    naula_memory_release(right_text);
    return same;
}

static int same_homes(const naula_repo_file *left, const naula_repo_file *right) {
    if (left->home_count != right->home_count) {
        return 0;
    }
    for (size_t index = 0; index < left->home_count; index++) {
        const naula_home *one = &left->homes[index];
        const naula_home *other = &right->homes[index];
        const char *const left_texts[] = {one->provider, one->host, one->account, one->domain, one->url};
        const char *const right_texts[] = {other->provider, other->host, other->account, other->domain, other->url};
        if (one->kind != other->kind) {
            return 0;
        }
        for (size_t member = 0; member < 5; member++) {
            if ((left_texts[member] == NULL) != (right_texts[member] == NULL) ||
                (left_texts[member] != NULL && strcmp(left_texts[member], right_texts[member]) != 0)) {
                return 0;
            }
        }
    }
    return 1;
}

static naula_status local_id(const char *repository, char id_text[80], char local_primary[128],
                             naula_refusal *refusal) {
    naula_repo_file file;
    naula_status status = cli_read_repo_file(repository, &file, refusal);
    if (status == NAULA_NOT_FOUND) {
        naula_refuse(refusal, "unadopted", "run naula retire in an adopted repo", "the repo has no repo.json");
        return NAULA_REFUSED;
    }
    char *branch = NULL;
    if (status == NAULA_OK && file.home_count > 0 && file.homes[0].kind == NAULA_HOME_PROVIDER) {
        snprintf(local_primary, 128, "%s", file.homes[0].account);
    }
    if (status == NAULA_OK) {
        status = cli_working_branch(repository, &branch, refusal);
    }
    naula_id id;
    if (status == NAULA_OK) {
        status = naula_git_repository_id(repository, branch, file.id, &id, refusal);
    }
    if (status == NAULA_OK) {
        snprintf(id_text, 80, "git:%s", id.commit);
        naula_id_release(&id);
    }
    naula_memory_release(branch);
    naula_repo_file_release(&file);
    return status;
}

/** The old home must be confirmed in this run: one that cannot be heard is never retired as though it were gone. */
static naula_status confirm_the_old_home(retirement *run, naula_refusal *refusal) {
    naula_reference reference;
    memset(&reference, 0, sizeof reference);
    reference.size = sizeof reference;
    reference.id = run->id_text;
    reference.at = &run->place;
    reference.at_count = 1;
    naula_status status = cli_resolve(&run->context, &reference, &run->resolution, refusal);
    if (status != NAULA_OK) {
        cli_print_verdicts(&run->resolution);
        if (status == NAULA_TIMED_OUT || status == NAULA_UNREACHABLE) {
            naula_refusal heard = *refusal;
            naula_refuse(refusal, "home-unheard", "run naula retire again once the home answers; nothing was archived",
                         "the home to retire could not be heard, so it is not retired: %s: %s", heard.code,
                         heard.detail);
            status = NAULA_REFUSED;
        }
        return status;
    }
    run->retiring = run->resolution.home_count;
    for (size_t index = 0; index < run->resolution.home_count; index++) {
        if (same_place(&run->resolution.homes[index], &run->place)) {
            run->retiring = index;
        }
    }
    if (run->retiring == run->resolution.home_count) {
        naula_refuse(refusal, "not-a-home", "name a home the repo's homes hold", "the place is not a confirmed home of %s",
                     run->id_text);
        return NAULA_REFUSED;
    }
    const char *serving_account = run->resolution.primary_repository.account;
    if (run->retiring == run->resolution.primary && serving_account != NULL &&
        strcmp(run->local_primary, serving_account) != 0) {
        naula_refuse(refusal, "listing-not-pushed",
                     "push the working branch, whose repo.json lists the new home first, to the old home first",
                     "the old home still serves a repo.json naming itself the primary, and the local one names %s",
                     run->local_primary);
        return NAULA_REFUSED;
    }
    if (run->retiring == run->resolution.primary) {
        naula_refuse(refusal, "retire-primary", "list the new home first in homes, push, and run naula sync first",
                     "the home to retire is still the primary");
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

/** GitHub makes an archived repo read-only, so the file naming its successors must be on it before it is archived. */
static naula_status refuse_until_the_listing_is_pushed(retirement *run, naula_refusal *refusal) {
    naula_repository repository;
    naula_status status = cli_history_lookup(run->context.history, &run->place, &repository, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    naula_id id;
    char *branch = NULL;
    status = cli_history_id(run->context.history, repository.clone_url, repository.default_branch,
                            run->resolution.file.id, &id, &branch, refusal);
    uint8_t *bytes = NULL;
    size_t length = 0;
    if (status == NAULA_OK) {
        naula_id_release(&id);
        status = cli_read_home_file(&run->context, &run->place, branch, &bytes, &length, refusal);
    }
    naula_repo_file file;
    memset(&file, 0, sizeof file);
    file.size = sizeof file;
    int listed = 0;
    if (status == NAULA_OK) {
        status = naula_repo_file_parse(bytes, length, &file, refusal);
        listed = status == NAULA_OK && same_homes(&file, &run->resolution.file);
    }
    if (status == NAULA_NOT_FOUND || (status == NAULA_OK && !listed)) {
        naula_refuse(refusal, "listing-not-pushed",
                     "push the working branch, whose repo.json lists the new home, to the old home first",
                     "the old home's repo.json does not list the homes the primary's does");
        status = NAULA_REFUSED;
    }
    naula_repo_file_release(&file);
    naula_memory_release(bytes);
    naula_memory_release(branch);
    naula_repository_release(&repository);
    return status;
}

static int id_matches(const char *dependency_id, const char *id_text) {
    size_t length = strlen(id_text);
    return dependency_id != NULL && strncmp(dependency_id, id_text, length) == 0 &&
           (dependency_id[length] == '\0' || dependency_id[length] == '/');
}

static int reaches_another_home(const retirement *run, const naula_reference *reference) {
    for (size_t place = 0; place < reference->at_count; place++) {
        for (size_t index = 0; index < run->resolution.home_count; index++) {
            if (index != run->retiring && same_place(&reference->at[place], &run->resolution.homes[index])) {
                return 1;
            }
        }
    }
    return 0;
}

/** Every dependent checkout under the root must reach a home that stays, by its `at`, before the old one goes. */
static naula_status refuse_while_a_dependent_lacks(retirement *run, const char *root, naula_refusal *refusal) {
    sync_model model;
    memset(&model, 0, sizeof model);
    naula_status status = sync_scan_checkouts(&model, root);
    size_t lacking = 0;
    char first[1024] = "";
    for (size_t index = 0; status == NAULA_OK && index < model.checkout_count; index++) {
        naula_repo_file file;
        naula_refusal ignored = cli_fresh_refusal();
        if (cli_read_repo_file(model.checkouts[index].path, &file, &ignored) != NAULA_OK) {
            continue;
        }
        for (size_t entry = 0; entry < file.dependency_count; entry++) {
            const naula_dependency *dependency = &file.dependencies[entry];
            if (id_matches(dependency->reference.id, run->id_text) && !reaches_another_home(run, &dependency->reference)) {
                if (lacking++ == 0) {
                    snprintf(first, sizeof first, "%s, dependencies[%zu] (%s)", model.checkouts[index].path, entry,
                             dependency->import_name);
                }
                fprintf(stderr, "naula: dependent %s dependencies[%zu] reaches %s only through the home to retire\n",
                        model.checkouts[index].path, entry, run->id_text);
            }
        }
        naula_repo_file_release(&file);
    }
    sync_model_release(&model);
    if (status == NAULA_OK && lacking > 0) {
        naula_refuse(refusal, "dependent-lacks-home", "add the new home to each dependent's at, commit and push first",
                     "%zu dependent%s reach%s the repo only through the home to retire, the first %s", lacking,
                     lacking == 1 ? "" : "s", lacking == 1 ? "es" : "", first);
        status = NAULA_REFUSED;
    }
    return status;
}

static naula_status archive(retirement *run, naula_refusal *refusal) {
    const naula_provider *provider = naula_providers_find(run->context.providers, run->place.provider, refusal);
    if (provider == NULL) {
        return NAULA_REFUSED;
    }
    naula_status status = provider->archive(provider->context, run->place.host, run->place.number,
                                            cli_credentials_token(run->context.credentials, run->place.provider,
                                                                  run->place.host),
                                            refusal);
    if (status == NAULA_OK) {
        char *text = naula_place_text(&run->place);
        printf("archive %s\n", text == NULL ? "(home)" : text);
        naula_memory_release(text);
    }
    return status;
}

/** `naula retire <home>`: spec 7.3 step 3. Deleting the archived home is a person's act, never naula's. */
int cli_command_retire(int count, char **values) {
    cli_arguments arguments;
    if (!cli_parse_arguments("retire", count, values, CLI_ALLOW_REPO | CLI_ALLOW_ROOT | CLI_ALLOW_ACCEPT, 1, 1,
                             &arguments)) {
        return CLI_USAGE;
    }
    retirement run;
    memset(&run, 0, sizeof run);
    run.resolution.size = sizeof run.resolution;
    run.place.size = sizeof run.place;
    naula_refusal refusal = cli_fresh_refusal();
    char *repository = cli_repository(&arguments);
    char *root = NULL;
    naula_status status = repository == NULL ? NAULA_OUT_OF_MEMORY : sync_find_root(&arguments, &root, &refusal);
    if (status == NAULA_OK) {
        status = naula_place_parse(arguments.positional[0], &run.place, &refusal);
    }
    if (status == NAULA_OK && run.place.kind != NAULA_HOME_PROVIDER) {
        naula_refuse(&refusal, "home-not-a-number", "name the home by its provider number, as github:<number>",
                     "%s names no provider number", arguments.positional[0]);
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK) {
        status = local_id(repository, run.id_text, run.local_primary, &refusal);
    }
    int opened = 0;
    if (status == NAULA_OK) {
        status = cli_context_open(&arguments, &run.context, &refusal);
        opened = status == NAULA_OK;
    }
    if (status == NAULA_OK) {
        status = confirm_the_old_home(&run, &refusal);
    }
    if (status == NAULA_OK) {
        status = refuse_until_the_listing_is_pushed(&run, &refusal);
    }
    if (status == NAULA_OK) {
        status = refuse_while_a_dependent_lacks(&run, root, &refusal);
    }
    if (status == NAULA_OK) {
        status = archive(&run, &refusal);
    }
    naula_resolution_release(&run.resolution);
    naula_place_release(&run.place);
    if (opened) {
        cli_print_questions(&run.context);
        cli_context_close(&run.context);
    }
    naula_memory_release(root);
    naula_memory_release(repository);
    return status == NAULA_OK ? CLI_DONE : cli_refused(status, &refusal);
}
