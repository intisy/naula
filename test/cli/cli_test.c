#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "disk.h"
#include "naula/json.h"
#include "naula/layout_apply.h"
#include "naula/memory.h"
#include "naula/run.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

#ifndef NAULA_PROGRAM
#error "NAULA_PROGRAM names the test build of the naula program"
#endif
#ifndef NAULA_TEST_SCRATCH
#error "NAULA_TEST_SCRATCH names the folder a test may write into"
#endif
#ifndef NAULA_CMAKE
#error "NAULA_CMAKE names cmake, which removes a case's folder"
#endif
#ifndef NAULA_STOPPED_MOVE
#error "NAULA_STOPPED_MOVE names the helper that leaves an unfinished move"
#endif

#define TEXT_SIZE 4096
#define FORGE_LIMIT 32
#define SNAPSHOT_SIZE (64 * 1024)

typedef struct forge_repo {
    unsigned number;
    char account[32];
    char name[64];
    char path[1024];
    char topics[128];
    int archived;
} forge_repo;

/** Everything one case builds under its own folder, and the fake forge it serves to the naula program. */
typedef struct test_case {
    char folder[1024];
    forge_repo repos[FORGE_LIMIT];
    size_t repo_count;
    unsigned clock;
    naula_run_result last;
    const char *environment[4];
} test_case;

static test_case here;

static void path_in(char *out, const char *relative) {
    snprintf(out, 1024, "%s/%s", here.folder, relative);
}

static int run_in(const char *directory, const char *const *arguments, const char *const *environment,
                  naula_run_result *result) {
    naula_refusal refusal;
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    if (naula_run(arguments, directory, environment, result, &refusal) != NAULA_OK) {
        fprintf(stderr, "cannot run %s: %s: %s\n", arguments[0], refusal.code, refusal.detail);
        check_failures++;
        return 0;
    }
    return 1;
}

static void git(const char *directory, const char *first, ...) {
    const char *arguments[24] = {"git", first};
    size_t count = 2;
    va_list more;
    va_start(more, first);
    for (const char *each = va_arg(more, const char *); each != NULL && count < 23; each = va_arg(more, const char *)) {
        arguments[count++] = each;
    }
    va_end(more);
    arguments[count] = NULL;
    char author_date[64];
    char committer_date[64];
    here.clock += 60;
    snprintf(author_date, sizeof author_date, "GIT_AUTHOR_DATE=%u +0000", 1700000000u + here.clock);
    snprintf(committer_date, sizeof committer_date, "GIT_COMMITTER_DATE=%u +0000", 1700000000u + here.clock);
    const char *environment[] = {author_date, committer_date, "GIT_AUTHOR_NAME=Fixture",
                                 "GIT_AUTHOR_EMAIL=fixture@example.invalid", "GIT_COMMITTER_NAME=Fixture",
                                 "GIT_COMMITTER_EMAIL=fixture@example.invalid", NULL};
    naula_run_result result;
    if (run_in(directory, arguments, environment, &result) && result.exit_code != 0) {
        fprintf(stderr, "git %s in %s exited %d: %s\n", first, directory == NULL ? "." : directory, result.exit_code,
                result.error);
        check_failures++;
    }
    naula_run_result_release(&result);
}

static void revision(const char *repository, const char *name, char hex[65]) {
    const char *arguments[] = {"git", "rev-parse", name, NULL};
    naula_run_result result;
    hex[0] = '\0';
    if (run_in(repository, arguments, NULL, &result) && result.exit_code == 0) {
        snprintf(hex, 65, "%.*s", (int)strcspn(result.output, "\r\n"), result.output);
    }
    naula_run_result_release(&result);
}

static void root_commit(const char *repository, char hex[65]) {
    const char *arguments[] = {"git", "rev-list", "--first-parent", "--max-parents=0", "HEAD", NULL};
    naula_run_result result;
    hex[0] = '\0';
    if (run_in(repository, arguments, NULL, &result) && result.exit_code == 0) {
        snprintf(hex, 65, "%.*s", (int)strcspn(result.output, "\r\n"), result.output);
    }
    naula_run_result_release(&result);
}

static void write_text(const char *path, const char *text) {
    char folder[1024];
    snprintf(folder, sizeof folder, "%s", path);
    char *slash = strrchr(folder, '/');
    if (slash != NULL) {
        *slash = '\0';
        const char *arguments[] = {NAULA_CMAKE, "-E", "make_directory", folder, NULL};
        naula_run_result result;
        if (run_in(NULL, arguments, NULL, &result)) {
            naula_run_result_release(&result);
        }
    }
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file != NULL) {
        fputs(text, file);
        fclose(file);
    }
}

static int exists(const char *path) {
    disk_kind kind = DISK_MISSING;
    unsigned long error = 0;
    return disk_kind_of(path, &kind, &error) == NAULA_OK && kind != DISK_MISSING;
}

static int is_link(const char *path) {
    disk_kind kind = DISK_MISSING;
    unsigned long error = 0;
    return disk_kind_of(path, &kind, &error) == NAULA_OK && kind == DISK_LINK;
}

static size_t case_number;

/** Each case works in `<scratch>/c<n>`: a short folder, since git paths inside nested checkouts near MAX_PATH. */
static void start_case(void) {
    memset(&here, 0, sizeof here);
    snprintf(here.folder, sizeof here.folder, "%s/c%zu", NAULA_TEST_SCRATCH, case_number);
    const char *remove[] = {NAULA_CMAKE, "-E", "remove_directory", here.folder, NULL};
    const char *make[] = {NAULA_CMAKE, "-E", "make_directory", here.folder, NULL};
    naula_run_result result;
    if (run_in(NULL, remove, NULL, &result)) {
        naula_run_result_release(&result);
    }
    if (run_in(NULL, make, NULL, &result)) {
        naula_run_result_release(&result);
    }
}

/** A repo at `<case>/<folder>` on branch main, committed once with every file `files` names, NULL-terminated pairs. */
static void make_repo(const char *folder, const char *const *files) {
    char path[1024];
    path_in(path, folder);
    const char *make[] = {NAULA_CMAKE, "-E", "make_directory", path, NULL};
    naula_run_result result;
    if (run_in(NULL, make, NULL, &result)) {
        naula_run_result_release(&result);
    }
    git(path, "init", "--quiet", "-b", "main", NULL);
    git(path, "config", "uploadpack.allowFilter", "true", NULL);
    for (size_t index = 0; files != NULL && files[index] != NULL; index += 2) {
        char file[1024];
        snprintf(file, sizeof file, "%s/%s", path, files[index]);
        write_text(file, files[index + 1]);
    }
    git(path, "add", "-A", NULL);
    git(path, "commit", "--quiet", "-m", "first", NULL);
}

static void commit_file(const char *folder, const char *name, const char *text) {
    char path[1024];
    char file[1024];
    path_in(path, folder);
    snprintf(file, sizeof file, "%s/%s", path, name);
    write_text(file, text);
    git(path, "add", "-A", NULL);
    git(path, "commit", "--quiet", "-m", name, NULL);
}

static void serve(unsigned number, const char *account, const char *name, const char *folder, const char *topics) {
    forge_repo *repo = &here.repos[here.repo_count++];
    repo->number = number;
    snprintf(repo->account, sizeof repo->account, "%s", account);
    snprintf(repo->name, sizeof repo->name, "%s", name);
    path_in(repo->path, folder);
    snprintf(repo->topics, sizeof repo->topics, "%s", topics == NULL ? "" : topics);
}

static void write_forge(void) {
    static char text[TEXT_SIZE * 4];
    size_t length = (size_t)snprintf(text, sizeof text, "{\"repos\":[");
    for (size_t index = 0; index < here.repo_count; index++) {
        const forge_repo *repo = &here.repos[index];
        length += (size_t)snprintf(text + length, sizeof text - length,
                                   "%s{\"number\":%u,\"account\":\"%s\",\"name\":\"%s\",\"path\":\"%s\",\"archived\":%s,"
                                   "\"topics\":[%s]}",
                                   index == 0 ? "" : ",", repo->number, repo->account, repo->name, repo->path,
                                   repo->archived ? "true" : "false", repo->topics);
    }
    snprintf(text + length, sizeof text - length, "]}");
    char path[1024];
    path_in(path, "forge.json");
    write_text(path, text);
}

