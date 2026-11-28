#include <stdio.h>
#include <string.h>

#include "naula/git.h"
#include "naula/memory.h"
#include "repo_file_internal.h"
#include "sync_internal.h"

typedef struct creation {
    const cli_arguments *arguments;
    cli_context context;
    sync_request request;
    naula_resolution declaring;
    naula_repo_file file;
    char *scratch;
    char commit[65];
    naula_place primary;
    size_t failed;
} creation;

/** The org of that name the root holds by reference: its declaring repo's homes are where the new repo lives. */
static naula_status find_org(creation *run, const char *name, naula_refusal *refusal) {
    const naula_root_file *root = &run->request.root_file;
    for (size_t index = 0; index <= root->added_count; index++) {
        const naula_reference *reference = index == 0 ? &root->top : &root->added[index - 1];
        naula_status status = cli_resolve(&run->context, reference, &run->declaring, refusal);
        if (status == NAULA_OK && run->declaring.file.org != NULL && run->declaring.file.org->name != NULL &&
            strcmp(run->declaring.file.org->name, name) == 0) {
            return NAULA_OK;
        }
        if (status != NAULA_OK && status != NAULA_REFUSED) {
            cli_print_verdicts(&run->declaring);
            naula_resolution_release(&run->declaring);
            return status;
        }
        naula_resolution_release(&run->declaring);
    }
    naula_refuse(refusal, "org-not-held", "name an org this root holds by its own declaring repo, or hold it first",
                 "this root holds no org named %s by reference", name);
    return NAULA_REFUSED;
}

