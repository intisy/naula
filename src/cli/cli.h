#ifndef NAULA_CLI_H
#define NAULA_CLI_H

#include <stddef.h>
#include <stdint.h>

#include "naula/history.h"
#include "naula/id.h"
#include "naula/provider.h"
#include "naula/reconcile.h"
#include "naula/repo_file.h"
#include "naula/resolve.h"
#include "naula/run.h"
#include "naula/session.h"
#include "naula/status.h"

#define CLI_DONE 0
#define CLI_REFUSED 1
#define CLI_USAGE 2

#define CLI_LIST_LIMIT 32

#define CLI_ALLOW_AT (1u << 0)
#define CLI_ALLOW_ROOT (1u << 1)
#define CLI_ALLOW_REPO (1u << 2)
#define CLI_ALLOW_PREVIOUS (1u << 3)
#define CLI_ALLOW_JSON (1u << 4)
#define CLI_ALLOW_OFFLINE (1u << 5)
#define CLI_ALLOW_ACCEPT (1u << 6)
#define CLI_ALLOW_PROJECT (1u << 7)
#define CLI_ALLOW_FORK_ID (1u << 8)

/** A command's words after its name. Every option value points into the process's own arguments. */
typedef struct cli_arguments {
    const char *positional[CLI_LIST_LIMIT];
    size_t positional_count;
    const char *at[CLI_LIST_LIMIT];
    size_t at_count;
    const char *root;
    const char *repo;
    const char *previous;
    const char *project;
    const char *fork_id;
    const char *accept_primary[CLI_LIST_LIMIT];
    size_t accept_primary_count;
    uint64_t accept_number[CLI_LIST_LIMIT];
    size_t accept_number_count;
    int json;
    int offline;
} cli_arguments;

/** 0 after printing why the words are not a use of the command; `allowed` holds the CLI_ALLOW_ options it takes. */
int cli_parse_arguments(const char *command, int count, char **values, unsigned allowed, size_t minimum,
                        size_t maximum, cli_arguments *arguments);

void cli_usage(void);
int cli_usage_error(const char *command, const char *format, ...);

/** Prints the refusal's code, detail and fix to standard error and answers CLI_REFUSED. */
int cli_refused(naula_status status, const naula_refusal *refusal);
naula_refusal cli_fresh_refusal(void);

/** Text helpers; every answer is released with naula_memory_release and is NULL only when memory ran out. */
char *cli_format(const char *format, ...);
char *cli_join(const char *folder, const char *name);

/** The value of an environment variable as UTF-8, NULL when unset or empty. */
char *cli_environment(const char *name);

/** `path` made absolute against the working folder, with `/` separators and no `.` or `..` segment. */
char *cli_absolute_path(const char *path);

/**
 * Makes `path` and every missing folder above it, each by path under its parent; a link on the way is followed, since
 * the folder is the user's. Only for folders outside any root, and a root's own ancestors at `init`: a folder below a
 * root is made with cli_make_folders_below.
 */
naula_status cli_make_folders(const char *path, naula_refusal *refusal);

/** Makes every missing folder of `below` under `root` by the walk from the root, which refuses a link on the way. */
naula_status cli_make_folders_below(const char *root, const char *below, naula_refusal *refusal);

/** Whether `path` itself is a folder, never what a link there points at. */
int cli_is_folder(const char *path);
int cli_exists(const char *path);

/** The whole file, NUL-terminated past `length`; NAULA_NOT_FOUND when there is none. */
naula_status cli_read_file(const char *path, uint8_t **bytes, size_t *length, naula_refusal *refusal);

/** Answers git's output for `arguments` in `repository`, refusing a git that exits other than 0. */
naula_status cli_git(const char *repository, const char *const *arguments, naula_run_result *result,
                     naula_refusal *refusal);

/** Fills `refusal` as `git-failed` for a git that ran and exited other than 0, naming its first argument. */
void cli_git_refusal(const char *const *arguments, const naula_run_result *result, naula_refusal *refusal);

/** The bytes of the regular files directly in `folder`, never following a link; 0 for a folder that is missing. */
naula_status cli_folder_bytes(const char *folder, uint64_t *bytes);

/**
 * Refuses `path-linked` for a link or other reparse point at any folder of `relative` below `base`, its first folder
 * included, and `path-in-git` for a path through a `.git` folder: a link a repo tracks would carry a write out of it.
 * Ask it right before the write, since an earlier write can bring links of its own.
 */
naula_status cli_refuse_links_below(const char *base, const char *relative, naula_refusal *refusal);

/** Whether a clone url may be handed to git: https, and in a test build a local file url. */
int cli_clone_url_is_allowed(const char *url);

typedef struct cli_credentials cli_credentials;

/** Tokens from `git credential fill`, asked once per host in a run and wiped when the run ends. */
cli_credentials *cli_credentials_open(void);
const char *cli_credentials_token(cli_credentials *credentials, const char *provider, const char *host);
void cli_credentials_close(cli_credentials *credentials);

typedef struct cli_history cli_history;

/**
 * The command line's history reader: each home's branches are fetched once in a run, commits only, into a bare repo
 * under `<cache>/history`, and every question is answered there by git.
 */
