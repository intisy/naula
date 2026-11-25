#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "naula/git.h"
#include "naula/run.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#ifndef NAULA_FIXTURE_ROOT
#error "NAULA_FIXTURE_ROOT names the folder make_repositories.cmake builds"
#endif

#define LINEAR NAULA_FIXTURE_ROOT "/linear"
#define MERGED NAULA_FIXTURE_ROOT "/merged"
#define WIDE NAULA_FIXTURE_ROOT "/wide"
#define REMOTE NAULA_FIXTURE_ROOT "/remote"
#define FETCHED NAULA_FIXTURE_ROOT "/fetched"
#define FLOOD_BYTES (1024 * 1024)

static const char *own_program;

typedef struct revision {
    char hex[65];
} revision;

static int resolve(const char *repository, const char *name, revision *found) {
    const char *arguments[] = {"git", "rev-parse", name, NULL};
    naula_run_result result;
    naula_refusal refusal;
    memset(found, 0, sizeof *found);
    if (naula_run(arguments, repository, &result, &refusal) != NAULA_OK || result.exit_code != 0) {
        fprintf(stderr, "cannot resolve %s in %s\n", name, repository);
        check_failures++;
        naula_run_result_release(&result);
        return 0;
    }
    size_t length = strcspn(result.output, "\r\n");
    snprintf(found->hex, sizeof found->hex, "%.*s", (int)length, result.output);
    naula_run_result_release(&result);
    return 1;
}

static void state_id(const revision *commit, char text[80]) {
    snprintf(text, 80, "git:%s", commit->hex);
}

static void expect_root(const char *repository, const char *ref, const char *expected_name, const char *label) {
    revision expected;
    char root[65] = "";
    naula_refusal refusal;
    CHECK(resolve(repository, expected_name, &expected));
    CHECK(naula_git_first_parent_root(repository, ref, root, &refusal) == NAULA_OK);
    check_text(root, expected.hex, label);
}

static void root_of_a_linear_history(void) {
    expect_root(LINEAR, "main", "main~2", "linear root");
}

static void root_follows_first_parents_past_a_merged_second_root(void) {
    revision second_root;
    revision first_root;
    CHECK(resolve(MERGED, "main^1~1", &first_root));
    CHECK(resolve(MERGED, "second", &second_root));
    CHECK(strcmp(first_root.hex, second_root.hex) != 0);
    expect_root(MERGED, "main", "main^1~1", "merged root");
}

static void root_of_a_sha256_repository(void) {
    char root[65] = "";
    naula_refusal refusal;
    CHECK(naula_git_first_parent_root(WIDE, "main", root, &refusal) == NAULA_OK);
    CHECK(strlen(root) == 64);
    expect_root(WIDE, "main", "main", "sha256 root");
}

static void fork_exception_accepts_its_first_own_commit(void) {
    revision own;
    char text[80];
    naula_id id;
    naula_refusal refusal;
    CHECK(resolve(LINEAR, "main~1", &own));
    state_id(&own, text);
    CHECK(naula_git_repository_id(LINEAR, "main", text, &id, &refusal) == NAULA_OK);
    check_text(id.commit, own.hex, "fork id");
    naula_id_release(&id);
}

