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
#include "naula/sha256.h"

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

/**
 * Every fixture git stops looking for a repository at the scratch folder: a case folder whose own repo is missing or
 * half removed would otherwise reach the repository the build folder sits in and commit the fixture there.
 */
#define FIXTURE_GIT_CEILING "GIT_CEILING_DIRECTORIES=" NAULA_TEST_SCRATCH

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
    int timed_out;
    int refuse_rename;
} forge_repo;

/** Everything one case builds under its own folder, and the fake forge it serves to the naula program. */
typedef struct test_case {
    char folder[1024];
    forge_repo repos[FORGE_LIMIT];
    size_t repo_count;
    unsigned clock;
    naula_run_result last;
    const char *environment[4];
    char log[TEXT_SIZE * 2];
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
                                 "GIT_COMMITTER_EMAIL=fixture@example.invalid", FIXTURE_GIT_CEILING, NULL};
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
                                   "\"timed_out\":%s,\"refuse_rename\":%s,\"topics\":[%s]}",
                                   index == 0 ? "" : ",", repo->number, repo->account, repo->name, repo->path,
                                   repo->archived ? "true" : "false", repo->timed_out ? "true" : "false",
                                   repo->refuse_rename ? "true" : "false", repo->topics);
    }
    snprintf(text + length, sizeof text - length, "],\"log\":%s}", here.log[0] == '\0' ? "[]" : here.log);
    char path[1024];
    path_in(path, "forge.json");
    write_text(path, text);
}

static forge_repo *find_served(unsigned number) {
    for (size_t index = 0; index < here.repo_count; index++) {
        if (here.repos[index].number == number) {
            return &here.repos[index];
        }
    }
    return NULL;
}

/** The served repo of that number; a case naming one the forge lacks fails, and gets a blank repo to read. */
static forge_repo *served(unsigned number) {
    static forge_repo nowhere;
    forge_repo *found = find_served(number);
    if (found == NULL) {
        fprintf(stderr, "the forge serves no repo %u\n", number);
        check_failures++;
        memset(&nowhere, 0, sizeof nowhere);
        return &nowhere;
    }
    return found;
}

/** Takes back what the program wrote into the forge, its log and every repo it created, renamed or moved. */
static void read_forge(void) {
    char path[1100];
    snprintf(path, sizeof path, "%s/forge.json", here.folder);
    uint8_t *bytes = NULL;
    size_t length = 0;
    unsigned long error = 0;
    cJSON *json = NULL;
    const char *reason = NULL;
    if (disk_file_read(path, &bytes, &length, &error) != NAULA_OK ||
        naula_json_parse(bytes, length, &json, &reason) != NAULA_OK) {
        naula_memory_release(bytes);
        return;
    }
    const cJSON *repo = NULL;
    cJSON_ArrayForEach(repo, cJSON_GetObjectItemCaseSensitive(json, "repos")) {
        unsigned number = (unsigned)cJSON_GetObjectItemCaseSensitive(repo, "number")->valuedouble;
        forge_repo *known = find_served(number);
        if (known == NULL && here.repo_count < FORGE_LIMIT) {
            known = &here.repos[here.repo_count++];
            memset(known, 0, sizeof *known);
            known->number = number;
            snprintf(known->path, sizeof known->path, "%s",
                     cJSON_GetObjectItemCaseSensitive(repo, "path")->valuestring);
        }
        if (known == NULL) {
            continue;
        }
        snprintf(known->account, sizeof known->account, "%s",
                 cJSON_GetObjectItemCaseSensitive(repo, "account")->valuestring);
        snprintf(known->name, sizeof known->name, "%s", cJSON_GetObjectItemCaseSensitive(repo, "name")->valuestring);
        known->archived = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(repo, "archived"));
        const cJSON *topics = cJSON_GetObjectItemCaseSensitive(repo, "topics");
        char *text = topics == NULL ? NULL : cJSON_PrintUnformatted(topics);
        if (text != NULL && strlen(text) >= 2) {
            snprintf(known->topics, sizeof known->topics, "%.*s", (int)strlen(text) - 2, text + 1);
        }
        cJSON_free(text);
    }
    char *log = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(json, "log"));
    snprintf(here.log, sizeof here.log, "%s", log == NULL ? "" : log);
    cJSON_free(log);
    cJSON_Delete(json);
    naula_memory_release(bytes);
}