naula_status cli_history_open(const char *cache_directory, naula_providers *providers, cli_credentials *credentials,
                              cli_history **history, naula_refusal *refusal);
const naula_history *cli_history_reader(cli_history *history);
void cli_history_close(cli_history *history);

/**
 * The id of the repo `url` serves, from its development branch where it has one and else `default_branch`, with
 * `stated_id` checked as the fork exception. `working_branch` answers the branch read, released by the caller.
 */
naula_status cli_history_id(cli_history *history, const char *url, const char *default_branch, const char *stated_id,
                            naula_id *id, char **working_branch, naula_refusal *refusal);

/**
 * Refuses a `commit` that is not of the repo `id_text` names: a branch of `url` must hold it, and its first-parent line
 * must start at the id, or pass it as the fork exception.
 */
naula_status cli_history_check_commit(cli_history *history, const char *url, const char *commit, const char *id_text,
                                      naula_refusal *refusal);

/** What the provider answers for `place`, its clone url checked. Release with naula_repository_release. */
naula_status cli_history_lookup(cli_history *history, const naula_place *place, naula_repository *repository,
                                naula_refusal *refusal);

/** Everything a command that reaches homes needs, opened from its arguments. */
typedef struct cli_context {
    const cli_arguments *arguments;
    char *cache_directory;
    naula_providers *providers;
    cli_credentials *credentials;
    cli_history *history;
    naula_session *session;
    char *questions[CLI_LIST_LIMIT];
    size_t question_count;
} cli_context;

naula_status cli_context_open(const cli_arguments *arguments, cli_context *context, naula_refusal *refusal);
void cli_context_close(cli_context *context);

/** Prints every acceptance the run asked for and did not get, each with the option that grants it. */
void cli_print_questions(const cli_context *context);

/**
 * The repo.json of the repo at `place` on `branch`, NAULA_NOT_FOUND when that branch holds none. Release with
 * naula_memory_release.
 */
naula_status cli_read_home_file(cli_context *context, const naula_place *place, const char *branch, uint8_t **bytes,
                                size_t *length, naula_refusal *refusal);

/**
 * A reference from the command line: an id with the `--at` places, or one place alone, whose id is derived from its
 * history and its repo.json. Release with cli_reference_release.
 */
naula_status cli_reference_from_arguments(cli_context *context, const char *text, const char *const *at,
                                          size_t at_count, naula_reference *reference, naula_refusal *refusal);
void cli_reference_release(naula_reference *reference);

/** The id of the repo at a provider place: its history's first-parent root, or the fork id its repo.json states. */
naula_status cli_derive_id(cli_context *context, const naula_place *place, char **id_text, naula_refusal *refusal);
naula_status cli_reference_copy(const naula_reference *from, naula_reference *to);

/** Resolves `reference` with the context's session; the resolution is the caller's to release. */
naula_status cli_resolve(cli_context *context, const naula_reference *reference, naula_resolution *resolution,
                         naula_refusal *refusal);

/** The verdicts of a resolution as lines on standard error, for a refused one. */
void cli_print_verdicts(const naula_resolution *resolution);

/** naula_reconcile for the checkout at `repository`, printing every change on standard output. */
naula_status cli_reconcile(cli_context *context, const char *repository, naula_reconcile_report *report,
                           naula_refusal *refusal);

/** The checkout a command acts on: `--repo`, else the working folder, absolute. */
char *cli_repository(const cli_arguments *arguments);

/** The local branch work happens on: `development` where it exists, else the one checked out. */
naula_status cli_working_branch(const char *repository, char **branch, naula_refusal *refusal);

/** Writes `file` canonically as `<repository>/repo.json`, through a temporary beside it; `changed` 0 for same bytes. */
naula_status cli_write_repo_file(const char *repository, const naula_repo_file *file, int *changed,
                                 naula_refusal *refusal);

/** The repo.json in the working tree of `repository`, format 2; NAULA_NOT_FOUND when there is none. */
naula_status cli_read_repo_file(const char *repository, naula_repo_file *file, naula_refusal *refusal);

/** The commit `url` holds at `refs/heads/<branch>`, or at its HEAD for a NULL `branch`, 40 or 64 hex. */
naula_status cli_remote_tip(const char *url, const char *branch, char tip[65], naula_refusal *refusal);

#if defined(NAULA_TEST_HOOKS)
/** Registers the test forge `NAULA_TEST_FORGE` describes, and points no provider at any real host. */
naula_status cli_test_forge_register(naula_providers *providers, naula_refusal *refusal);

/** The forge account and name whose repo the clone `url` reaches, 0 when none does. */
int cli_test_forge_repo_for_url(const char *url, char **account, char **name);
#endif

int cli_command_resolve(int count, char **values);
int cli_command_check(int count, char **values);
int cli_command_siblings(int count, char **values);
int cli_command_init(int count, char **values);
int cli_command_hold(int count, char **values);
int cli_command_release(int count, char **values);
int cli_command_sync(int count, char **values);
int cli_command_clone(int count, char **values);
int cli_command_new(int count, char **values);
int cli_command_adopt(int count, char **values);
int cli_command_add(int count, char **values);
int cli_command_retire(int count, char **values);

#endif