/** Runs the naula program with the case's forge and cache; the answer stays in `here.last` until the next run. */
static int naula(const char *first, ...) {
    const char *arguments[24] = {NAULA_PROGRAM, first};
    size_t count = 2;
    va_list more;
    va_start(more, first);
    for (const char *each = va_arg(more, const char *); each != NULL && count < 23; each = va_arg(more, const char *)) {
        arguments[count++] = each;
    }
    va_end(more);
    arguments[count] = NULL;
    write_forge();
    char forge[1100];
    char cache[1100];
    char banks[1100];
    snprintf(forge, sizeof forge, "NAULA_TEST_FORGE=%s/forge.json", here.folder);
    snprintf(cache, sizeof cache, "NAULA_CACHE=%s/cache", here.folder);
    snprintf(banks, sizeof banks, "NAULA_MEMORY_BANKS=%s/banks", here.folder);
    const char *environment[8] = {forge, cache, banks, NULL};
    for (size_t index = 0; index < 4 && here.environment[index] != NULL; index++) {
        environment[3 + index] = here.environment[index];
        environment[4 + index] = NULL;
    }
    naula_run_result_release(&here.last);
    if (!run_in(NULL, arguments, environment, &here.last)) {
        return -1;
    }
    return here.last.exit_code;
}

static void expect_exit(int code, int expected, const char *label) {
    if (code != expected) {
        fprintf(stderr, "%s: naula exited %d, expected %d\nstdout:\n%s\nstderr:\n%s\n", label, code, expected,
                here.last.output == NULL ? "" : here.last.output, here.last.error == NULL ? "" : here.last.error);
        check_failures++;
    }
}

static void expect_output(const char *text, int wanted, const char *label) {
    int found = (here.last.output != NULL && strstr(here.last.output, text) != NULL) ||
                (here.last.error != NULL && strstr(here.last.error, text) != NULL);
    if (found != wanted) {
        fprintf(stderr, "%s: the output %s \"%s\"\nstdout:\n%s\nstderr:\n%s\n", label, wanted ? "lacks" : "holds", text,
                here.last.output == NULL ? "" : here.last.output, here.last.error == NULL ? "" : here.last.error);
        check_failures++;
    }
}

static void app_file(char *out, const char *name, const char *account, const char *extra) {
    snprintf(out, TEXT_SIZE,
             "{\"format\":2,\"name\":\"%s\",\"importName\":\"%s\",\"homes\":[{\"provider\":\"forge\",\"account\":\"%s\"}]%s}\n",
             name, name, account, extra == NULL ? "" : extra);
}

static void check_refuses_a_rebased_root(void) {
    start_case();
    char file[TEXT_SIZE];
    app_file(file, "app", "alpha", NULL);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    commit_file("app", "second.txt", "second\n");
    char repo[1024];
    char first[65];
    char previous[65];
    path_in(repo, "app");
    revision(repo, "HEAD~1", first);
    revision(repo, "HEAD", previous);
    expect_exit(naula("check", "--repo", repo, "--previous", first, NULL), 0, "a fast-forward keeps the id");
    git(repo, "checkout", "--quiet", "--orphan", "rewritten", NULL);
    git(repo, "commit", "--quiet", "-m", "rewritten", NULL);
    expect_exit(naula("check", "--repo", repo, "--previous", previous, NULL), 1, "a rebased root");
    expect_output("id-changed", 1, "a rebased root");
}

static void check_warns_on_a_narrow_at_and_prints_the_line(void) {
    start_case();
    const char *library[] = {"repo.json",
                             "{\"format\":2,\"name\":\"lib\",\"importName\":\"lib\",\"homes\":[{\"provider\":\"forge\","
                             "\"account\":\"alpha\"},{\"provider\":\"forge\",\"account\":\"beta\"}]}\n",
                             NULL};
    make_repo("lib", library);
    serve(201, "alpha", "lib", "lib", NULL);
    serve(202, "beta", "lib", "lib", NULL);
    char lib[1024];
    char root[65];
    char head[65];
    path_in(lib, "lib");
    root_commit(lib, root);
    revision(lib, "HEAD", head);
    char dependency[512];
    char file[TEXT_SIZE];
    snprintf(dependency, sizeof dependency,
             ",\"dependencies\":[{\"id\":\"git:%s\",\"at\":[\"forge:201\"],\"importName\":\"lib\",\"commit\":\"%s\"}]",
             root, head);
    app_file(file, "app", "alpha", dependency);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    char repo[1024];
    path_in(repo, "app");
    expect_exit(naula("check", "--repo", repo, NULL), 0, "a narrow at is a warning");
    expect_output("warning at-narrow", 1, "a narrow at");
    expect_output("\"at\": [\"forge:201\", \"forge:202\"]", 1, "the line to commit");
}

static void check_warns_on_a_url_dependency(void) {
    start_case();
    char file[TEXT_SIZE];
    app_file(file, "app", "alpha",
             ",\"dependencies\":[{\"url\":\"https://example.org/x/lib.git\",\"importName\":\"lib\","
             "\"commit\":\"0123456789abcdef0123456789abcdef01234567\"}]");
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    char repo[1024];
    path_in(repo, "app");
    expect_exit(naula("check", "--repo", repo, NULL), 0, "a url dependency is a warning");
    expect_output("warning url-dependency", 1, "a url dependency");
}

static void check_refuses_an_owner_repo_literal_outside_exempt_places(void) {
    start_case();
    char file[TEXT_SIZE];
    app_file(file, "app", "alpha", NULL);
    const char *exempt[] = {"repo.json", file,
                            ".github/workflows/test.yml",
                            "jobs:\n  test:\n    uses: alpha/workflows/.github/workflows/test.yml@main\n",
                            "docs/specs/2026-01-01-history.md", "It once lived at alpha/old-name.\n", NULL};
    make_repo("app", exempt);
    char repo[1024];
    path_in(repo, "app");
    expect_exit(naula("check", "--repo", repo, NULL), 0, "exempt places may name a repo");
    commit_file("app", "README.md", "Build it beside alpha/other-tool.\n");
    expect_exit(naula("check", "--repo", repo, NULL), 1, "a literal outside the exempt places");
    expect_output("owner-repo-literal", 1, "the literal");
    expect_output("README.md:1", 1, "where the literal is");
    expect_output(".github/workflows", 0, "the workflow is exempt");
}

typedef struct proxy_entry {
    const char *repo;
    const char *revision;
    const char *path;
} proxy_entry;