static naula_status copy_home(const naula_home *from, naula_home *to) {
    memset(to, 0, sizeof *to);
    to->size = sizeof *to;
    to->kind = from->kind;
    const char *const texts[] = {from->provider, from->host, from->account, from->domain, from->url};
    char **copies[] = {&to->provider, &to->host, &to->account, &to->domain, &to->url};
    for (size_t index = 0; index < 5; index++) {
        if (texts[index] != NULL && (*copies[index] = naula_memory_duplicate_text(texts[index])) == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
    }
    return NAULA_OK;
}

static naula_status make_file(creation *run, const char *name, naula_refusal *refusal) {
    const naula_org_declaration *org = run->declaring.file.org;
    const char *project = run->arguments->project;
    int declared = project == NULL;
    for (size_t index = 0; !declared && index < org->project_count; index++) {
        declared = strcmp(org->project_names[index], project) == 0;
    }
    if (!declared) {
        naula_refuse(refusal, "project-unknown", "name a project the org declares, or leave --project out",
                     "the org %s declares no project %s", org->name, project);
        return NAULA_REFUSED;
    }
    if (!repo_file_is_repo_name(name) || !repo_file_is_import_name(name)) {
        naula_refuse(refusal, "name-invalid", "choose a name of letters, digits, dots, dashes and underscores",
                     "%s is not a name a repo may take", name);
        return NAULA_REFUSED;
    }
    naula_repo_file *file = &run->file;
    file->format = 2;
    file->name = naula_memory_duplicate_text(name);
    file->import_name = naula_memory_duplicate_text(name);
    const naula_repo_file *declaring = &run->declaring.file;
    file->homes = naula_memory_allocate(declaring->home_count * sizeof *file->homes);
    naula_status status = file->name == NULL || file->import_name == NULL || file->homes == NULL ? NAULA_OUT_OF_MEMORY
                                                                                                 : NAULA_OK;
    for (size_t index = 0; status == NAULA_OK && index < declaring->home_count; index++) {
        status = copy_home(&declaring->homes[index], &file->homes[index]);
        file->home_count++;
    }
    if (status == NAULA_OK && project != NULL) {
        file->metadata.topics = naula_memory_allocate(sizeof *file->metadata.topics);
        status = file->metadata.topics == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
        if (status == NAULA_OK) {
            file->metadata.topics[0] = naula_memory_duplicate_text(project);
            file->metadata.topic_count = file->metadata.topics[0] != NULL;
            file->metadata.present = NAULA_METADATA_TOPICS;
            status = file->metadata.topics[0] == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
        }
    }
    return status;
}

static const naula_provider *provider_of(creation *run, const naula_home *home, naula_refusal *refusal) {
    return naula_providers_find(run->context.providers, home->provider, refusal);
}

static const char *token_of(creation *run, const naula_home *home) {
    return cli_credentials_token(run->context.credentials, home->provider, home->host);
}

/** Every home is asked before any is created, so a taken name creates nothing anywhere. */
static naula_status refuse_a_taken_name(creation *run, naula_refusal *refusal) {
    for (size_t index = 0; index < run->file.home_count; index++) {
        const naula_home *home = &run->file.homes[index];
        if (home->kind != NAULA_HOME_PROVIDER) {
            continue;
        }
        const naula_provider *provider = provider_of(run, home, refusal);
        uint64_t number = 0;
        naula_status status = provider == NULL ? NAULA_REFUSED
                                               : provider->find(provider->context, home->host, home->account,
                                                                run->file.name, token_of(run, home), &number, refusal);
        if (status != NAULA_OK) {
            return status;
        }
        if (number != 0) {
            naula_refuse(refusal, "name-taken", "choose another name", "%s account %s already holds %s (number %llu)",
                         home->provider, home->account, run->file.name, (unsigned long long)number);
            return NAULA_REFUSED;
        }
    }
    return NAULA_OK;
}

static naula_status git_in_scratch(creation *run, const char *const *arguments, const char *input, char out[65],
                                   naula_refusal *refusal) {
    naula_run_result result;
    naula_status status = naula_git_run(run->scratch, arguments, input, input == NULL ? 0 : strlen(input), &result,
                                        refusal);
    if (status != NAULA_OK) {
        return status;
    }
    if (result.exit_code != 0) {
        cli_git_refusal(arguments, &result, refusal);
        status = NAULA_REFUSED;
    } else {
        snprintf(out, 65, "%.*s", (int)strcspn(result.output, "\r\n"), result.output);
    }
    naula_run_result_release(&result);
    return status;
}

/**
 * The first commit, made with git's plumbing in a bare repo in the cache so no working tree is written: it holds the
 * repo.json alone, and its hash is the repo's id.
 */
static naula_status make_first_commit(creation *run, naula_refusal *refusal) {
    char *text = NULL;
    size_t length = 0;
    naula_status status = naula_repo_file_write(&run->file, &text, &length);
    if (status != NAULA_OK) {
        return status;
    }
    run->scratch = cli_join(run->context.cache_directory, "new.git");
    status = run->scratch == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    if (status == NAULA_OK && !cli_is_folder(run->scratch)) {
        const char *initialise[] = {"init", "--quiet", "--bare", run->scratch, NULL};
        naula_run_result result;
        status = cli_git(NULL, initialise, &result, refusal);
        if (status == NAULA_OK) {
            naula_run_result_release(&result);
        }
    }
    char blob[65] = "";
    char tree[65] = "";
    const char *hash[] = {"hash-object", "-w", "--stdin", NULL};
    const char *make_tree[] = {"mktree", NULL};
    char *message = cli_format("chore: create %s", run->file.name);
    if (status == NAULA_OK) {
        status = message == NULL ? NAULA_OUT_OF_MEMORY : git_in_scratch(run, hash, text, blob, refusal);
    }
    char *entry = status == NAULA_OK ? cli_format("100644 blob %s\trepo.json\n", blob) : NULL;
    if (status == NAULA_OK) {
        status = entry == NULL ? NAULA_OUT_OF_MEMORY : git_in_scratch(run, make_tree, entry, tree, refusal);
    }
    const char *commit[] = {"commit-tree", tree, "-m", message, NULL};
    if (status == NAULA_OK) {
        status = git_in_scratch(run, commit, NULL, run->commit, refusal);
    }
    naula_memory_release(entry);
    naula_memory_release(message);
    naula_memory_release(text);
    return status;
}

static naula_status push_first_commit(creation *run, const char *url, naula_refusal *refusal) {
    if (!cli_clone_url_is_allowed(url)) {
        naula_refuse(refusal, "clone-url-refused", "ask the host for an https url",
                     "the created home answers no url naula hands to git");
        return NAULA_REFUSED;
    }
    char main_branch[100];
    char development[100];
    snprintf(main_branch, sizeof main_branch, "%s:refs/heads/main", run->commit);
    snprintf(development, sizeof development, "%s:refs/heads/development", run->commit);
    const char *push[] = {"push", "--quiet", url, main_branch, development, NULL};
    naula_run_result result;
    naula_status status = cli_git(run->scratch, push, &result, refusal);
    if (status == NAULA_OK) {
        naula_run_result_release(&result);
    }
    return status;
}

static void report_failure(creation *run, const naula_home *home, const naula_refusal *refusal) {
    run->failed++;
    fprintf(stderr, "naula: warning %s account %s: %s: %s\n", home->provider, home->account, refusal->code,
            refusal->detail);
}

/** Creates each home, pushes the first commit to it and writes its metadata; a home that fails is the next sync's. */
static naula_status create_homes(creation *run, naula_refusal *refusal) {
    for (size_t index = 0; index < run->file.home_count; index++) {
        const naula_home *home = &run->file.homes[index];
        if (home->kind != NAULA_HOME_PROVIDER) {
            continue;
        }
        naula_refusal failure = cli_fresh_refusal();
        const naula_provider *provider = provider_of(run, home, &failure);
        uint64_t number = 0;
        naula_status status = provider == NULL ? NAULA_REFUSED
                                               : provider->create(provider->context, home->host, home->account,
                                                                  run->file.name, &run->file.metadata,
                                                                  token_of(run, home), &number, &failure);
        naula_repository repository;
        memset(&repository, 0, sizeof repository);
        repository.size = sizeof repository;
        if (status == NAULA_OK) {
            printf("create %s/%s\n", home->account, run->file.name);
            status = provider->lookup(provider->context, home->host, number, token_of(run, home), &repository,
                                      &failure);
        }
        if (status == NAULA_OK) {
            status = push_first_commit(run, repository.clone_url, &failure);
        }
        uint32_t applied = 0;
        if (status == NAULA_OK && run->file.metadata.present != 0) {
            status = provider->write_metadata(provider->context, home->host, number, &run->file.metadata,
                                              token_of(run, home), &applied, &failure);
        }
        naula_repository_release(&repository);
        if (status == NAULA_OUT_OF_MEMORY) {
            return status;
        }
        if (status != NAULA_OK) {
            report_failure(run, home, &failure);
            if (run->primary.number == 0) {
                *refusal = failure;
                return status;
            }
        } else if (run->primary.number == 0) {
            run->primary.size = sizeof run->primary;
            run->primary.kind = NAULA_HOME_PROVIDER;
            run->primary.provider = naula_memory_duplicate_text(home->provider);
            run->primary.host = home->host == NULL ? NULL : naula_memory_duplicate_text(home->host);
            run->primary.number = number;
            if (run->primary.provider == NULL || (home->host != NULL && run->primary.host == NULL)) {
                return NAULA_OUT_OF_MEMORY;
            }
        }
    }
    if (run->primary.number == 0) {
        naula_refuse(refusal, "no-home-created", "list a provider home in the org's declaring repo",
                     "no home of the org is one naula can create a repo in");
        return NAULA_REFUSED;
    }
    return NAULA_OK;
}

static naula_status clone_into_the_root(creation *run, naula_refusal *refusal) {
    naula_reference reference;
    memset(&reference, 0, sizeof reference);
    reference.size = sizeof reference;
    reference.id = cli_format("git:%s", run->commit);
    reference.at = &run->primary;
    reference.at_count = 1;
    if (reference.id == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    run->request.clone = &reference;
    naula_status status = sync_run(&run->context, &run->request, refusal);
    run->request.clone = NULL;
    naula_memory_release(reference.id);
    return status;
}

/** `naula new <org> <name> [--project p]`: spec section 10. The repo's first commit holds its repo.json alone. */
int cli_command_new(int count, char **values) {
    cli_arguments arguments;
    if (!cli_parse_arguments("new", count, values, CLI_ALLOW_ROOT | CLI_ALLOW_PROJECT | CLI_ALLOW_ACCEPT, 2, 2,
                             &arguments)) {
        return CLI_USAGE;
    }
    creation run;
    memset(&run, 0, sizeof run);
    run.arguments = &arguments;
    run.declaring.size = sizeof run.declaring;
    run.file.size = sizeof run.file;
    naula_refusal refusal = cli_fresh_refusal();
    naula_status status = sync_open_root(&arguments, &run.context, &run.request, &refusal);
    if (status != NAULA_OK) {
        return cli_refused(status, &refusal);
    }
    status = find_org(&run, arguments.positional[0], &refusal);
    if (status == NAULA_OK) {
        status = make_file(&run, arguments.positional[1], &refusal);
    }
    if (status == NAULA_OK) {
        status = refuse_a_taken_name(&run, &refusal);
    }
    if (status == NAULA_OK) {
        status = make_first_commit(&run, &refusal);
    }
    if (status == NAULA_OK) {
        status = create_homes(&run, &refusal);
    }
    if (status == NAULA_OK) {
        status = clone_into_the_root(&run, &refusal);
    }
    if (status == NAULA_OK && run.failed > 0) {
        naula_refuse(&refusal, "new-incomplete", "run naula sync once the hosts answer; it creates what is missing",
                     "%zu home%s could not be made ready", run.failed, run.failed == 1 ? "" : "s");
        status = NAULA_REFUSED;
    }
    naula_place_release(&run.primary);
    naula_memory_release(run.scratch);
    naula_repo_file_release(&run.file);
    naula_resolution_release(&run.declaring);
    sync_request_release(&run.request);
    cli_context_close(&run.context);
    return status == NAULA_OK ? CLI_DONE : cli_refused(status, &refusal);
}