static size_t log_holds(const char *entry) {
    char quoted[300];
    snprintf(quoted, sizeof quoted, "\"%s\"", entry);
    size_t count = 0;
    for (const char *at = strstr(here.log, quoted); at != NULL; at = strstr(at + 1, quoted)) {
        count++;
    }
    return count;
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
    read_forge();
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
static void make_tool_disowned_by_zeta(const char *tool_name);

static void make_tool_disowned_beside_a_fork(const char *tool_name, const char *fork_name) {
    char tool[1024];
    char fork[1024];
    char url[1100];
    path_in(tool, "alpha-tool");
    path_in(fork, "alpha-fork");
    snprintf(url, sizeof url, "file://%s", tool);
    git(NULL, "clone", "--quiet", url, fork, NULL);
    char fork_file[TEXT_SIZE];
    app_file(fork_file, fork_name, "alpha", NULL);
    commit_file("alpha-fork", "repo.json", fork_file);
    serve(103, "alpha", fork_name, "alpha-fork", NULL);
    make_tool_disowned_by_zeta(tool_name);
}

/** Our tool lists a second home in zeta, whose repo of the same id and name does not list it back. */
static void make_tool_disowned_by_zeta(const char *tool_name) {
    char tool[1024];
    char zeta[1024];
    char url[1100];
    path_in(tool, "alpha-tool");
    path_in(zeta, "zeta-tool");
    snprintf(url, sizeof url, "file://%s", tool);
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
    expect_output("id-contested", 1, "the fork may not take the checkout naula linked");
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

/** beta, held beside alpha, holds a self-listing fork of alpha's tool under the tool's own name and topic. */
static void make_fork_of_our_name_in_beta(const char *root) {
    char tool[1024];
    char fork[1024];
    char url[1100];
    expect_exit(naula("hold", "forge:201", "--root", root, NULL), 0, "hold beta");
    path_in(tool, "alpha-tool");
    path_in(fork, "beta-tool");
    snprintf(url, sizeof url, "file://%s", tool);
    git(NULL, "clone", "--quiet", url, fork, NULL);
    char fork_file[TEXT_SIZE];
    app_file(fork_file, "tool", "beta", ",\"topics\":[\"tools\"]");
    commit_file("beta-tool", "repo.json", fork_file);
    serve(203, "beta", "tool", "beta-tool", NULL);
}

static void expect_checkout_where_it_was(const char *root, const char *label) {
    char tool[1024];
    char tool_root[65];
    char checkout[128];
    path_in(tool, "alpha-tool");
    root_commit(tool, tool_root);
    checkout_of(tool_root, checkout, NULL);
    expect_path(root, checkout, 0, label);
}

static void sync_refuses_a_fork_of_our_name_in_another_held_account(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char before[1100];
    char after[1100];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    tool_link_target(root, before, sizeof before);
    make_fork_of_our_name_in_beta(root);
    expect_exit(naula("sync", "--root", root, NULL), 1, "a fork of our name and id in another held account");
    expect_output("id-contested", 1, "the contested id refuses");
    expect_output("unlink ", 0, "no link is removed");
    tool_link_target(root, after, sizeof after);
    check_text(after, before, "the tool keeps its link");
    expect_checkout_where_it_was(root, "the tool keeps its checkout");
}

static void sync_moves_nothing_when_our_repo_beside_a_fork_of_its_name_is_made_a_copy(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char before[1100];
    char after[1100];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    tool_link_target(root, before, sizeof before);
    make_fork_of_our_name_in_beta(root);
    expect_exit(naula("sync", "--root", root, NULL), 1, "a fork of our name and id in another held account");
    make_tool_disowned_by_zeta("tool");
    expect_exit(naula("sync", "--root", root, NULL), 1, "our repo made a copy beside the fork");
    expect_output("copy-linked", 1, "our linked copy refuses");
    expect_output("id-contested", 1, "and so does the contested id");
    expect_output("move ", 0, "nothing moves");
    expect_output("unlink ", 0, "no link is removed");
    tool_link_target(root, after, sizeof after);
    check_text(after, before, "the tool keeps its link");
    expect_checkout_where_it_was(root, "the tool keeps its checkout");
}

static void bare_clone(const char *from, const char *to) {
    char source[1024];
    char target[1024];
    path_in(source, from);
    path_in(target, to);
    git(NULL, "clone", "--quiet", "--bare", source, target, NULL);
    git(target, "config", "uploadpack.allowFilter", "true", NULL);
}

/** The url the test forge serves a fixture folder at, which a clone of it must carry to be recognised as that home. */
static void forge_url(const char *folder, char *url) {
    char path[1024];
    path_in(path, folder);
    snprintf(url, 1100, "file://%s%s", path[0] == '/' ? "" : "/", path);
}

static void read_text(const char *path, char *out, size_t capacity) {
    uint8_t *bytes = NULL;
    size_t length = 0;
    unsigned long error = 0;
    out[0] = '\0';
    if (disk_file_read(path, &bytes, &length, &error) == NAULA_OK) {
        snprintf(out, capacity, "%.*s", (int)length, (const char *)bytes);
    }
    naula_memory_release(bytes);
}

static void expect_text(const char *text, const char *wanted, int present, const char *label) {
    if ((strstr(text, wanted) != NULL) != present) {
        fprintf(stderr, "%s: the text %s \"%s\":\n%s\n", label, present ? "lacks" : "holds", wanted, text);
        check_failures++;
    }
}

static void homes_file(char *out, const char *name, const char *first, const char *second, const char *extra) {
    char homes[256];
    snprintf(homes, sizeof homes, "{\"provider\":\"forge\",\"account\":\"%s\"}", first);
    if (second != NULL) {
        size_t length = strlen(homes);
        snprintf(homes + length, sizeof homes - length, ",{\"provider\":\"forge\",\"account\":\"%s\"}", second);
    }
    snprintf(out, TEXT_SIZE, "{\"format\":2,\"name\":\"%s\",\"importName\":\"%s\",\"homes\":[%s]%s}\n", name, name,
             homes, extra == NULL ? "" : extra);
}

static void make_alpha_docs(const char *mirror_account) {
    char file[TEXT_SIZE];
    homes_file(file, "docs", "alpha", mirror_account,
               ",\"org\":{\"name\":\"alpha\",\"accounts\":[{\"provider\":\"forge\",\"account\":\"alpha\"}],"
               "\"rules\":\"RULES.md\",\"projects\":{\"tools\":\"Programs.\"}}");
    const char *files[] = {"repo.json", file, "RULES.md", "alpha rules\n", NULL};
    make_repo("alpha-docs", files);
    serve(101, "alpha", "docs", "alpha-docs", NULL);
    if (mirror_account != NULL) {
        bare_clone("alpha-docs", "alpha-docs-mirror.git");
        serve(111, mirror_account, "docs", "alpha-docs-mirror.git", NULL);
    }
}

/** `app` of alpha, mirrored as a bare clone in account `mirror`, laid out in a root that holds alpha. */
static void make_app_with_mirror(char *root) {
    make_alpha_docs(NULL);
    char file[TEXT_SIZE];
    homes_file(file, "app", "alpha", "mirror", NULL);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    bare_clone("app", "app-mirror.git");
    serve(501, "alpha", "app", "app", NULL);
    serve(502, "mirror", "app", "app-mirror.git", NULL);
    init_root(root);
}

static void sync_converges_after_a_home_refused_the_rename(void) {
    start_case();
    char root[1024];
    char file[TEXT_SIZE];
    make_app_with_mirror(root);
    homes_file(file, "app2", "alpha", "mirror", NULL);
    commit_file("app", "repo.json", file);
    served(502)->refuse_rename = 1;
    expect_exit(naula("sync", "--root", root, "--accept-home", "forge:mirror", NULL), 1, "a mirror refuses the rename");
    expect_output("rename-refused", 1, "the refusing home is reported");
    check_text(served(501)->name, "app2", "the primary is renamed");
    check_text(served(502)->name, "app", "the refusing home keeps its name");
    expect_path(root, "app2", 1, "the link follows the primary");
    served(502)->refuse_rename = 0;
    expect_exit(naula("sync", "--root", root, NULL), 0, "the next sync converges");
    check_text(served(502)->name, "app2", "the mirror is renamed on the retry");
    CHECK(log_holds("rename 501 app2") == 1);
    CHECK(log_holds("rename 502 app2") == 1);
    size_t written = strlen(here.log);
    expect_exit(naula("sync", "--root", root, NULL), 0, "a converged repo");
    CHECK(strlen(here.log) == written);
}

static void rename_is_refused_before_push_when_a_home_has_the_name(void) {
    start_case();
    char root[1024];
    char file[TEXT_SIZE];
    char other[TEXT_SIZE];
    make_app_with_mirror(root);
    app_file(other, "taken", "mirror", NULL);
    const char *other_files[] = {"repo.json", other, "other.txt", "another repo\n", NULL};
    make_repo("other", other_files);
    serve(503, "mirror", "taken", "other", NULL);
    homes_file(file, "taken", "alpha", "mirror", NULL);
    commit_file("app", "repo.json", file);
    expect_exit(naula("sync", "--root", root, "--accept-home", "forge:mirror", NULL), 1, "a home holds the new name");
    expect_output("name-taken", 1, "the taken name");
    CHECK(log_holds("rename 501 taken") == 0);
    CHECK(log_holds("rename 502 taken") == 0);
    check_text(served(501)->name, "app", "nothing is renamed");
}

static void rename_is_refused_for_a_reusable_workflow_repo(void) {
    start_case();
    char root[1024];
    char file[TEXT_SIZE];
    make_alpha_docs(NULL);
    homes_file(file, "workflows", "alpha", NULL, NULL);
    const char *files[] = {"repo.json", file, ".github/workflows/check.yml",
                           "on:\n  workflow_call:\n    inputs: {}\njobs: {}\n", NULL};
    make_repo("workflows", files);
    serve(501, "alpha", "workflows", "workflows", NULL);
    init_root(root);
    homes_file(file, "shared-workflows", "alpha", NULL, NULL);
    commit_file("workflows", "repo.json", file);
    expect_exit(naula("sync", "--root", root, NULL), 1, "a reusable workflow repo is renamed");
    expect_output("literal-consumers", 1, "why the rename is refused");
    expect_output(".github/workflows/check.yml", 1, "the workflow is named");
    CHECK(log_holds("rename 501 shared-workflows") == 0);
}

static void transfer_moves_the_checkout_only_across_orgs(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char tool_root[65];
    char before[128];
    char after[128];
    char docs[128];
    char tool[1024];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    expect_exit(naula("hold", "forge:201", "--root", root, NULL), 0, "hold beta");
    path_in(tool, "alpha-tool");
    root_commit(tool, tool_root);
    checkout_of(tool_root, before, NULL);
    checkout_of(tool_root, after, "beta");
    checkout_of(alpha, docs, NULL);
    expect_path(root, before, 0, "the tool's checkout in alpha");
    char file[TEXT_SIZE];
    app_file(file, "tool", "beta", ",\"topics\":[\"tools\"]");
    commit_file("alpha-tool", "repo.json", file);
    expect_exit(naula("sync", "--root", root, NULL), 0, "the tool moves to beta");
    expect_output("transfer forge:102 from alpha to beta", 1, "the home is transferred");
    check_text(served(102)->account, "beta", "the host holds it under beta");
    expect_path(root, after, 0, "the checkout moved into beta's repos");
    expect_absent(root, before, "the checkout left alpha's repos");
    expect_path(root, "beta/tool", 1, "the link under beta");
    expect_path(root, docs, 0, "alpha's docs stay where they were");
    expect_exit(naula("sync", "--root", root, NULL), 0, "a second sync");
    expect_output("move ", 0, "a second sync moves nothing");
    CHECK(log_holds("transfer 102 beta") == 1);
}

static void planned_move_creates_the_new_home_and_pushes_every_ref(void) {
    start_case();
    char root[1024];
    char file[TEXT_SIZE];
    char app[1024];
    make_alpha_docs(NULL);
    homes_file(file, "app", "alpha", NULL, NULL);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    path_in(app, "app");
    git(app, "branch", "feature", NULL);
    git(app, "tag", "v1", NULL);
    serve(501, "alpha", "app", "app", NULL);
    init_root(root);
    homes_file(file, "app", "fresh", "alpha", NULL);
    commit_file("app", "repo.json", file);
    naula("sync", "--root", root, NULL);
    expect_output("home-unaccepted", 1, "an account no held org owns waits for acceptance");
    CHECK(find_served(900) == NULL);
    naula("sync", "--root", root, "--accept-home", "forge:fresh", NULL);
    expect_output("create forge:900 fresh/app", 1, "the new home is created");
    const forge_repo *created = find_served(900);
    CHECK(created != NULL);
    if (created != NULL) {
        static const char *const refs[] = {"refs/heads/main", "refs/heads/feature", "refs/tags/v1"};
        for (size_t index = 0; index < 3; index++) {
            char wanted[65];
            char pushed[65];
            revision(app, refs[index], wanted);
            revision(created->path, refs[index], pushed);
            check_text(pushed, wanted, refs[index]);
        }
    }
    naula("sync", "--root", root, "--accept-primary", "forge:900", "--accept-home", "forge:fresh", NULL);
    CHECK(log_holds("create 900 fresh/app") == 1);
}

/** `lib` moved from account old (601, a bare clone) to account new (602); `pushed` 0 leaves old on the first file. */
static void make_moved_lib(int pushed, char *lib) {
    char file[TEXT_SIZE];
    homes_file(file, "lib", pushed ? "new" : "old", pushed ? "old" : NULL, NULL);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("lib", files);
    bare_clone("lib", "old.git");
    if (!pushed) {
        homes_file(file, "lib", "new", "old", NULL);
        commit_file("lib", "repo.json", file);
    }
    serve(601, "old", "lib", "old.git", NULL);
    serve(602, "new", "lib", "lib", NULL);
    path_in(lib, "lib");
}

static void retire_refuses_while_a_dependent_lacks_the_new_home(void) {
    start_case();
    char root[1024];
    char lib[1024];
    char lib_root[65];
    char head[65];
    make_alpha_docs(NULL);
    make_moved_lib(1, lib);
    root_commit(lib, lib_root);
    revision(lib, "HEAD", head);
    char dependency[512];
    char file[TEXT_SIZE];
    snprintf(dependency, sizeof dependency,
             ",\"dependencies\":[{\"id\":\"git:%s\",\"at\":[\"forge:601\"],\"importName\":\"lib\",\"commit\":\"%s\"}]",
             lib_root, head);
    app_file(file, "app", "alpha", dependency);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    serve(501, "alpha", "app", "app", NULL);
    init_root(root);
    expect_exit(naula("retire", "forge:601", "--repo", lib, "--root", root, NULL), 1, "a dependent lacks the new home");
    expect_output("dependent-lacks-home", 1, "the dependent is named");
    CHECK(log_holds("archive 601") == 0);
    char app_root[65];
    char app[1024];
    char checkout[128];
    char dependent[1300];
    path_in(app, "app");
    root_commit(app, app_root);
    checkout_of(app_root, checkout, NULL);
    snprintf(dependent, sizeof dependent, "%s/%s/repo.json", root, checkout);
    snprintf(dependency, sizeof dependency,
             ",\"dependencies\":[{\"id\":\"git:%s\",\"at\":[\"forge:601\",\"forge:602\"],\"importName\":\"lib\","
             "\"commit\":\"%s\"}]",
             lib_root, head);
    app_file(file, "app", "alpha", dependency);
    write_text(dependent, file);
    expect_exit(naula("retire", "forge:601", "--repo", lib, "--root", root, NULL), 0, "every dependent has the new home");
    CHECK(log_holds("archive 601") == 1);
    CHECK(served(601)->archived);
}

static void retire_refuses_while_the_old_home_times_out(void) {
    start_case();
    char root[1024];
    char lib[1024];
    make_alpha_docs(NULL);
    make_moved_lib(1, lib);
    init_root(root);
    served(601)->timed_out = 1;
    expect_exit(naula("retire", "forge:601", "--repo", lib, "--root", root, NULL), 1, "the old home is slow");
    expect_output("home-unheard", 1, "a slow home is never retired");
    CHECK(log_holds("archive 601") == 0);
    CHECK(!served(601)->archived);
}

static void retire_archives_only_after_the_listing_commit_is_pushed(void) {
    start_case();
    char root[1024];
    char lib[1024];
    char old[1024];
    make_alpha_docs(NULL);
    make_moved_lib(0, lib);
    init_root(root);
    expect_exit(naula("retire", "forge:601", "--repo", lib, "--root", root, NULL), 1, "the listing is not pushed");
    expect_output("listing-not-pushed", 1, "why the old home stays");
    CHECK(log_holds("archive 601") == 0);
    path_in(old, "old.git");
    git(lib, "push", "--quiet", old, "main", NULL);
    expect_exit(naula("retire", "forge:601", "--repo", lib, "--root", root, "--accept-primary", "forge:602", NULL), 0,
                "the listing is pushed");
    CHECK(log_holds("archive 601") == 1);
}

/** `lib` of alpha served as 401 with two commits, and `app`, whose format 1 file depends on it twice, cloned locally. */
static void make_format_one_app(const char *extra_members, char *local, char first[65], char second[65]) {
    char lib_file[TEXT_SIZE];
    char lib[1024];
    app_file(lib_file, "lib", "alpha", NULL);
    const char *lib_files[] = {"repo.json", lib_file, NULL};
    make_repo("lib", lib_files);
    commit_file("lib", "second.txt", "second\n");
    serve(401, "alpha", "lib", "lib", NULL);
    path_in(lib, "lib");
    revision(lib, "HEAD~1", first);
    revision(lib, "HEAD", second);
    char file[TEXT_SIZE];
    snprintf(file, sizeof file,
             "{\"format\":1,\"importName\":\"app\",\"hosts\":[\"forge:alpha\"],\"dependencies\":["
             "{\"id\":\"forge:401\",\"importName\":\"lib\",\"commit\":\"%s\",\"path\":\"siblings/x/siblings/lib\"},"
             "{\"id\":\"forge:401\",\"importName\":\"lib\",\"commit\":\"%s\"}]%s}\n",
             first, second, extra_members == NULL ? "" : extra_members);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    serve(501, "alpha", "app", "app", NULL);
    char url[1100];
    forge_url("app", url);
    path_in(local, "app-local");
    git(NULL, "clone", "--quiet", url, local, NULL);
}

static void adopt_keeps_paths_and_repeats(void) {
    start_case();
    char local[1024];
    char first[65];
    char second[65];
    char lib_root[65];
    char lib[1024];
    make_format_one_app(NULL, local, first, second);
    path_in(lib, "lib");
    root_commit(lib, lib_root);
    expect_exit(naula("adopt", "--repo", local, NULL), 0, "adopt a format 1 file");
    char path[1100];
    char text[TEXT_SIZE * 2];
    char wanted[200];
    snprintf(path, sizeof path, "%s/repo.json", local);
    read_text(path, text, sizeof text);
    expect_text(text, "\"format\": 2", 1, "format 2");
    expect_text(text, "\"name\": \"app\"", 1, "the name from the host");
    expect_text(text, "\"provider\": \"forge\"", 1, "the home");
    snprintf(wanted, sizeof wanted, "\"id\": \"git:%s\"", lib_root);
    expect_text(text, wanted, 1, "the dependency's id from its history");
    expect_text(text, "\"forge:401\"", 1, "the dependency's at");
    expect_text(text, "\"path\": \"siblings/x/siblings/lib\"", 1, "the path is kept");
    expect_text(text, first, 1, "the first commit is kept");
    expect_text(text, second, 1, "the repeated dependency is kept");
    const char *at = strstr(text, wanted);
    CHECK(at != NULL && strstr(at + 1, wanted) != NULL);
}

static void adopt_is_idempotent(void) {
    start_case();
    char local[1024];
    char first[65];
    char second[65];
    make_format_one_app(",\"description\":\"An app.\",\"topics\":[\"tools\"]", local, first, second);
    expect_exit(naula("adopt", "--repo", local, NULL), 0, "the first adopt");
    char path[1100];
    static char once[TEXT_SIZE * 2];
    static char twice[TEXT_SIZE * 2];
    snprintf(path, sizeof path, "%s/repo.json", local);
    read_text(path, once, sizeof once);
    expect_exit(naula("adopt", "--repo", local, NULL), 0, "a second adopt");
    expect_output("kept repo.json", 1, "the second run writes nothing");
    read_text(path, twice, sizeof twice);
    check_text(twice, once, "a second run changes no byte");
}

static void adopt_proposes_the_fork_exception_on_a_shared_root(void) {
    start_case();
    const char *shelf_files[] = {"README.md", "shelf data\n", NULL};
    make_repo("shelf", shelf_files);
    char shelf[1024];
    char inventree[1024];
    path_in(shelf, "shelf");
    path_in(inventree, "inventree");
    git(NULL, "clone", "--quiet", shelf, inventree, NULL);
    commit_file("inventree", "own.txt", "the copy's own work\n");
    char own[65];
    revision(inventree, "HEAD", own);
    commit_file("inventree", "repo.json", "{\"format\":1,\"importName\":\"inventree\",\"hosts\":[\"forge:alpha\"]}\n");
    serve(601, "alpha", "shelf", "shelf", NULL);
    serve(602, "alpha", "inventree", "inventree", NULL);
    char url[1100];
    char local[1024];
    forge_url("inventree", url);
    path_in(local, "inventree-local");
    git(NULL, "clone", "--quiet", url, local, NULL);
    char path[1100];
    char text[TEXT_SIZE];
    snprintf(path, sizeof path, "%s/repo.json", local);
    expect_exit(naula("adopt", "--repo", local, NULL), 1, "a root shelf shares");
    expect_output("root-shared", 1, "the shared root");
    expect_output(own, 1, "the proposed fork id");
    read_text(path, text, sizeof text);
    expect_text(text, "\"format\":1", 1, "nothing is written while it is undecided");
    expect_exit(naula("adopt", "--repo", local, "--fork-id", own, NULL), 0, "the fork exception taken");
    char wanted[100];
    snprintf(wanted, sizeof wanted, "\"id\": \"git:%s\"", own);
    read_text(path, text, sizeof text);
    expect_text(text, wanted, 1, "the stated id");
}

static void adopt_drops_claims_and_place(void) {
    start_case();
    char local[1024];
    char first[65];
    char second[65];
    make_format_one_app(",\"place\":\"tools/app\",\"claims\":[\"forge:beta\"]", local, first, second);
    expect_exit(naula("adopt", "--repo", local, NULL), 0, "adopt drops what format 2 removed");
    char path[1100];
    char text[TEXT_SIZE * 2];
    snprintf(path, sizeof path, "%s/repo.json", local);
    read_text(path, text, sizeof text);
    expect_text(text, "\"format\": 2", 1, "format 2");
    expect_text(text, "claims", 0, "claims are dropped");
    expect_text(text, "place", 0, "place is dropped");
}

static void new_creates_every_home_and_the_link(void) {
    start_case();
    char root[1024];
    make_alpha_docs("alphamirror");
    init_root(root);
    expect_exit(naula("new", "alpha", "widget", "--project", "tools", "--root", root, NULL), 0, "a new repo");
    CHECK(log_holds("create 900 alpha/widget") == 1);
    CHECK(log_holds("create 901 alphamirror/widget") == 1);
    CHECK(log_holds("topics 900 1") == 1);
    expect_path(root, "tools/widget", 1, "the new repo's link");
    const forge_repo *primary = find_served(900);
    const forge_repo *mirror = find_served(901);
    CHECK(primary != NULL && mirror != NULL);
    if (primary != NULL && mirror != NULL) {
        char main_tip[65];
        char development[65];
        char mirrored[65];
        revision(primary->path, "refs/heads/main", main_tip);
        revision(primary->path, "refs/heads/development", development);
        revision(mirror->path, "refs/heads/development", mirrored);
        CHECK(main_tip[0] != '\0');
        check_text(development, main_tip, "both branches at the first commit");
        check_text(mirrored, main_tip, "the mirror holds it too");
    }
    expect_exit(naula("new", "alpha", "widget", "--root", root, NULL), 1, "the name is taken now");
    expect_output("name-taken", 1, "a second new refuses");
}

static void add_writes_id_at_importname_and_commit(void) {
    start_case();
    char lib_file[TEXT_SIZE];
    char lib[1024];
    char lib_root[65];
    char head[65];
    app_file(lib_file, "lib", "alpha", NULL);
    const char *lib_files[] = {"repo.json", lib_file, NULL};
    make_repo("lib", lib_files);
    commit_file("lib", "second.txt", "second\n");
    serve(401, "alpha", "lib", "lib", NULL);
    path_in(lib, "lib");
    root_commit(lib, lib_root);
    revision(lib, "HEAD", head);
    char file[TEXT_SIZE];
    app_file(file, "app", "gamma", NULL);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    char app[1024];
    path_in(app, "app");
    expect_exit(naula("add", "forge:401", "--repo", app, NULL), 0, "add a dependency");
    char path[1100];
    char text[TEXT_SIZE];
    char wanted[200];
    snprintf(path, sizeof path, "%s/repo.json", app);
    read_text(path, text, sizeof text);
    snprintf(wanted, sizeof wanted, "\"id\": \"git:%s\"", lib_root);
    expect_text(text, wanted, 1, "the id from its history");
    expect_text(text, "\"forge:401\"", 1, "at from the confirmed homes");
    expect_text(text, "\"importName\": \"lib\"", 1, "the dependency's import name");
    snprintf(wanted, sizeof wanted, "\"commit\": \"%s\"", head);
    expect_text(text, wanted, 1, "the primary's tip");
    expect_exit(naula("add", "forge:401", "--repo", app, NULL), 1, "a second add");
    expect_output("dependency-present", 1, "the dependency is there already");
}

static void reconcile_refuses_a_home_no_held_org_owns(void) {
    start_case();
    char root[1024];
    char file[TEXT_SIZE];
    make_alpha_docs(NULL);
    homes_file(file, "app", "alpha", NULL, NULL);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    serve(501, "alpha", "app", "app", NULL);
    init_root(root);
    snprintf(file, sizeof file,
             "{\"format\":2,\"name\":\"app\",\"importName\":\"app\",\"homes\":[{\"provider\":\"forge\",\"account\":"
             "\"alpha\"},{\"provider\":\"forge\",\"host\":\"elsewhere.example\",\"account\":\"outsider\"}]}\n");
    commit_file("app", "repo.json", file);
    expect_exit(naula("sync", "--root", root, NULL), 1, "a home in an account no held org owns");
    expect_output("home-unaccepted", 1, "the unaccepted home is refused");
    expect_output("forge@elsewhere.example:outsider", 1, "the home is named as --accept-home takes it");
    CHECK(log_holds("create 900 outsider/app") == 0);
    CHECK(find_served(900) == NULL);
    expect_exit(naula("sync", "--root", root, "--accept-home", "forge@elsewhere.example:outsider", NULL), 0,
                "the home accepted for this run");
    CHECK(log_holds("create 900 outsider/app") == 1);
    const forge_repo *created = find_served(900);
    if (created != NULL) {
        char app[1024];
        char wanted[65];
        char pushed[65];
        path_in(app, "app");
        revision(app, "refs/heads/main", wanted);
        revision(created->path, "refs/heads/main", pushed);
        check_text(pushed, wanted, "the accepted home holds the branch");
    }
}

static void reconcile_creates_in_a_held_account_without_acceptance(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char file[TEXT_SIZE];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    expect_exit(naula("hold", "forge:201", "--root", root, NULL), 0, "hold beta");
    homes_file(file, "tool", "alpha", "beta", ",\"topics\":[\"tools\"]");
    commit_file("alpha-tool", "repo.json", file);
    naula("sync", "--root", root, NULL);
    expect_output("home-unaccepted", 0, "a held account needs no acceptance");
    CHECK(log_holds("create 900 beta/tool") == 1);
}

static void sync_of_an_org_reconciles_only_its_repos(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char file[TEXT_SIZE];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    expect_exit(naula("hold", "forge:201", "--root", root, NULL), 0, "hold beta");
    app_file(file, "tool2", "alpha", ",\"topics\":[\"tools\"]");
    commit_file("alpha-tool", "repo.json", file);
    app_file(file, "app2", "beta", NULL);
    commit_file("beta-app", "repo.json", file);
    naula("sync", "beta", "--root", root, NULL);
    CHECK(log_holds("rename 202 app2") == 1);
    CHECK(log_holds("rename 102 tool2") == 0);
    check_text(served(102)->name, "tool", "alpha's repo is left alone");
}

static void adopt_refuses_to_invent_an_org(void) {
    start_case();
    char local[1024];
    char first[65];
    char second[65];
    make_format_one_app(",\"projects\":{\"tools\":\"Programs.\"}", local, first, second);
    expect_exit(naula("adopt", "--repo", local, NULL), 1, "format 1 projects name no org");
    expect_output("org-undeclared", 1, "the org is refused, never invented");
    char path[1100];
    char text[TEXT_SIZE * 2];
    snprintf(path, sizeof path, "%s/repo.json", local);
    read_text(path, text, sizeof text);
    expect_text(text, "\"format\":1", 1, "nothing is written");
}

/** Writes `hints` as the hint file of the checkout of `root_hex` whose history listed the homes `anchor` joins. */
static void plant_hints(const char *root_hex, const char *anchor, const char *hints) {
    naula_sha256 hash;
    char key[NAULA_SHA256_HEX_SIZE];
    naula_sha256_start(&hash);
    naula_sha256_add(&hash, (const uint8_t *)anchor, strlen(anchor));
    naula_sha256_hex(&hash, key);
    char homes[1100];
    char name[200];
    snprintf(homes, sizeof homes, "%s/cache/homes", here.folder);
    snprintf(name, sizeof name, "%s-%s.json", root_hex, key);
    make_folder(homes);
    unsigned long error = 0;
    CHECK(disk_file_replace(homes, name, (const uint8_t *)hints, strlen(hints), DISK_FOR_NAULA_FILE, &error) ==
          NAULA_OK);
}

/**
 * A hint naming an account the checkout's own repo.json never listed, as one another checkout of the id (a fork)
 * would leave behind, is never taken as the serving home.
 */
static void sync_never_serves_from_a_hint_the_checkout_never_listed(void) {
    start_case();
    char root[1024];
    char file[TEXT_SIZE];
    char app[1024];
    char app_root[65];
    make_alpha_docs(NULL);
    homes_file(file, "app", "alpha", NULL, NULL);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    path_in(app, "app");
    root_commit(app, app_root);
    serve(501, "alpha", "app", "app", NULL);
    init_root(root);
    bare_clone("app", "fork.git");
    char fork_file[TEXT_SIZE];
    homes_file(fork_file, "app", "zeta", NULL, ",\"topics\":[\"stolen\"]");
    char fork_work[1024];
    char fork_bare[1024];
    path_in(fork_work, "fork-work");
    path_in(fork_bare, "fork.git");
    git(NULL, "clone", "--quiet", fork_bare, fork_work, NULL);
    commit_file("fork-work", "repo.json", fork_file);
    git(fork_work, "push", "--quiet", "origin", "main", NULL);
    serve(702, "zeta", "app", "fork.git", NULL);
    snprintf(served(501)->name, sizeof served(501)->name, "%s", "renamed-by-hand");
    plant_hints(app_root, "forge@:alpha", "{\"homes\":[{\"provider\":\"forge\",\"account\":\"zeta\",\"number\":702}]}");
    naula("sync", "--root", root, NULL);
    expect_output("serving-unproven", 1, "the fork's hint is not taken");
    CHECK(log_holds("topics 702 1") == 0);
}

/** `app` of alpha with `mallory`, a bare clone of it at forge:502, added to its homes in a later commit. */
static void make_app_mallory_lists(char *root, char *app) {
    char file[TEXT_SIZE];
    make_alpha_docs(NULL);
    homes_file(file, "app", "alpha", NULL, NULL);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    path_in(app, "app");
    serve(501, "alpha", "app", "app", NULL);
    init_root(root);
    bare_clone("app", "mallory.git");
    serve(502, "mallory", "app", "mallory.git", NULL);
    homes_file(file, "app", "alpha", "mallory", NULL);
    commit_file("app", "repo.json", file);
}

static void expect_same_branch(const char *left, const char *right, int same, const char *label) {
    char one[65];
    char other[65];
    revision(left, "refs/heads/main", one);
    revision(right, "refs/heads/main", other);
    if ((strcmp(one, other) == 0) != same) {
        fprintf(stderr, "%s: main is %s and %s\n", label, one, other);
        check_failures++;
    }
}

/**
 * Someone who can push the working branch lists their own account and pre-creates a repo there holding our root and
 * a repo.json listing the serving home: it receives nothing until a run accepts it, and from then on every push.
 */
static void sync_pushes_into_a_home_outside_the_root_only_once_accepted(void) {
    start_case();
    char root[1024];
    char app[1024];
    make_app_mallory_lists(root, app);
    const char *mallory = served(502)->path;
    expect_exit(naula("sync", "--root", root, NULL), 1, "a home no held org owns");
    expect_output("home-unaccepted", 1, "the unaccepted home is refused");
    expect_output("forge:mallory", 1, "the home is named as --accept-home takes it");
    expect_same_branch(app, mallory, 0, "nothing is pushed to it");
    expect_exit(naula("sync", "--root", root, "--accept-home", "forge:mallory", NULL), 0, "the home accepted");
    expect_same_branch(app, mallory, 1, "the accepted home receives the push");
    commit_file("app", "notes.txt", "more\n");
    expect_exit(naula("sync", "--root", root, NULL), 0, "the acceptance is kept for this checkout");
    expect_same_branch(app, mallory, 1, "the accepted home receives the next push");
}

static void sync_pushes_into_a_held_accounts_home_without_acceptance(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char file[TEXT_SIZE];
    char tool[1024];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    expect_exit(naula("hold", "forge:201", "--root", root, NULL), 0, "hold beta");
    bare_clone("alpha-tool", "beta-tool.git");
    serve(250, "beta", "tool", "beta-tool.git", NULL);
    homes_file(file, "tool", "alpha", "beta", ",\"topics\":[\"tools\"]");
    commit_file("alpha-tool", "repo.json", file);
    expect_exit(naula("sync", "--root", root, NULL), 0, "a held account");
    expect_output("home-unaccepted", 0, "a held account needs no acceptance");
    path_in(tool, "alpha-tool");
    expect_same_branch(tool, served(250)->path, 1, "the held home receives the push");
}

/** A home a sync only found and had nothing to write to is not accepted by that, so a later write waits for one. */
static void sync_never_accepts_a_home_it_only_found(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char file[TEXT_SIZE];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    expect_exit(naula("clone", "forge:301", "--root", root, NULL), 0, "clone gamma's thing");
    expect_exit(naula("sync", "--root", root, NULL), 0, "nothing to write to gamma");
    app_file(file, "thing", "gamma", ",\"topics\":[\"things\"]");
    commit_file("gamma-thing", "repo.json", file);
    expect_exit(naula("sync", "--root", root, NULL), 1, "a write to gamma");
    expect_output("home-unaccepted", 1, "the write waits for acceptance");
    CHECK(log_holds("topics 301 1") == 0);
}

/** A transfer moves the repo found under an old account, so that account must be held or accepted as well. */
static void transfer_refuses_a_source_never_accepted(void) {
    start_case();
    char alpha[65];
    char beta_app[65];
    char gamma[65];
    char root[1024];
    char file[TEXT_SIZE];
    make_orgs(alpha, beta_app, gamma);
    init_root(root);
    expect_exit(naula("hold", "forge:201", "--root", root, NULL), 0, "hold beta");
    bare_clone("alpha-tool", "stranger-tool.git");
    serve(150, "stranger", "tool", "stranger-tool.git", NULL);
    homes_file(file, "tool", "alpha", "stranger", ",\"topics\":[\"tools\"]");
    commit_file("alpha-tool", "repo.json", file);
    homes_file(file, "tool", "alpha", "beta", ",\"topics\":[\"tools\"]");
    commit_file("alpha-tool", "repo.json", file);
    expect_exit(naula("sync", "--root", root, NULL), 1, "a transfer from an account never accepted");
    expect_output("transfer-source-unaccepted", 1, "the source is refused");
    CHECK(log_holds("transfer 150 beta") == 0);
    check_text(served(150)->account, "stranger", "the repo stays where it is");
}

/**
 * A home once listed in the checkout's history and dropped since, never accepted nor held, is not taken as the serving
 * home from a hint, though the history still lists it.
 */
static void sync_never_serves_from_a_dropped_home_never_accepted(void) {
    start_case();
    char root[1024];
    char file[TEXT_SIZE];
    char app[1024];
    char app_root[65];
    make_alpha_docs(NULL);
    homes_file(file, "app", "alpha", NULL, NULL);
    const char *files[] = {"repo.json", file, NULL};
    make_repo("app", files);
    path_in(app, "app");
    root_commit(app, app_root);
    homes_file(file, "app", "alpha", "mallory", NULL);
    commit_file("app", "repo.json", file);
    homes_file(file, "app", "alpha", NULL, NULL);
    commit_file("app", "repo.json", file);
    serve(501, "alpha", "app", "app", NULL);
    init_root(root);
    bare_clone("app", "mallory.git");
    char mallory_file[TEXT_SIZE];
    homes_file(mallory_file, "app", "mallory", "alpha", ",\"topics\":[\"stolen\"]");
    char work[1024];
    char bare[1024];
    path_in(work, "mallory-work");
    path_in(bare, "mallory.git");
    git(NULL, "clone", "--quiet", bare, work, NULL);
    commit_file("mallory-work", "repo.json", mallory_file);
    git(work, "push", "--quiet", "origin", "main", NULL);
    serve(702, "mallory", "app", "mallory.git", NULL);
    snprintf(served(501)->name, sizeof served(501)->name, "%s", "renamed-by-hand");
    plant_hints(app_root, "forge@:alpha\nforge@:mallory",
                "{\"homes\":[{\"provider\":\"forge\",\"account\":\"mallory\",\"number\":702}]}");
    naula("sync", "--root", root, NULL);
    expect_output("serving-unproven", 1, "the dropped home's hint is not taken");
    CHECK(log_holds("topics 702 1") == 0);
    CHECK(log_holds("create 900 alpha/app") == 0);
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
    {"sync_refuses_a_fork_of_our_name_in_another_held_account",
     sync_refuses_a_fork_of_our_name_in_another_held_account},
    {"sync_moves_nothing_when_our_repo_beside_a_fork_of_its_name_is_made_a_copy",
     sync_moves_nothing_when_our_repo_beside_a_fork_of_its_name_is_made_a_copy},
    {"sync_converges_after_a_home_refused_the_rename", sync_converges_after_a_home_refused_the_rename},
    {"rename_is_refused_before_push_when_a_home_has_the_name", rename_is_refused_before_push_when_a_home_has_the_name},
    {"rename_is_refused_for_a_reusable_workflow_repo", rename_is_refused_for_a_reusable_workflow_repo},
    {"transfer_moves_the_checkout_only_across_orgs", transfer_moves_the_checkout_only_across_orgs},
    {"planned_move_creates_the_new_home_and_pushes_every_ref", planned_move_creates_the_new_home_and_pushes_every_ref},
    {"retire_refuses_while_a_dependent_lacks_the_new_home", retire_refuses_while_a_dependent_lacks_the_new_home},
    {"retire_refuses_while_the_old_home_times_out", retire_refuses_while_the_old_home_times_out},
    {"retire_archives_only_after_the_listing_commit_is_pushed", retire_archives_only_after_the_listing_commit_is_pushed},
    {"adopt_keeps_paths_and_repeats", adopt_keeps_paths_and_repeats},
    {"adopt_is_idempotent", adopt_is_idempotent},
    {"adopt_proposes_the_fork_exception_on_a_shared_root", adopt_proposes_the_fork_exception_on_a_shared_root},
    {"adopt_drops_claims_and_place", adopt_drops_claims_and_place},
    {"new_creates_every_home_and_the_link", new_creates_every_home_and_the_link},
    {"add_writes_id_at_importname_and_commit", add_writes_id_at_importname_and_commit},
    {"reconcile_refuses_a_home_no_held_org_owns", reconcile_refuses_a_home_no_held_org_owns},
    {"reconcile_creates_in_a_held_account_without_acceptance", reconcile_creates_in_a_held_account_without_acceptance},
    {"sync_of_an_org_reconciles_only_its_repos", sync_of_an_org_reconciles_only_its_repos},
    {"adopt_refuses_to_invent_an_org", adopt_refuses_to_invent_an_org},
    {"sync_never_serves_from_a_hint_the_checkout_never_listed",
     sync_never_serves_from_a_hint_the_checkout_never_listed},
    {"sync_pushes_into_a_home_outside_the_root_only_once_accepted",
     sync_pushes_into_a_home_outside_the_root_only_once_accepted},
    {"sync_pushes_into_a_held_accounts_home_without_acceptance",
     sync_pushes_into_a_held_accounts_home_without_acceptance},
    {"sync_never_accepts_a_home_it_only_found", sync_never_accepts_a_home_it_only_found},
    {"transfer_refuses_a_source_never_accepted", transfer_refuses_a_source_never_accepted},
    {"sync_never_serves_from_a_dropped_home_never_accepted", sync_never_serves_from_a_dropped_home_never_accepted},
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