static void siblings_clones_nested_paths(void) {
    start_case();
    static const char *const names[] = {"typescript", "core", "standard", "conformance", "forebay-library", "library",
                                        "c"};
    char roots[7][65];
    for (size_t index = 0; index < 7; index++) {
        char file[TEXT_SIZE];
        app_file(file, names[index], "alpha", NULL);
        const char *files[] = {"repo.json", file, NULL};
        make_repo(names[index], files);
        commit_file(names[index], "second.txt", "second\n");
        commit_file(names[index], "third.txt", "third\n");
        serve(400 + (unsigned)index, "alpha", names[index], names[index], NULL);
        char path[1024];
        path_in(path, names[index]);
        root_commit(path, roots[index]);
    }
    static const proxy_entry entries[] = {
        {"typescript", "HEAD", NULL},
        {"core", "HEAD~1", "siblings/typescript/siblings/core"},
        {"standard", "HEAD", "siblings/typescript/siblings/standard"},
        {"conformance", "HEAD~2", "siblings/typescript/siblings/conformance"},
        {"forebay-library", "HEAD", NULL},
        {"library", "HEAD~1", "siblings/forebay-library/siblings/library"},
        {"core", "HEAD", "siblings/forebay-library/siblings/core"},
        {"standard", "HEAD", "siblings/forebay-library/siblings/standard"},
        {"c", "HEAD", "siblings/forebay-library/siblings/c"},
        {"library", "HEAD", NULL},
        {"core", "HEAD", "siblings/library/siblings/core"},
        {"standard", "HEAD~1", "siblings/library/siblings/standard"},
        {"c", "HEAD", "siblings/library/siblings/c"},
    };
    static char dependencies[TEXT_SIZE * 2];
    char commits[13][65];
    size_t length = (size_t)snprintf(dependencies, sizeof dependencies, ",\"dependencies\":[");
    for (size_t index = 0; index < 13; index++) {
        size_t which = 0;
        while (strcmp(names[which], entries[index].repo) != 0) {
            which++;
        }
        char path[1024];
        path_in(path, names[which]);
        revision(path, entries[index].revision, commits[index]);
        char member[160] = "";
        if (entries[index].path != NULL) {
            snprintf(member, sizeof member, ",\"path\":\"%s\"", entries[index].path);
        }
        length += (size_t)snprintf(dependencies + length, sizeof dependencies - length,
                                   "%s{\"id\":\"git:%s\",\"at\":[\"forge:%u\"],\"importName\":\"%s\",\"commit\":\"%s\"%s}",
                                   index == 0 ? "" : ",", roots[which], 400 + (unsigned)which, names[which],
                                   commits[index], member);
    }
    snprintf(dependencies + length, sizeof dependencies - length, "]");
    static char file[TEXT_SIZE * 2];
    snprintf(file, sizeof file,
             "{\"format\":2,\"name\":\"claude-code-proxy\",\"importName\":\"claude-code-proxy\",\"homes\":[{\"provider\":"
             "\"forge\",\"account\":\"alpha\"}]%s}\n",
             dependencies);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("proxy", files);
    char proxy[1024];
    path_in(proxy, "proxy");
    expect_exit(naula("siblings", "--repo", proxy, NULL), 0, "thirteen siblings");
    for (size_t index = 0; index < 13; index++) {
        char relative[256];
        char target[1100];
        char head[65];
        if (entries[index].path == NULL) {
            snprintf(relative, sizeof relative, "siblings/%s", entries[index].repo);
        } else {
            snprintf(relative, sizeof relative, "%s", entries[index].path);
        }
        snprintf(target, sizeof target, "%s/%s", proxy, relative);
        CHECK(!is_link(target));
        revision(target, "HEAD", head);
        check_text(head, commits[index], relative);
    }
    expect_exit(naula("siblings", "--repo", proxy, NULL), 0, "a second run keeps them");
    expect_output("kept siblings/library/siblings/c", 1, "kept on a second run");
}

static void siblings_refuses_a_copy(void) {
    start_case();
    char original_file[TEXT_SIZE];
    char copy_file[TEXT_SIZE];
    app_file(original_file, "lib", "alpha", NULL);
    app_file(copy_file, "lib", "beta", NULL);
    const char *original_files[] = {"repo.json", original_file, NULL};
    const char *copy_files[] = {"repo.json", copy_file, "copy.txt", "a copy with its own history\n", NULL};
    make_repo("original", original_files);
    make_repo("copy", copy_files);
    serve(301, "alpha", "lib", "original", NULL);
    serve(302, "beta", "lib", "copy", NULL);
    char path[1024];
    char root[65];
    char head[65];
    path_in(path, "original");
    root_commit(path, root);
    path_in(path, "copy");
    revision(path, "HEAD", head);
    char dependency[512];
    char file[TEXT_SIZE];
    snprintf(dependency, sizeof dependency,
             ",\"dependencies\":[{\"id\":\"git:%s\",\"at\":[\"forge:302\"],\"importName\":\"lib\",\"commit\":\"%s\"}]",
             root, head);
    app_file(file, "app", "gamma", dependency);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    char app[1024];
    char siblings[1100];
    path_in(app, "app");
    snprintf(siblings, sizeof siblings, "%s/siblings", app);
    expect_exit(naula("siblings", "--repo", app, NULL), 1, "a copy at another root");
    expect_output("not-in-history", 1, "why the copy was refused");
    CHECK(!exists(siblings));
}

/** The orgs every layout case starts from: alpha the top with docs and a tool, beta beside it, gamma held by none. */
static void make_orgs(char alpha_root[65], char beta_app_root[65], char gamma_root[65]) {
    const char *alpha_docs[] = {
        "repo.json",
        "{\"format\":2,\"name\":\"docs\",\"importName\":\"docs\",\"homes\":[{\"provider\":\"forge\",\"account\":\"alpha\"}],"
        "\"org\":{\"name\":\"alpha\",\"accounts\":[{\"provider\":\"forge\",\"account\":\"alpha\"}],\"rules\":\"RULES.md\","
        "\"projects\":{\"tools\":\"Programs.\"}}}\n",
        "RULES.md", "alpha rules\n", NULL};
    make_repo("alpha-docs", alpha_docs);
    char path[1024];
    path_in(path, "alpha-docs");
    root_commit(path, alpha_root);
    char tool[TEXT_SIZE];
    app_file(tool, "tool", "alpha", ",\"topics\":[\"tools\"]");
    const char *tool_files[] = {"repo.json", tool, NULL};
    make_repo("alpha-tool", tool_files);
    char beta_docs[TEXT_SIZE];
    snprintf(beta_docs, sizeof beta_docs,
             "{\"format\":2,\"name\":\"docs\",\"importName\":\"docs\",\"homes\":[{\"provider\":\"forge\",\"account\":"
             "\"beta\"}],\"org\":{\"name\":\"beta\",\"parent\":{\"id\":\"git:%s\",\"at\":[\"forge:101\"]},"
             "\"accounts\":[{\"provider\":\"forge\",\"account\":\"beta\"}],\"rules\":\"RULES.md\"}}\n",
             alpha_root);
    const char *beta_docs_files[] = {"repo.json", beta_docs, "RULES.md", "beta rules\n", NULL};
    make_repo("beta-docs", beta_docs_files);
    char app[TEXT_SIZE];
    app_file(app, "app", "beta", NULL);
    const char *app_files[] = {"repo.json", app, NULL};
    make_repo("beta-app", app_files);
    path_in(path, "beta-app");
    root_commit(path, beta_app_root);
    char thing[TEXT_SIZE];
    app_file(thing, "thing", "gamma", NULL);
    const char *thing_files[] = {"repo.json", thing, NULL};
    make_repo("gamma-thing", thing_files);
    path_in(path, "gamma-thing");
    root_commit(path, gamma_root);
    serve(101, "alpha", "docs", "alpha-docs", NULL);
    serve(102, "alpha", "tool", "alpha-tool", NULL);
    serve(201, "beta", "docs", "beta-docs", NULL);
    serve(202, "beta", "app", "beta-app", NULL);
    serve(301, "gamma", "thing", "gamma-thing", NULL);
}

static void init_root(char *root) {
    path_in(root, "root");
    expect_exit(naula("init", root, "forge:101", NULL), 0, "init");
}

static void expect_path(const char *root, const char *relative, int wanted_link, const char *label) {
    char path[1100];
    snprintf(path, sizeof path, "%s/%s", root, relative);
    if (!exists(path) || (wanted_link && !is_link(path))) {
        fprintf(stderr, "%s: %s is %s\n", label, path, exists(path) ? "not a link" : "missing");
        check_failures++;
    }
}

static void expect_absent(const char *root, const char *relative, const char *label) {
    char path[1100];
    snprintf(path, sizeof path, "%s/%s", root, relative);
    if (exists(path)) {
        fprintf(stderr, "%s: %s is still there\n", label, path);
        check_failures++;
    }
}

static void checkout_of(const char *hex, char *relative, const char *org) {
    snprintf(relative, 128, "%s%srepos/%.12s", org == NULL ? "" : org, org == NULL ? "" : "/", hex);
}