static void fork_exception_refuses_the_root(void) {
    revision root;
    char text[80];
    naula_id id;
    naula_refusal refusal;
    CHECK(resolve(LINEAR, "main~2", &root));
    state_id(&root, text);
    CHECK(naula_git_repository_id(LINEAR, "main", text, &id, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "fork-id-is-root", "root refusal");
}

static void fork_exception_refuses_a_commit_off_the_line(void) {
    revision off_line;
    char text[80];
    naula_id id;
    naula_refusal refusal;
    CHECK(resolve(MERGED, "second", &off_line));
    state_id(&off_line, text);
    CHECK(naula_git_repository_id(MERGED, "main", text, &id, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "fork-id-not-on-line", "off line refusal");
}

static void id_kept_across_a_fast_forward(void) {
    naula_refusal refusal;
    CHECK(naula_git_check_id_kept(LINEAR, "previous", "main", NULL, &refusal) == NAULA_OK);
}

static void id_changed_by_a_rebase_across_the_root_is_refused(void) {
    naula_refusal refusal;
    CHECK(naula_git_check_id_kept(LINEAR, "main", "rewritten", NULL, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "id-changed", "changed refusal");
}

static void id_change_allowed_when_the_new_tip_states_the_fork_exception(void) {
    revision own;
    char text[80];
    naula_refusal refusal;
    CHECK(resolve(LINEAR, "rewritten", &own));
    state_id(&own, text);
    CHECK(naula_git_check_id_kept(LINEAR, "main", "rewritten", text, &refusal) == NAULA_OK);
}

static void parents_and_ancestry(void) {
    naula_refusal refusal;
    revision merge_tip;
    size_t count = 0;
    int ancestor = 0;
    CHECK(resolve(MERGED, "main", &merge_tip));
    CHECK(naula_git_parent_count(MERGED, merge_tip.hex, &count, &refusal) == NAULA_OK);
    CHECK(count == 2);
    CHECK(naula_git_parent_count(MERGED, "second", &count, &refusal) == NAULA_OK);
    CHECK(count == 0);
    CHECK(naula_git_is_ancestor(LINEAR, "previous", "main", &ancestor, &refusal) == NAULA_OK);
    CHECK(ancestor == 1);
    CHECK(naula_git_is_ancestor(LINEAR, "main", "previous", &ancestor, &refusal) == NAULA_OK);
    CHECK(ancestor == 0);
}

static void check_version_accepts_the_installed_git(void) {
    naula_refusal refusal;
    CHECK(naula_git_check_version(&refusal) == NAULA_OK);
}

static void file_url(const char *path, char *url, size_t capacity) {
    snprintf(url, capacity, "file://%s%s", path[0] == '/' ? "" : "/", path);
}

static void fetch_commits_with_tree_filter_reads_no_trees(void) {
    char url[1100];
    naula_refusal refusal;
    naula_run_result result;
    const char *listing[] = {"git", "rev-list", "--objects", "--missing=print", "FETCH_HEAD", NULL};
    file_url(REMOTE, url, sizeof url);
    CHECK(naula_git_fetch_commits(FETCHED, url, "main", &refusal) == NAULA_OK);
    CHECK(naula_run(listing, FETCHED, &result, &refusal) == NAULA_OK);
    CHECK(result.exit_code == 0);
    CHECK(strstr(result.output, "?") != NULL);
    naula_run_result_release(&result);
    char fetched_root[65] = "";
    revision remote_root;
    CHECK(resolve(REMOTE, "main~1", &remote_root));
    CHECK(naula_git_first_parent_root(FETCHED, "FETCH_HEAD", fetched_root, &refusal) == NAULA_OK);
    check_text(fetched_root, remote_root.hex, "fetched root");
}

static void run_passes_an_argument_with_spaces_and_quotes(void) {
    const char *arguments[] = {own_program, "--print-arguments", "a b", "he said \"hi\"", "trailing\\", "dir with space\\",
                               "", "plain", "two\\\\\"quotes", NULL};
    const char *expected = "[a b]\n[he said \"hi\"]\n[trailing\\]\n[dir with space\\]\n[]\n[plain]\n[two\\\\\"quotes]\n";
    naula_run_result result;
    naula_refusal refusal;
    CHECK(naula_run(arguments, NULL, &result, &refusal) == NAULA_OK);
    CHECK(result.exit_code == 0);
    check_text(result.output, expected, "echoed arguments");
    naula_run_result_release(&result);
}

static void run_collects_large_output_on_both_streams(void) {
    const char *arguments[] = {own_program, "--flood", NULL};
    naula_run_result result;
    naula_refusal refusal;
    CHECK(naula_run(arguments, NULL, &result, &refusal) == NAULA_OK);
    CHECK(result.exit_code == 3);
    CHECK(result.output_length == FLOOD_BYTES);
    CHECK(strlen(result.error) == FLOOD_BYTES);
    naula_run_result_release(&result);
}

static void run_says_a_missing_program_is_not_found(void) {
    const char *arguments[] = {"naula-no-such-program", NULL};
    naula_run_result result;
    naula_refusal refusal;
    CHECK(naula_run(arguments, NULL, &result, &refusal) == NAULA_NOT_FOUND);
    check_text(refusal.code, "program-not-found", "missing program");
    naula_run_result_release(&result);
}

static int print_arguments(int count, char **values) {
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    for (int index = 2; index < count; index++) {
        printf("[%s]\n", values[index]);
    }
    return 0;
}

static int flood(void) {
    char block[1024];
    memset(block, 'x', sizeof block);
    for (int written = 0; written < FLOOD_BYTES; written += (int)sizeof block) {
        fwrite(block, 1, sizeof block, stdout);
        fwrite(block, 1, sizeof block, stderr);
    }
    return 3;
}

int main(int count, char **values) {
    if (count > 1 && strcmp(values[1], "--print-arguments") == 0) {
        return print_arguments(count, values);
    }
    if (count > 1 && strcmp(values[1], "--flood") == 0) {
        return flood();
    }
    own_program = values[0];
    root_of_a_linear_history();
    root_follows_first_parents_past_a_merged_second_root();
    root_of_a_sha256_repository();
    fork_exception_accepts_its_first_own_commit();
    fork_exception_refuses_the_root();
    fork_exception_refuses_a_commit_off_the_line();
    id_kept_across_a_fast_forward();
    id_changed_by_a_rebase_across_the_root_is_refused();
    id_change_allowed_when_the_new_tip_states_the_fork_exception();
    parents_and_ancestry();
    check_version_accepts_the_installed_git();
    fetch_commits_with_tree_filter_reads_no_trees();
    run_passes_an_argument_with_spaces_and_quotes();
    run_collects_large_output_on_both_streams();
    run_says_a_missing_program_is_not_found();
    return check_summary("git_test");
}