/** Every path under `folder` with its link target or file bytes, never entering a link, `.git` or a journal. */
static void snapshot(const char *folder, char *out, size_t *length) {
    char **names = NULL;
    size_t count = 0;
    unsigned long error = 0;
    if (disk_names(folder, &names, &count, &error) != NAULA_OK) {
        return;
    }
    for (size_t index = 0; index < count; index++) {
        if (strcmp(names[index], ".git") == 0 || strcmp(names[index], ".naula-journal") == 0) {
            continue;
        }
        char path[1100];
        snprintf(path, sizeof path, "%s/%s", folder, names[index]);
        disk_kind kind = DISK_MISSING;
        disk_kind_of(path, &kind, &error);
        char *target = NULL;
        uint8_t *bytes = NULL;
        size_t size = 0;
        if (kind == DISK_LINK && disk_link_target(path, &target, &error) == NAULA_OK) {
            *length += (size_t)snprintf(out + *length, SNAPSHOT_SIZE - *length, "%s -> %s\n", path, target);
        } else if (kind == DISK_FILE && disk_file_read(path, &bytes, &size, &error) == NAULA_OK) {
            *length += (size_t)snprintf(out + *length, SNAPSHOT_SIZE - *length, "%s = %.*s\n", path, (int)size,
                                        (const char *)bytes);
        } else if (kind == DISK_FOLDER) {
            *length += (size_t)snprintf(out + *length, SNAPSHOT_SIZE - *length, "%s/\n", path);
            snapshot(path, out, length);
        }
        naula_memory_release(target);
        naula_memory_release(bytes);
    }
    disk_names_release(names, count);
}

static void sync_is_a_no_op_on_a_laid_out_root(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char checkout[128];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    checkout_of(alpha, checkout, NULL);
    expect_path(root, checkout, 0, "the docs checkout");
    expect_path(root, "docs", 1, "the docs link");
    expect_path(root, "tools/tool", 1, "the tool link");
    expect_path(root, "CLAUDE.md", 0, "the rules import");
    static char before[SNAPSHOT_SIZE];
    static char after[SNAPSHOT_SIZE];
    size_t before_length = 0;
    size_t after_length = 0;
    snapshot(root, before, &before_length);
    expect_exit(naula("sync", "--root", root, NULL), 0, "a second sync");
    expect_output("clone ", 0, "a second sync clones nothing");
    expect_output("move ", 0, "a second sync moves nothing");
    expect_output("unlink ", 0, "a second sync removes no link");
    snapshot(root, after, &after_length);
    check_text(after, before, "the root after a second sync");
}

static void hold_adds_an_org_and_lays_it_out(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char checkout[128];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    expect_absent(root, "beta", "before hold");
    expect_exit(naula("hold", "forge:201", "--root", root, NULL), 0, "hold beta");
    checkout_of(beta_app, checkout, "beta");
    expect_path(root, checkout, 0, "beta's app checkout");
    expect_path(root, "beta/app", 1, "beta's app link");
    expect_path(root, "beta/docs", 1, "beta's docs link");
    expect_path(root, "beta/CLAUDE.md", 0, "beta's rules import");
}

static void release_removes_an_orgs_links_but_never_a_dirty_clone(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char checkout[128];
    char dirty[1200];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    expect_exit(naula("hold", "forge:201", "--root", root, NULL), 0, "hold beta");
    checkout_of(beta_app, checkout, "beta");
    snprintf(dirty, sizeof dirty, "%s/%s/work-in-progress.txt", root, checkout);
    write_text(dirty, "not committed anywhere\n");
    expect_exit(naula("release", "beta", "--root", root, NULL), 0, "release beta");
    expect_output("naula-test: root lock held as the rules are removed", 1, "the root stays held for the whole release");
    expect_absent(root, "beta/app", "beta's app link");
    expect_absent(root, "beta/docs", "beta's docs link");
    expect_absent(root, "beta/CLAUDE.md", "beta's generated rules import");
    CHECK(exists(dirty));
    expect_path(root, "docs", 1, "alpha's docs link stays");
}

static void clone_names_the_folder_by_twelve_hex(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char checkout[128];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    expect_exit(naula("clone", "forge:301", "--root", root, NULL), 0, "clone gamma's thing");
    checkout_of(gamma, checkout, NULL);
    expect_path(root, checkout, 0, "the checkout named by twelve hex");
    expect_path(root, "thing", 1, "the link by name");
    char path[1200];
    char expected[80];
    snprintf(path, sizeof path, "%s/%s", root, checkout);
    snprintf(expected, sizeof expected, "naula::git:%s", gamma);
    const char *arguments[] = {"git", "remote", "get-url", "origin", NULL};
    naula_run_result result;
    if (run_in(path, arguments, NULL, &result)) {
        result.output[strcspn(result.output, "\r\n")] = '\0';
        check_text(result.output, expected, "the remote names the id");
        naula_run_result_release(&result);
    }
    expect_exit(naula("sync", "--root", root, NULL), 0, "a sync after the clone");
    expect_output("clone ", 0, "the kept repo is not cloned again");
}

static void make_folder(const char *path) {
    const char *make[] = {NAULA_CMAKE, "-E", "make_directory", path, NULL};
    naula_run_result result;
    if (run_in(NULL, make, NULL, &result)) {
        naula_run_result_release(&result);
    }
}

static void make_link(const char *folder, const char *name, const char *target) {
    unsigned long error = 0;
    CHECK(disk_link_make(folder, name, target, DISK_FOR_PLAN_STEP, &error) == NAULA_OK);
}

static void remove_file(const char *path) {
    const char *remove[] = {NAULA_CMAKE, "-E", "rm", "-f", path, NULL};
    naula_run_result result;
    if (run_in(NULL, remove, NULL, &result)) {
        naula_run_result_release(&result);
    }
}

/** A repo `app` of account gamma depending on `lib` of alpha, served as forge:<number>, its dependency extra appended. */
static void make_app_with_lib(unsigned number, const char *dependency_extra, char *app) {
    char lib_file[TEXT_SIZE];
    app_file(lib_file, "lib", "alpha", NULL);
    const char *lib_files[] = {"repo.json", lib_file, "payload.txt", "written outside\n", NULL};
    make_repo("lib", lib_files);
    serve(number, "alpha", "lib", "lib", NULL);
    char lib[1024];
    char root[65];
    char head[65];
    path_in(lib, "lib");
    root_commit(lib, root);
    revision(lib, "HEAD", head);
    char dependency[1024];
    snprintf(dependency, sizeof dependency,
             ",\"dependencies\":[{\"id\":\"git:%s\",\"at\":[\"forge:%u\"],\"importName\":\"lib\",\"commit\":\"%s\"%s}]",
             root, number, head, dependency_extra == NULL ? "" : dependency_extra);
    char file[TEXT_SIZE];
    app_file(file, "app", "gamma", dependency);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    path_in(app, "app");
}

static void siblings_refuses_a_link_in_its_own_siblings_folder(void) {
    start_case();
    char app[1024];
    char escaped[1024];
    char link[1100];
    char written[1200];
    make_app_with_lib(711, NULL, app);
    path_in(escaped, "escaped");
    make_folder(escaped);
    snprintf(link, sizeof link, "%s/siblings", app);
    make_link(app, "siblings", escaped);
    expect_exit(naula("siblings", "--repo", app, NULL), 1, "a linked siblings folder");
    expect_output("path-linked", 1, "why the link was refused");
    snprintf(written, sizeof written, "%s/lib", escaped);
    CHECK(!exists(written));
}

#if !defined(_WIN32)
static void siblings_refuses_a_link_a_dependency_tracks(void) {
    start_case();
    char escaped[1024];
    path_in(escaped, "escaped");
    make_folder(escaped);
    char outer_file[TEXT_SIZE];
    app_file(outer_file, "outer", "alpha", NULL);
    const char *outer_files[] = {"repo.json", outer_file, NULL};
    make_repo("outer", outer_files);
    char outer[1024];
    char link[1100];
    path_in(outer, "outer");
    snprintf(link, sizeof link, "%s/inner", outer);
    CHECK(symlink(escaped, link) == 0);
    git(outer, "add", "-A", NULL);
    git(outer, "commit", "--quiet", "-m", "link", NULL);
    char inner_file[TEXT_SIZE];
    app_file(inner_file, "lib", "alpha", NULL);
    const char *inner_files[] = {"repo.json", inner_file, "payload.txt", "written outside\n", NULL};
    make_repo("lib", inner_files);
    serve(701, "alpha", "outer", "outer", NULL);
    serve(702, "alpha", "lib", "lib", NULL);
    char lib[1024];
    char outer_root[65];
    char outer_head[65];
    char lib_root[65];
    char lib_head[65];
    path_in(lib, "lib");
    root_commit(outer, outer_root);
    revision(outer, "HEAD", outer_head);
    root_commit(lib, lib_root);
    revision(lib, "HEAD", lib_head);
    char dependencies[2048];
    snprintf(dependencies, sizeof dependencies,
             ",\"dependencies\":[{\"id\":\"git:%s\",\"at\":[\"forge:701\"],\"importName\":\"outer\",\"commit\":\"%s\"},"
             "{\"id\":\"git:%s\",\"at\":[\"forge:702\"],\"importName\":\"lib\",\"commit\":\"%s\","
             "\"path\":\"siblings/outer/inner/x\"}]",
             outer_root, outer_head, lib_root, lib_head);
    char file[TEXT_SIZE];
    app_file(file, "app", "gamma", dependencies);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    char app[1024];
    char written[1200];
    path_in(app, "app");
    expect_exit(naula("siblings", "--repo", app, NULL), 1, "a link the outer dependency tracks");
    expect_output("path-linked", 1, "why the nested clone was refused");
    snprintf(written, sizeof written, "%s/x", escaped);
    CHECK(!exists(written));
}
#else
static void siblings_refuses_a_link_a_dependency_tracks(void) {
    fprintf(stderr, "git for Windows checks a tracked link out as a file, so no clone can follow one there\n");
}
#endif

static void siblings_compares_paths_ignoring_case(void) {
    start_case();
    char lib_file[TEXT_SIZE];
    app_file(lib_file, "lib", "alpha", NULL);
    const char *lib_files[] = {"repo.json", lib_file, NULL};
    make_repo("lib", lib_files);
    serve(721, "alpha", "lib", "lib", NULL);
    char lib[1024];
    char root[65];
    char head[65];
    path_in(lib, "lib");
    root_commit(lib, root);
    revision(lib, "HEAD", head);
    char dependencies[1024];
    snprintf(dependencies, sizeof dependencies,
             ",\"dependencies\":[{\"id\":\"git:%s\",\"at\":[\"forge:721\"],\"importName\":\"lib\",\"commit\":\"%s\","
             "\"path\":\"siblings/Lib\"},{\"id\":\"git:%s\",\"at\":[\"forge:721\"],\"importName\":\"other\","
             "\"commit\":\"%s\",\"path\":\"siblings/lib\"}]",
             root, head, root, head);
    char file[TEXT_SIZE];
    app_file(file, "app", "gamma", dependencies);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    char app[1024];
    path_in(app, "app");
    expect_exit(naula("siblings", "--repo", app, NULL), 1, "two paths one case apart");
    expect_output("sibling-path-repeated", 1, "the repeated path");
}

/**
 * `lib`'s pinned commit exists only on a fork of it, which another dependency names and the run fetches first, so it
 * sits in the shared history store; a branch of lib's own primary must hold it, and the refusal comes before any clone.
 */
static void siblings_checks_every_commit_on_its_own_primary_before_cloning(void) {
    start_case();
    char lib_file[TEXT_SIZE];
    app_file(lib_file, "lib", "alpha", NULL);
    const char *lib_files[] = {"repo.json", lib_file, NULL};
    make_repo("lib", lib_files);
    char lib[1024];
    char fork[1024];
    char url[1100];
    char lib_root[65];
    char own[65];
    char fork_head[65];
    path_in(lib, "lib");
    path_in(fork, "fork");
    root_commit(lib, lib_root);
    snprintf(url, sizeof url, "file://%s", lib);
    git(NULL, "clone", "--quiet", url, fork, NULL);
    commit_file("fork", "own.txt", "the fork's own work\n");
    revision(fork, "HEAD", own);
    char fork_file[TEXT_SIZE];
    char stated[100];
    snprintf(stated, sizeof stated, ",\"id\":\"git:%s\"", own);
    app_file(fork_file, "fork", "beta", stated);
    commit_file("fork", "repo.json", fork_file);
    revision(fork, "HEAD", fork_head);
    serve(731, "alpha", "lib", "lib", NULL);
    serve(732, "beta", "fork", "fork", NULL);
    char dependencies[1024];
    snprintf(dependencies, sizeof dependencies,
             ",\"dependencies\":[{\"id\":\"git:%s\",\"at\":[\"forge:732\"],\"importName\":\"fork\",\"commit\":\"%s\","
             "\"path\":\"siblings/a-fork\"},{\"id\":\"git:%s\",\"at\":[\"forge:731\"],\"importName\":\"lib\","
             "\"commit\":\"%s\"}]",
             own, fork_head, lib_root, own);
    char file[TEXT_SIZE];
    app_file(file, "app", "gamma", dependencies);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    char app[1024];
    char siblings[1100];
    path_in(app, "app");
    snprintf(siblings, sizeof siblings, "%s/siblings", app);
    expect_exit(naula("siblings", "--repo", app, NULL), 1, "a commit only a fork holds");
    expect_output("commit-not-on-home", 1, "why the commit was refused");
    CHECK(!exists(siblings));
}

/** Whether `folder` holds a file git writes while a pack arrives, which only a fetch cut short leaves. */
static int holds_a_temporary_pack(const char *folder) {
    char **names = NULL;
    size_t count = 0;
    unsigned long error = 0;
    int found = 0;
    if (disk_names(folder, &names, &count, &error) == NAULA_OK) {
        for (size_t index = 0; index < count; index++) {
            if (strncmp(names[index], "tmp_pack_", 9) == 0 || strncmp(names[index], "tmp_idx_", 8) == 0) {
                fprintf(stderr, "left in %s: %s\n", folder, names[index]);
                found = 1;
            }
        }
        disk_names_release(names, count);
    }
    return found;
}

static void siblings_stops_a_fetch_past_its_byte_limit(void) {
    start_case();
    char app[1024];
    make_app_with_lib(741, NULL, app);
    here.environment[0] = "NAULA_FETCH_BYTE_LIMIT=1";
    expect_exit(naula("siblings", "--repo", app, NULL), 1, "a fetch past its byte limit");
    expect_output("fetch-too-large", 1, "the byte limit");
    char cache[1100];
    char packs[1200];
    snprintf(cache, sizeof cache, "%s/cache", here.folder);
    snprintf(packs, sizeof packs, "%s/history/sha1.git/objects/pack", cache);
    CHECK(!holds_a_temporary_pack(packs));
    const char *forget[] = {NAULA_CMAKE, "-E", "remove_directory", cache, NULL};
    naula_run_result result;
    if (run_in(NULL, forget, NULL, &result)) {
        naula_run_result_release(&result);
    }
    here.environment[0] = "NAULA_FETCH_TIME_LIMIT_MS=1";
    expect_exit(naula("siblings", "--repo", app, NULL), 1, "a fetch past its time limit");
    expect_output("fetch-too-slow", 1, "the time limit");
}

static void sync_refuses_while_a_listed_repo_is_unread(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char tool[1024];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    expect_path(root, "tools/tool", 1, "the tool link");
    path_in(tool, "alpha-tool");
    git(tool, "branch", "-m", "main", "trunk", NULL);
    expect_exit(naula("sync", "--root", root, NULL), 1, "a listed repo without its branch");
    expect_output("repos-unread", 1, "the refusal");
    expect_output("no-working-branch", 1, "the cause");
    expect_path(root, "tools/tool", 1, "the unread repo's link is kept");
}

static void read_whole(const char *path, char *out, size_t capacity) {
    uint8_t *bytes = NULL;
    size_t size = 0;
    unsigned long error = 0;
    out[0] = '\0';
    if (disk_file_read(path, &bytes, &size, &error) == NAULA_OK) {
        snprintf(out, capacity, "%.*s", (int)size, (const char *)bytes);
        naula_memory_release(bytes);
    }
}

static void clone_of_a_refused_repo_writes_nothing(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    make_orgs(alpha, beta_app, gamma);
    char copy_file[TEXT_SIZE];
    snprintf(copy_file, sizeof copy_file,
             "{\"format\":2,\"name\":\"thing\",\"importName\":\"thing\",\"homes\":[{\"provider\":\"forge\",\"account\":"
             "\"delta\"},{\"provider\":\"forge\",\"account\":\"gamma\"}]}\n");
    char thing[1024];
    char copy[1024];
    char url[1100];
    path_in(thing, "gamma-thing");
    path_in(copy, "copy");
    snprintf(url, sizeof url, "file://%s", thing);
    git(NULL, "clone", "--quiet", url, copy, NULL);
    commit_file("copy", "repo.json", copy_file);
    serve(801, "delta", "thing", "copy", NULL);
    init_root(root);
    char file[1100];
    static char before[TEXT_SIZE];
    static char after[TEXT_SIZE];
    snprintf(file, sizeof file, "%s/.naula.json", root);
    read_whole(file, before, sizeof before);
    expect_exit(naula("clone", "forge:801", "--root", root, NULL), 1, "a copy another home disowns");
    read_whole(file, after, sizeof after);
    check_text(after, before, "the root file after a refused clone");
}

static void check_passes_the_adoption_of_an_existing_repo(void) {
    start_case();
    const char *host_files[] = {"README.md", "an existing repo\n", NULL};
    make_repo("host", host_files);
    serve(901, "alpha", "app", "host", NULL);
    char host[1024];
    char local[1024];
    char url[1100];
    path_in(host, "host");
    path_in(local, "local");
    snprintf(url, sizeof url, "file://%s", host);
    git(NULL, "clone", "--quiet", url, local, NULL);
    char file[TEXT_SIZE];
    app_file(file, "app", "alpha", NULL);
    commit_file("local", "repo.json", file);
    expect_exit(naula("check", "--repo", local, NULL), 0, "adopting the repo the name already belongs to");
    expect_output("name-taken", 0, "the name is this repo's own");
}

static void check_refuses_a_shallow_checkout(void) {
    start_case();
    char file[TEXT_SIZE];
    app_file(file, "app", "alpha", NULL);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("host", files);
    commit_file("host", "second.txt", "2\n");
    commit_file("host", "third.txt", "3\n");
    serve(911, "alpha", "app", "host", NULL);
    char host[1024];
    char local[1024];
    char url[1100];
    path_in(host, "host");
    path_in(local, "local");
    snprintf(url, sizeof url, "file://%s", host);
    git(NULL, "clone", "--quiet", "--depth", "1", url, local, NULL);
    expect_exit(naula("check", "--repo", local, NULL), 1, "a shallow checkout");
    expect_output("shallow-checkout", 1, "why it was refused");
    expect_output("fetch-depth: 0", 1, "the fix");
    expect_output("name-taken", 0, "no false name-taken");
}

static void check_fails_closed_past_its_finding_limit(void) {
    start_case();
    static char dependencies[TEXT_SIZE * 16];
    size_t length = (size_t)snprintf(dependencies, sizeof dependencies, ",\"dependencies\":[");
    for (int index = 0; index < 300; index++) {
        length += (size_t)snprintf(dependencies + length, sizeof dependencies - length,
                                   "%s{\"url\":\"https://example.org/x/l%d.git\",\"importName\":\"l%d\","
                                   "\"commit\":\"0123456789abcdef0123456789abcdef01234567\"}",
                                   index == 0 ? "" : ",", index, index);
    }
    snprintf(dependencies + length, sizeof dependencies - length, "]");
    static char file[TEXT_SIZE * 16];
    snprintf(file, sizeof file,
             "{\"format\":2,\"name\":\"app\",\"importName\":\"app\",\"homes\":[{\"provider\":\"forge\",\"account\":"
             "\"alpha\"}]%s}\n",
             dependencies);
    const char *files[] = {"repo.json", file, "README.md", "Build it beside alpha/other-tool.\n", NULL};
    make_repo("app", files);
    char app[1024];
    path_in(app, "app");
    expect_exit(naula("check", "--offline", "--repo", app, NULL), 1, "an error past the finding limit");
    expect_output("too-many-findings", 1, "the overflow");
}

static void check_takes_zeros_as_no_previous(void) {
    start_case();
    char file[TEXT_SIZE];
    app_file(file, "app", "alpha", NULL);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    char app[1024];
    path_in(app, "app");
    expect_exit(naula("check", "--offline", "--repo", app, "--previous", "0000000000000000000000000000000000000000",
                      NULL),
                0, "a new branch's before");
}

static void check_ignores_an_inherited_git_dir(void) {
    start_case();
    char file[TEXT_SIZE];
    app_file(file, "app", "alpha", NULL);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    commit_file("app", "second.txt", "2\n");
    const char *other_files[] = {"README.md", "other\n", NULL};
    make_repo("other", other_files);
    char app[1024];
    char other[1024];
    char first[65];
    char git_dir[1200];
    path_in(app, "app");
    path_in(other, "other");
    revision(app, "HEAD~1", first);
    snprintf(git_dir, sizeof git_dir, "GIT_DIR=%s/.git", other);
    here.environment[0] = git_dir;
    expect_exit(naula("check", "--offline", "--repo", app, "--previous", first, NULL), 0, "a GIT_DIR left by a hook");
}

static void check_prints_findings_as_json_with_the_verdict(void) {
    start_case();
    char file[TEXT_SIZE];
    app_file(file, "app", "alpha", NULL);
    const char *files[] = {"repo.json", file, "README.md", "Build it beside alpha/other-tool.\n", NULL};
    make_repo("app", files);
    char app[1024];
    path_in(app, "app");
    expect_exit(naula("check", "--offline", "--json", "--repo", app, NULL), 1, "a refused check as JSON");
    expect_output("\"code\":\"owner-repo-literal\"", 1, "the finding");
    expect_output("\"passed\":false", 1, "the verdict");
}

static void check_exempts_uses_only_as_a_step_key(void) {
    start_case();
    char file[TEXT_SIZE];
    app_file(file, "app", "alpha", NULL);
    const char *files[] = {"repo.json", file, ".github/workflows/test.yml",
                           "jobs:\n  test:\n    steps:\n      - uses: alpha/workflows/.github/actions/x@main\n"
                           "      - run: echo \"uses: alpha/secret-tool\"\n",
                           NULL};
    make_repo("app", files);
    char app[1024];
    path_in(app, "app");
    expect_exit(naula("check", "--offline", "--repo", app, NULL), 1, "uses: inside a run line");
    expect_output("alpha/secret-tool", 1, "the literal in the run line");
    expect_output("alpha/workflows", 0, "the step's own uses key");
}

static void check_reads_a_repo_in_a_folder_named_outside_the_code_page(void) {
    start_case();
    char file[TEXT_SIZE];
    app_file(file, "app", "alpha", NULL);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("plain", files);
    char plain[1024];
    char named[1024];
    path_in(plain, "plain");
    path_in(named, "r\xc3\xa9po-\xe2\x82\xac");
    const char *rename[] = {NAULA_CMAKE, "-E", "rename", plain, named, NULL};
    naula_run_result result;
    if (run_in(NULL, rename, NULL, &result)) {
        naula_run_result_release(&result);
    }
    expect_exit(naula("check", "--offline", "--repo", named, NULL), 0, "a folder name in UTF-8");
    expect_output("note offline", 1, "its repo.json was read");
    expect_output("unadopted", 0, "the repo was found");
}

/** The one entry in `folder`, empty when it holds none or several. */
static void only_entry(const char *folder, char *name, size_t capacity) {
    char **names = NULL;
    size_t count = 0;
    unsigned long error = 0;
    name[0] = '\0';
    if (disk_names(folder, &names, &count, &error) == NAULA_OK) {
        if (count == 1) {
            snprintf(name, capacity, "%s", names[0]);
        }
        disk_names_release(names, count);
    }
}

/** Finds the boundary where the checkout has moved and its memory bank has not, leaving that journal unfinished. */
static int stop_between_move_and_bank(const char *root, const char *from, const char *to, const char *banks) {
    for (unsigned long stop = 1; stop <= 12; stop++) {
        char key[400];
        char bank[1500];
        char text[16];
        snprintf(text, sizeof text, "%lu", stop);
        const char *remove[] = {NAULA_CMAKE, "-E", "remove_directory", root, NULL};
        const char *clear[] = {NAULA_CMAKE, "-E", "remove_directory", banks, NULL};
        naula_run_result result;
        if (run_in(NULL, remove, NULL, &result)) {
            naula_run_result_release(&result);
        }
        if (run_in(NULL, clear, NULL, &result)) {
            naula_run_result_release(&result);
        }
        make_folder(from);
        git(from, "init", "--quiet", "-b", "main", NULL);
        naula_memory_bank_key(from, key, sizeof key);
        snprintf(bank, sizeof bank, "%s/%s", banks, key);
        make_folder(bank);
        const char *arguments[] = {NAULA_STOPPED_MOVE, root, from, to, banks, text, NULL};
        if (!run_in(NULL, arguments, NULL, &result)) {
            return 0;
        }
        int stopped = result.exit_code == 0;
        naula_run_result_release(&result);
        if (stopped && exists(to) && exists(bank)) {
            char lock[1100];
            snprintf(lock, sizeof lock, "%s/.naula-journal/lock", root);
            remove_file(lock);
            return 1;
        }
    }
    return 0;
}

static void sync_resumes_a_move_with_its_memory_bank(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char from[1100];
    char to[1100];
    char banks[1024];
    char key[400];
    char name[400];
    make_orgs(alpha, beta_app, gamma);
    path_in(root, "root");
    path_in(banks, "banks");
    snprintf(from, sizeof from, "%s/old-place", root);
    snprintf(to, sizeof to, "%s/new-place", root);
    CHECK(stop_between_move_and_bank(root, from, to, banks));
    expect_exit(naula("init", root, "forge:101", NULL), 0, "init resumes the unfinished move");
    expect_output("resume ", 1, "the resume");
    naula_memory_bank_key(to, key, sizeof key);
    only_entry(banks, name, sizeof name);
    check_text(name, key, "the memory bank follows the resumed move");
}

static void init_and_sync_take_a_root_with_a_trailing_separator(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char given[1100];
    make_orgs(alpha, beta_app, gamma);
    path_in(root, "root");
    snprintf(given, sizeof given, "%s/", root);
    expect_exit(naula("init", given, "forge:101", NULL), 0, "init with a trailing separator");
    expect_path(root, "tools/tool", 1, "the tool link");
    expect_exit(naula("sync", "--root", given, NULL), 0, "sync with a trailing separator");
    expect_output("unlink ", 0, "the same root, not another");
}

/** gamma's thing copied into alpha, its repo.json pointing at beta's app, which holds another id and disowns it. */
static void sync_skips_a_copy_a_home_disowns(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char thing[1024];
    char copy[1024];
    char url[1100];
    make_orgs(alpha, beta_app, gamma);
    path_in(thing, "gamma-thing");
    path_in(copy, "alpha-copy");
    snprintf(url, sizeof url, "file://%s", thing);
    git(NULL, "clone", "--quiet", url, copy, NULL);
    char file[TEXT_SIZE];
    snprintf(file, sizeof file, "{\"format\":2,\"name\":\"%s\",\"importName\":\"%s\",\"homes\":["
             "{\"provider\":\"forge\",\"account\":\"alpha\"},{\"provider\":\"forge\",\"account\":\"beta\"}]}\n",
             "app", "app");
    commit_file("alpha-copy", "repo.json", file);
    serve(103, "alpha", "app", "alpha-copy", NULL);
    path_in(root, "root");
    expect_exit(naula("init", root, "forge:101", NULL), 0, "a copy in a held account");
    expect_output("copy-skipped", 1, "the copy is named");
    expect_output("repos-unread", 0, "and the run goes on");
    expect_path(root, "tools/tool", 1, "the rest is laid out");
}

static void sync_refuses_a_copy_naula_linked(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    char file[TEXT_SIZE];
    snprintf(file, sizeof file, "{\"format\":2,\"name\":\"%s\",\"importName\":\"%s\",\"homes\":["
             "{\"provider\":\"forge\",\"account\":\"alpha\"},{\"provider\":\"forge\",\"account\":\"beta\"}]}\n",
             "tool", "tool");
    commit_file("alpha-tool", "repo.json", file);
    char other[TEXT_SIZE];
    app_file(other, "tool", "beta", NULL);
    const char *other_files[] = {"repo.json", other, NULL};
    make_repo("beta-tool", other_files);
    serve(203, "beta", "tool", "beta-tool", NULL);
    expect_exit(naula("sync", "--root", root, NULL), 1, "our own repo with a mistake in its homes");
    expect_output("copy-linked", 1, "why it was not skipped");
    expect_path(root, "tools/tool", 1, "its link is kept");
}

static void sync_notes_an_empty_repo_as_unadopted(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char empty[1024];
    make_orgs(alpha, beta_app, gamma);
    path_in(empty, "alpha-empty");
    make_folder(empty);
    git(empty, "init", "--quiet", "-b", "main", NULL);
    serve(104, "alpha", "empty", "alpha-empty", NULL);
    path_in(root, "root");
    expect_exit(naula("init", root, "forge:101", NULL), 0, "an empty repo in a held account");
    expect_output("note unadopted", 1, "the empty repo is a note");
    expect_output("repos-unread", 0, "never a refusal");
    expect_path(root, "tools/tool", 1, "the rest is laid out");
}

static void check_counts_an_unheard_home_as_unverifiable(void) {
    start_case();
    char file[TEXT_SIZE];
    app_file(file, "app", "alpha", NULL);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("host", files);
    serve(921, "alpha", "app", "host", NULL);
    char host[1024];
    path_in(host, "host");
    here.environment[0] = "NAULA_FETCH_TIME_LIMIT_MS=1";
    expect_exit(naula("check", "--repo", host, NULL), 0, "a home whose history cannot be read in time");
    expect_output("name-unverifiable", 1, "the warning");
    expect_output("name-taken", 0, "never taken for unheard");
}

static void sync_takes_the_lock_before_cloning(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char tool[1024];
    char tool_root[65];
    char checkout[128];
    char path[1200];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    path_in(tool, "alpha-tool");
    root_commit(tool, tool_root);
    checkout_of(tool_root, checkout, NULL);
    snprintf(path, sizeof path, "%s/%s", root, checkout);
    const char *remove[] = {NAULA_CMAKE, "-E", "remove_directory", path, NULL};
    naula_run_result result;
    if (run_in(NULL, remove, NULL, &result)) {
        naula_run_result_release(&result);
    }
    char lock[1100];
    char holder[32];
    snprintf(lock, sizeof lock, "%s/.naula-journal/lock", root);
    snprintf(holder, sizeof holder, "%lu\n", disk_process_id());
    write_text(lock, holder);
    expect_exit(naula("sync", "--root", root, NULL), 1, "a root another naula holds");
    expect_output("root-locked", 1, "the lock");
    CHECK(!exists(path));
    remove_file(lock);
}

/**
 * Our tool lists a second home in zeta, whose repo of the same id does not list it back, so the tool is disowned; a
 * self-listing fork of the tool's id, `fork_name`, sits in alpha, which the root holds.
 */
static void make_tool_disowned_beside_a_fork(const char *tool_name, const char *fork_name) {
    char tool[1024];
    char fork[1024];
    char zeta[1024];
    char url[1100];
    path_in(tool, "alpha-tool");
    path_in(fork, "alpha-fork");
    path_in(zeta, "zeta-tool");
    snprintf(url, sizeof url, "file://%s", tool);
    git(NULL, "clone", "--quiet", url, fork, NULL);
    char fork_file[TEXT_SIZE];
    app_file(fork_file, fork_name, "alpha", NULL);
    commit_file("alpha-fork", "repo.json", fork_file);
    serve(103, "alpha", fork_name, "alpha-fork", NULL);
    git(NULL, "clone", "--quiet", url, zeta, NULL);
    char zeta_file[TEXT_SIZE];
    app_file(zeta_file, tool_name, "zeta", NULL);
    commit_file("zeta-tool", "repo.json", zeta_file);
    serve(901, "zeta", tool_name, "zeta-tool", NULL);
    char tool_file[TEXT_SIZE];
    snprintf(tool_file, sizeof tool_file,
             "{\"format\":2,\"name\":\"%s\",\"importName\":\"%s\",\"topics\":[\"tools\"],\"homes\":[{\"provider\":"
             "\"forge\",\"account\":\"alpha\"},{\"provider\":\"forge\",\"account\":\"zeta\"}]}\n",
             tool_name, tool_name);
    commit_file("alpha-tool", "repo.json", tool_file);
}

static void expect_tool_left_as_it_was(const char *root, const char *before_target, const char *label) {
    char link[1100];
    char *target = NULL;
    unsigned long error = 0;
    snprintf(link, sizeof link, "%s/tools/tool", root);
    expect_path(root, "tools/tool", 1, label);
    if (disk_link_target(link, &target, &error) == NAULA_OK) {
        check_text(target, before_target, label);
        naula_memory_release(target);
    }
    expect_absent(root, "evil", "the fork gets no link");
}

static void tool_link_target(const char *root, char *target_out, size_t capacity) {
    char link[1100];
    char *target = NULL;
    unsigned long error = 0;
    snprintf(link, sizeof link, "%s/tools/tool", root);
    target_out[0] = '\0';
    if (disk_link_target(link, &target, &error) == NAULA_OK) {
        snprintf(target_out, capacity, "%s", target);
        naula_memory_release(target);
    }
}

static void sync_keeps_a_linked_copys_checkout_from_a_fork_of_its_id(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char before[1100];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    tool_link_target(root, before, sizeof before);
    make_tool_disowned_beside_a_fork("tool", "evil");
    expect_exit(naula("sync", "--root", root, NULL), 1, "our linked repo made a copy beside a fork of its id");
    expect_output("copy-linked", 1, "the copy naula linked refuses");
    expect_tool_left_as_it_was(root, before, "the tool keeps its link and checkout");
}

static void sync_refuses_a_fork_taking_a_checkout_naula_linked(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char before[1100];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    tool_link_target(root, before, sizeof before);
    make_tool_disowned_beside_a_fork("renamed", "evil");
    expect_exit(naula("sync", "--root", root, NULL), 1, "a fork of our id beside our renamed copy");
    expect_output("checkout-claimed", 1, "the fork may not take the checkout naula linked");
    expect_output("copy-skipped", 1, "the renamed copy has no link of its own");
    expect_output("copy-linked", 0, "a link is never matched by its id alone");
    expect_tool_left_as_it_was(root, before, "the tool keeps its link and checkout");
}

static void sync_refuses_an_empty_repo_naula_linked(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char empty[1024];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    path_in(empty, "alpha-tool-emptied");
    make_folder(empty);
    git(empty, "init", "--quiet", "-b", "main", NULL);
    for (size_t index = 0; index < here.repo_count; index++) {
        if (here.repos[index].number == 102) {
            snprintf(here.repos[index].path, sizeof here.repos[index].path, "%s", empty);
        }
    }
    expect_exit(naula("sync", "--root", root, NULL), 1, "our linked repo emptied on its host");
    expect_output("empty-linked", 1, "the empty repo naula linked refuses");
    expect_output("note unadopted", 0, "never a note");
    expect_path(root, "tools/tool", 1, "its link is kept");
}

static void release_keeps_an_edited_one_line_rules_file(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char rules[1100];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    expect_exit(naula("hold", "forge:201", "--root", root, NULL), 0, "hold beta");
    snprintf(rules, sizeof rules, "%s/beta/CLAUDE.md", root);
    write_text(rules, "@docs/MY-OWN-RULES.md\n");
    expect_exit(naula("release", "beta", "--root", root, NULL), 0, "release beta");
    expect_output("rules-file-edited", 1, "the edited import is named");
    expect_path(root, "beta/CLAUDE.md", 0, "the edited import stays");
}

typedef struct test_entry {
    const char *name;
    void (*run)(void);
} test_entry;

static const test_entry tests[] = {
    {"check_refuses_a_rebased_root", check_refuses_a_rebased_root},
    {"check_warns_on_a_narrow_at_and_prints_the_line", check_warns_on_a_narrow_at_and_prints_the_line},
    {"check_warns_on_a_url_dependency", check_warns_on_a_url_dependency},
    {"check_refuses_an_owner_repo_literal_outside_exempt_places",
     check_refuses_an_owner_repo_literal_outside_exempt_places},
    {"siblings_clones_nested_paths", siblings_clones_nested_paths},
    {"siblings_refuses_a_copy", siblings_refuses_a_copy},
    {"sync_is_a_no_op_on_a_laid_out_root", sync_is_a_no_op_on_a_laid_out_root},
    {"hold_adds_an_org_and_lays_it_out", hold_adds_an_org_and_lays_it_out},
    {"release_removes_an_orgs_links_but_never_a_dirty_clone", release_removes_an_orgs_links_but_never_a_dirty_clone},
    {"clone_names_the_folder_by_twelve_hex", clone_names_the_folder_by_twelve_hex},
    {"siblings_refuses_a_link_in_its_own_siblings_folder", siblings_refuses_a_link_in_its_own_siblings_folder},
    {"siblings_refuses_a_link_a_dependency_tracks", siblings_refuses_a_link_a_dependency_tracks},
    {"siblings_compares_paths_ignoring_case", siblings_compares_paths_ignoring_case},
    {"siblings_checks_every_commit_on_its_own_primary_before_cloning",
     siblings_checks_every_commit_on_its_own_primary_before_cloning},
    {"siblings_stops_a_fetch_past_its_byte_limit", siblings_stops_a_fetch_past_its_byte_limit},
    {"sync_refuses_while_a_listed_repo_is_unread", sync_refuses_while_a_listed_repo_is_unread},
    {"clone_of_a_refused_repo_writes_nothing", clone_of_a_refused_repo_writes_nothing},
    {"check_passes_the_adoption_of_an_existing_repo", check_passes_the_adoption_of_an_existing_repo},
    {"check_refuses_a_shallow_checkout", check_refuses_a_shallow_checkout},
    {"check_fails_closed_past_its_finding_limit", check_fails_closed_past_its_finding_limit},
    {"check_takes_zeros_as_no_previous", check_takes_zeros_as_no_previous},
    {"check_ignores_an_inherited_git_dir", check_ignores_an_inherited_git_dir},
    {"check_prints_findings_as_json_with_the_verdict", check_prints_findings_as_json_with_the_verdict},
    {"check_exempts_uses_only_as_a_step_key", check_exempts_uses_only_as_a_step_key},
    {"check_reads_a_repo_in_a_folder_named_outside_the_code_page",
     check_reads_a_repo_in_a_folder_named_outside_the_code_page},
    {"sync_resumes_a_move_with_its_memory_bank", sync_resumes_a_move_with_its_memory_bank},
    {"check_counts_an_unheard_home_as_unverifiable", check_counts_an_unheard_home_as_unverifiable},
    {"sync_skips_a_copy_a_home_disowns", sync_skips_a_copy_a_home_disowns},
    {"sync_refuses_a_copy_naula_linked", sync_refuses_a_copy_naula_linked},
    {"sync_notes_an_empty_repo_as_unadopted", sync_notes_an_empty_repo_as_unadopted},
    {"init_and_sync_take_a_root_with_a_trailing_separator", init_and_sync_take_a_root_with_a_trailing_separator},
    {"sync_takes_the_lock_before_cloning", sync_takes_the_lock_before_cloning},
    {"sync_keeps_a_linked_copys_checkout_from_a_fork_of_its_id", sync_keeps_a_linked_copys_checkout_from_a_fork_of_its_id},
    {"sync_refuses_a_fork_taking_a_checkout_naula_linked", sync_refuses_a_fork_taking_a_checkout_naula_linked},
    {"sync_refuses_an_empty_repo_naula_linked", sync_refuses_an_empty_repo_naula_linked},
    {"release_keeps_an_edited_one_line_rules_file", release_keeps_an_edited_one_line_rules_file},
};

int main(int count, char **values) {
    naula_json_use_allocator();
    if (count != 2) {
        fprintf(stderr, "usage: cli_test <test name>\n");
        return 2;
    }
    for (size_t index = 0; index < sizeof tests / sizeof tests[0]; index++) {
        if (strcmp(values[1], tests[index].name) == 0) {
            case_number = index;
            tests[index].run();
            naula_run_result_release(&here.last);
            return check_summary(values[1]);
        }
    }
    fprintf(stderr, "no test named %s\n", values[1]);
    return 2;
}
