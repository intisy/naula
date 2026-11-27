#include <stdio.h>
#include <string.h>

#include "check.h"
#include "memory_testing.h"
#include "naula/layout.h"

#define ROOT "T:/root"
#define MAX_REPOS 16
#define MAX_ORGS 8

typedef struct scenario {
    naula_org_declaration top;
    naula_org_declaration orgs[MAX_ORGS];
    naula_id org_repositories[MAX_ORGS];
    size_t org_count;
    naula_id top_repository;
    naula_layout_repo repos[MAX_REPOS];
    size_t repo_count;
    const char *dropped[4];
    size_t dropped_count;
    naula_reference extra[4];
    const char *extra_orgs[4];
    size_t extra_count;
    naula_layout_link links[16];
    size_t link_count;
} scenario;

static naula_home intisy_accounts[] = {{sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "intisy", NULL, NULL}};
static naula_home spisor_accounts[] = {{sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "spisor", NULL, NULL}};
static naula_home forebay_accounts[] = {
    {sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "forebay", NULL, NULL}};
static naula_home weivel_accounts[] = {{sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "weivel", NULL, NULL}};
static naula_home pinaxis_accounts[] = {
    {sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "pinaxis", NULL, NULL}};
static char *tools_project[] = {"tools"};
static char *tools_description[] = {"Programs used to build the rest"};
static char *spisor_projects[] = {"shared", "platform"};
static char *spisor_descriptions[] = {"What any application uses", "The Java server side"};

static char hex_texts[MAX_REPOS * 2][41];
static size_t hex_used;

static const char *hex_of(char digit) {
    char *text = hex_texts[hex_used++ % (MAX_REPOS * 2)];
    memset(text, digit, 40);
    text[40] = '\0';
    return text;
}

static naula_id git_id(const char *hex) {
    naula_id id;
    memset(&id, 0, sizeof id);
    id.size = sizeof id;
    id.kind = NAULA_ID_GIT;
    snprintf(id.commit, sizeof id.commit, "%s", hex);
    return id;
}

static naula_place intisy_docs_place = {sizeof(naula_place), NAULA_HOME_PROVIDER, "github", NULL, 1, NULL, NULL};
static naula_reference intisy_docs_reference = {sizeof(naula_reference), "git:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                                                NULL, &intisy_docs_place, 1};
static naula_place spisor_docs_place = {sizeof(naula_place), NAULA_HOME_PROVIDER, "github", NULL, 2, NULL, NULL};
static naula_reference spisor_docs_reference = {sizeof(naula_reference), "git:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                                                NULL, &spisor_docs_place, 1};
static naula_place weivel_records_place = {sizeof(naula_place), NAULA_HOME_PROVIDER, "github", NULL, 3, NULL, NULL};
static naula_reference weivel_records_reference = {sizeof(naula_reference),
                                                   "git:cccccccccccccccccccccccccccccccccccccccc", NULL,
                                                   &weivel_records_place, 1};

static naula_org_declaration declaration(const char *name, naula_reference *parent, naula_home *accounts,
                                         const char *rules) {
    naula_org_declaration org;
    memset(&org, 0, sizeof org);
    org.size = sizeof org;
    org.name = (char *)name;
    org.parent = parent;
    org.accounts = accounts;
    org.account_count = 1;
    org.rules = (char *)rules;
    return org;
}

static naula_org_declaration forebay_suborg;

static naula_layout_repo repo_at(const char *hex, const char *name, const char *account, const char *current_path) {
    naula_layout_repo repo;
    memset(&repo, 0, sizeof repo);
    repo.size = sizeof repo;
    repo.id = git_id(hex);
    repo.name = (char *)name;
    repo.primary.size = sizeof repo.primary;
    repo.primary.kind = NAULA_HOME_PROVIDER;
    repo.primary.provider = "github";
    repo.primary.account = (char *)account;
    repo.current_path = (char *)current_path;
    repo.adopted = 1;
    return repo;
}

static void add_link_made(scenario *world, const char *path, const char *target, int made_by_naula) {
    naula_layout_link *link = &world->links[world->link_count++];
    link->size = sizeof *link;
    link->path = (char *)path;
    link->target = (char *)target;
    link->made_by_naula = made_by_naula;
}

static void add_link(scenario *world, const char *path, const char *target) {
    add_link_made(world, path, target, 1);
}

static void add_foreign_link(scenario *world, const char *path, const char *target) {
    add_link_made(world, path, target, 0);
}

static naula_layout_repo *add_repo(scenario *world, naula_layout_repo repo) {
    world->repos[world->repo_count] = repo;
    return &world->repos[world->repo_count++];
}

static void set_topics(naula_layout_repo *repo, char **topics, size_t count) {
    repo->topics = topics;
    repo->topic_count = count;
}

static char *naula_topics[] = {"tools"};
static char *library_topics[] = {"shared"};

static void start_world(scenario *world) {
    memset(world, 0, sizeof *world);
    hex_used = 0;
    world->top = declaration("intisy", NULL, intisy_accounts, "RULES.md");
    world->top.project_names = tools_project;
    world->top.project_descriptions = tools_description;
    world->top.project_count = 1;
    world->top_repository = git_id(hex_of('a'));

    forebay_suborg = declaration("forebay", NULL, forebay_accounts, "forebay/CLAUDE.md");
    world->orgs[0] = declaration("spisor", &intisy_docs_reference, spisor_accounts, "RULES.md");
    world->orgs[0].project_names = spisor_projects;
    world->orgs[0].project_descriptions = spisor_descriptions;
    world->orgs[0].project_count = 2;
    world->orgs[0].suborgs = &forebay_suborg;
    world->orgs[0].suborg_count = 1;
    world->org_repositories[0] = git_id(hex_of('b'));
    world->orgs[1] = declaration("weivel", &intisy_docs_reference, weivel_accounts, "RULES.md");
    world->org_repositories[1] = git_id(hex_of('c'));
    world->orgs[2] = declaration("pinaxis", &intisy_docs_reference, pinaxis_accounts, NULL);
    world->org_repositories[2] = git_id(hex_of('9'));
    world->org_count = 3;

    add_repo(world, repo_at(hex_of('a'), "docs", "intisy", NULL));
    set_topics(add_repo(world, repo_at(hex_of('d'), "naula", "intisy", NULL)), naula_topics, 1);
    add_repo(world, repo_at(hex_of('b'), "docs", "spisor", NULL));
    set_topics(add_repo(world, repo_at(hex_of('e'), "library", "spisor", NULL)), library_topics, 1);
    add_repo(world, repo_at(hex_of('f'), "host", "forebay", NULL));
    add_repo(world, repo_at(hex_of('c'), "records", "weivel", NULL));
    add_repo(world, repo_at(hex_of('9'), "docs", "pinaxis", NULL));
}

static naula_layout_input input_of(const scenario *world) {
    naula_layout_input input;
    memset(&input, 0, sizeof input);
    input.size = sizeof input;
    input.root = ROOT;
    input.top = &world->top;
    input.orgs = world->orgs;
    input.org_count = world->org_count;
    input.org_repositories = world->org_repositories;
    input.top_repository = &world->top_repository;
    input.dropped = world->dropped;
    input.dropped_count = world->dropped_count;
    input.repos = world->repos;
    input.repo_count = world->repo_count;
    input.extra = world->extra;
    input.extra_orgs = world->extra_orgs;
    input.extra_count = world->extra_count;
    input.existing_links = world->links;
    input.existing_link_count = world->link_count;
    return input;
}

static naula_status plan_world(const scenario *world, naula_layout_plan *plan, naula_refusal *refusal) {
    memset(plan, 0, sizeof *plan);
    plan->size = sizeof *plan;
    memset(refusal, 0, sizeof *refusal);
    refusal->size = sizeof *refusal;
    naula_layout_input input = input_of(world);
    naula_status status = naula_layout_plan_make(&input, plan, refusal);
    if (status != NAULA_OK && strcmp(refusal->code, "") != 0) {
        fprintf(stderr, "  (plan refused: %s: %s)\n", refusal->code, refusal->detail);
    }
    return status;
}

static const naula_layout_step *find_step(const naula_layout_plan *plan, uint32_t kind, const char *path) {
    for (size_t index = 0; index < plan->step_count; index++) {
        const naula_layout_step *step = &plan->steps[index];
        if (step->kind == kind && step->path != NULL && strcmp(step->path, path) == 0) {
            return step;
        }
    }
    return NULL;
}

static size_t step_index(const naula_layout_plan *plan, uint32_t kind, const char *path) {
    const naula_layout_step *step = find_step(plan, kind, path);
    return step == NULL ? (size_t)-1 : (size_t)(step - plan->steps);
}

static const naula_layout_step *find_report(const naula_layout_plan *plan, const char *code, const char *path) {
    for (size_t index = 0; index < plan->step_count; index++) {
        const naula_layout_step *step = &plan->steps[index];
        if (step->kind == NAULA_REPORT && strcmp(step->code, code) == 0 &&
            (path == NULL || (step->path != NULL && strcmp(step->path, path) == 0))) {
            return step;
        }
    }
    return NULL;
}

static size_t count_kind(const naula_layout_plan *plan, uint32_t kind) {
    size_t count = 0;
    for (size_t index = 0; index < plan->step_count; index++) {
        count += plan->steps[index].kind == kind;
    }
    return count;
}

static int links_to(const naula_layout_plan *plan, const char *link, const char *target) {
    const naula_layout_step *step = find_step(plan, NAULA_MAKE_LINK, link);
    if (step == NULL) {
        fprintf(stderr, "  no link at %s\n", link);
        return 0;
    }
    return check_text(step->target, target, link);
}

static int writes(const naula_layout_plan *plan, const char *path, const char *content) {
    const naula_layout_step *step = find_step(plan, NAULA_WRITE_FILE, path);
    if (step == NULL) {
        fprintf(stderr, "  no file written at %s\n", path);
        return 0;
    }
    return check_text(step->content, content, path);
}

static void nests_a_suborg_under_its_parent(void) {
    scenario world;
    start_world(&world);
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_MAKE_FOLDER, ROOT "/spisor") != NULL);
    CHECK(find_step(&plan, NAULA_MAKE_FOLDER, ROOT "/spisor/forebay") != NULL);
    CHECK(find_step(&plan, NAULA_MAKE_FOLDER, ROOT "/spisor/forebay/repos") != NULL);
    CHECK(find_report(&plan, "account-not-owned", ROOT "/spisor/forebay") != NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/spisor/forebay/host") == NULL);
    CHECK(step_index(&plan, NAULA_MAKE_FOLDER, ROOT "/spisor") <
          step_index(&plan, NAULA_MAKE_FOLDER, ROOT "/spisor/forebay"));
    naula_layout_plan_release(&plan);
}

static void a_suborg_holds_its_account_once_a_repo_there_confirms_it(void) {
    scenario world;
    start_world(&world);
    world.repos[4].member_of = &spisor_docs_reference;
    add_repo(&world, repo_at(hex_of('1'), "proxy", "forebay", NULL));
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "account-not-owned", NULL) == NULL);
    CHECK(links_to(&plan, ROOT "/spisor/forebay/host", ROOT "/spisor/forebay/repos/ffffffffffff"));
    CHECK(links_to(&plan, ROOT "/spisor/forebay/proxy", ROOT "/spisor/forebay/repos/111111111111"));
    CHECK(find_report(&plan, "member-of-unknown", NULL) == NULL);
    CHECK(find_report(&plan, "member-of-unlisted", NULL) == NULL);
    naula_layout_plan_release(&plan);
}

static naula_home intisy_and_archive_accounts[] = {
    {sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "intisy", NULL, NULL},
    {sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "intisy-archive", NULL, NULL}};

static void start_world_holding_the_archive(scenario *world) {
    start_world(world);
    world->top.accounts = intisy_and_archive_accounts;
    world->top.account_count = 2;
    add_repo(world, repo_at(hex_of('1'), "old-tool", "intisy-archive", NULL));
}

static void an_org_holds_an_extra_account_through_a_confirming_repo(void) {
    scenario world;
    start_world_holding_the_archive(&world);
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "account-not-owned", ROOT) != NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/old-tool") == NULL);
    naula_layout_plan_release(&plan);

    world.repos[world.repo_count - 1].member_of = &intisy_docs_reference;
    add_repo(&world, repo_at(hex_of('2'), "older-tool", "intisy-archive", NULL));
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "account-not-owned", ROOT) == NULL);
    CHECK(links_to(&plan, ROOT "/old-tool", ROOT "/repos/111111111111"));
    CHECK(links_to(&plan, ROOT "/older-tool", ROOT "/repos/222222222222"));
    naula_layout_plan_release(&plan);
}

static void a_strangers_member_of_confirms_nothing(void) {
    scenario world;
    start_world_holding_the_archive(&world);
    add_repo(&world, repo_at(hex_of('2'), "claimer", "stranger", ROOT "/claimer"))->member_of =
        &intisy_docs_reference;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "account-not-owned", ROOT) != NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/old-tool") == NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/claimer") == NULL);
    CHECK(find_step(&plan, NAULA_MOVE_CHECKOUT, ROOT "/claimer") == NULL);
    CHECK(find_report(&plan, "member-of-unlisted", ROOT "/claimer") != NULL);
    naula_layout_plan_release(&plan);
}

static naula_place unknown_docs_place = {sizeof(naula_place), NAULA_HOME_PROVIDER, "github", NULL, 4, NULL, NULL};
static naula_reference unknown_docs_reference = {sizeof(naula_reference),
                                                 "git:6666666666666666666666666666666666666666", NULL,
                                                 &unknown_docs_place, 1};

static void a_member_of_naming_an_org_nobody_holds_is_reported(void) {
    const char *dropped[] = {NULL, "spisor"};
    naula_reference *references[] = {&unknown_docs_reference, &spisor_docs_reference};
    for (size_t index = 0; index < 2; index++) {
        scenario world;
        start_world(&world);
        world.dropped[0] = dropped[index];
        world.dropped_count = dropped[index] == NULL ? 0 : 1;
        world.repos[4].member_of = references[index];
        world.repos[4].current_path = ROOT "/host";
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
        const naula_layout_step *report = find_report(&plan, "member-of-unknown", ROOT "/host");
        CHECK(report != NULL && strstr(report->content, "host") != NULL);
        CHECK(find_step(&plan, NAULA_MOVE_CHECKOUT, ROOT "/host") == NULL);
        naula_layout_plan_release(&plan);
    }
}

static naula_home weivel_and_forebay_accounts[] = {
    {sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "weivel", NULL, NULL},
    {sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "forebay", NULL, NULL}};

static void two_confirmation_only_claims_hold_neither(void) {
    scenario world;
    start_world(&world);
    world.orgs[1].accounts = weivel_and_forebay_accounts;
    world.orgs[1].account_count = 2;
    world.repos[4].member_of = &spisor_docs_reference;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(links_to(&plan, ROOT "/spisor/forebay/host", ROOT "/spisor/forebay/repos/ffffffffffff"));
    CHECK(find_report(&plan, "account-not-owned", ROOT "/weivel") != NULL);
    naula_layout_plan_release(&plan);

    add_repo(&world, repo_at(hex_of('1'), "proxy", "forebay", NULL))->member_of = &weivel_records_reference;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "member-of-contested", ROOT "/spisor/forebay") != NULL);
    CHECK(find_report(&plan, "member-of-contested", ROOT "/weivel") != NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/spisor/forebay/host") == NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/weivel/proxy") == NULL);
    CHECK(find_report(&plan, "member-of-unlisted", NULL) == NULL);
    naula_layout_plan_release(&plan);
}

static naula_home weivel_and_spisor_accounts[] = {
    {sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "weivel", NULL, NULL},
    {sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "spisor", NULL, NULL}};

static void a_declaring_repo_wins_over_a_confirmation_only_claim(void) {
    scenario world;
    start_world(&world);
    world.orgs[1].accounts = weivel_and_spisor_accounts;
    world.orgs[1].account_count = 2;
    world.repos[3].member_of = &weivel_records_reference;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "member-of-contested", ROOT "/weivel") != NULL);
    CHECK(links_to(&plan, ROOT "/spisor/shared/library", ROOT "/spisor/repos/eeeeeeeeeeee"));
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/weivel/library") == NULL);
    CHECK(find_report(&plan, "member-of-unlisted", NULL) == NULL);
    naula_layout_plan_release(&plan);
}

static void a_suborg_entry_listing_its_parents_account_is_reported(void) {
    scenario world;
    start_world(&world);
    forebay_suborg.accounts = spisor_accounts;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "suborg-lists-parent-account", ROOT "/spisor/forebay") != NULL);
    CHECK(links_to(&plan, ROOT "/spisor/shared/library", ROOT "/spisor/repos/eeeeeeeeeeee"));
    naula_layout_plan_release(&plan);
}

static naula_reference evil_url_reference = {sizeof(naula_reference), NULL, "https://evil.example/x.git", NULL, 0};

static void a_member_of_by_an_unknown_url_confirms_nothing(void) {
    scenario world;
    start_world(&world);
    world.repos[5].member_of = &evil_url_reference;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "member-of-unknown", ROOT "/weivel/repos/cccccccccccc") != NULL);
    CHECK(links_to(&plan, ROOT "/weivel/records", ROOT "/weivel/repos/cccccccccccc"));
    naula_layout_plan_release(&plan);
}

static void places_a_held_org_without_its_parent_at_the_root(void) {
    scenario world;
    start_world(&world);
    world.orgs[1].parent = &spisor_docs_reference;
    world.dropped[0] = "spisor";
    world.dropped_count = 1;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_MAKE_FOLDER, ROOT "/weivel") != NULL);
    CHECK(links_to(&plan, ROOT "/weivel/records", ROOT "/weivel/repos/cccccccccccc"));
    naula_layout_plan_release(&plan);
}

static void generates_one_import_line_per_org_with_rules(void) {
    scenario world;
    start_world(&world);
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(writes(&plan, ROOT "/CLAUDE.md", "@docs/RULES.md\n"));
    CHECK(writes(&plan, ROOT "/spisor/CLAUDE.md", "@docs/RULES.md\n"));
    CHECK(writes(&plan, ROOT "/weivel/CLAUDE.md", "@records/RULES.md\n"));
    CHECK(count_kind(&plan, NAULA_WRITE_FILE) == 4);
    naula_layout_plan_release(&plan);
}

static void forebay_suborg_imports_through_its_parents_docs(void) {
    scenario world;
    start_world(&world);
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(writes(&plan, ROOT "/spisor/forebay/CLAUDE.md", "@../docs/forebay/CLAUDE.md\n"));
    naula_layout_plan_release(&plan);
}

static void org_without_rules_gets_no_claude_file(void) {
    scenario world;
    start_world(&world);
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_MAKE_FOLDER, ROOT "/pinaxis") != NULL);
    CHECK(find_step(&plan, NAULA_WRITE_FILE, ROOT "/pinaxis/CLAUDE.md") == NULL);
    naula_layout_plan_release(&plan);
}

static void a_listed_suborg_declaring_itself_takes_its_members_from_its_own_declaration(void) {
    scenario world;
    start_world(&world);
    world.orgs[world.org_count] = declaration("forebay", &spisor_docs_reference, forebay_accounts, "RULES.md");
    world.org_repositories[world.org_count++] = git_id(hex_of('7'));
    add_repo(&world, repo_at(hex_of('7'), "records", "forebay", NULL));
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    const naula_layout_step *report = find_report(&plan, "suborg-declares-itself", ROOT "/spisor/forebay");
    CHECK(report != NULL && strstr(report->content, "authorizes") != NULL &&
          strstr(report->content, "supplies its members") != NULL && strstr(report->content, "deleted") == NULL);
    CHECK(find_report(&plan, "suborg-entry-stale", NULL) == NULL);
    CHECK(writes(&plan, ROOT "/spisor/forebay/CLAUDE.md", "@records/RULES.md\n"));
    CHECK(links_to(&plan, ROOT "/spisor/forebay/host", ROOT "/spisor/forebay/repos/ffffffffffff"));
    naula_layout_plan_release(&plan);
}

static void links_by_the_one_project_topic(void) {
    scenario world;
    start_world(&world);
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(links_to(&plan, ROOT "/spisor/shared/library", ROOT "/spisor/repos/eeeeeeeeeeee"));
    CHECK(links_to(&plan, ROOT "/tools/naula", ROOT "/repos/dddddddddddd"));
    CHECK(step_index(&plan, NAULA_MAKE_FOLDER, ROOT "/spisor/shared") <
          step_index(&plan, NAULA_MAKE_LINK, ROOT "/spisor/shared/library"));
    CHECK(find_step(&plan, NAULA_MAKE_FOLDER, ROOT "/spisor/platform") == NULL);
    naula_layout_plan_release(&plan);
}

static void links_at_the_org_top_with_no_project_topic(void) {
    scenario world;
    start_world(&world);
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(links_to(&plan, ROOT "/docs", ROOT "/repos/aaaaaaaaaaaa"));
    CHECK(links_to(&plan, ROOT "/spisor/docs", ROOT "/spisor/repos/bbbbbbbbbbbb"));
    naula_layout_plan_release(&plan);
}

static char *two_topics[] = {"shared", "platform"};

static void two_project_topics_get_no_link(void) {
    scenario world;
    start_world(&world);
    set_topics(add_repo(&world, repo_at(hex_of('1'), "both", "spisor", NULL)), two_topics, 2);
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "two-project-topics", ROOT "/spisor/repos/111111111111") != NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/spisor/shared/both") == NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/spisor/platform/both") == NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/spisor/both") == NULL);
    naula_layout_plan_release(&plan);
}

static void refuses_a_name_colliding_with_repos(void) {
    const char *names[] = {"repos", "archive", "CLAUDE.md", "Repos", "shared", "forebay", ".claude"};
    for (size_t index = 0; index < sizeof names / sizeof names[0]; index++) {
        scenario world;
        start_world(&world);
        add_repo(&world, repo_at(hex_of('1'), names[index], "spisor", NULL));
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
        if (find_report(&plan, "name-collides", ROOT "/spisor/repos/111111111111") == NULL) {
            fprintf(stderr, "  %s was not refused\n", names[index]);
            check_failures++;
        }
        for (size_t step = 0; step < plan.step_count; step++) {
            CHECK(!(plan.steps[step].kind == NAULA_MAKE_LINK &&
                    strcmp(plan.steps[step].target, ROOT "/spisor/repos/111111111111") == 0));
        }
        naula_layout_plan_release(&plan);
    }
}

static void refuses_an_org_folder_colliding_with_repos(void) {
    const char *names[] = {"repos", "Shared", "CLAUDE.md"};
    for (size_t index = 0; index < sizeof names / sizeof names[0]; index++) {
        scenario world;
        start_world(&world);
        forebay_suborg.name = (char *)names[index];
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
        CHECK(check_text(refusal.code, "name-collides", names[index]));
        CHECK(plan.step_count == 0 && plan.steps == NULL);
        forebay_suborg.name = "forebay";
    }
}

static void refuses_an_unsafe_org_or_project_name(void) {
    const char *names[] = {"..", "con", "nul.txt", ".git", "a/b", "trailing.", ""};
    for (size_t index = 0; index < sizeof names / sizeof names[0]; index++) {
        scenario world;
        start_world(&world);
        world.orgs[1].name = (char *)names[index];
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
        CHECK(check_text(refusal.code, "name-invalid", names[index]));
    }
    scenario world;
    start_world(&world);
    char *bad_projects[] = {"..", "platform"};
    world.orgs[0].project_names = bad_projects;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "name-invalid", "project .."));
    start_world(&world);
    world.orgs[1].rules = "../outside/RULES.md";
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "name-invalid", "rules ../outside"));
}

static void a_hostile_repo_name_is_never_a_path(void) {
    const char *names[] = {"..", "../escape", "con", "aux.json", ".git", ".GIT", "x\\y", "dot.", ""};
    for (size_t index = 0; index < sizeof names / sizeof names[0]; index++) {
        scenario world;
        start_world(&world);
        add_repo(&world, repo_at(hex_of('1'), names[index], "spisor", NULL));
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
        if (find_report(&plan, "name-invalid", ROOT "/spisor/repos/111111111111") == NULL) {
            fprintf(stderr, "  %s was not refused\n", names[index]);
            check_failures++;
        }
        for (size_t step = 0; step < plan.step_count; step++) {
            CHECK(!(plan.steps[step].kind == NAULA_MAKE_LINK &&
                    strcmp(plan.steps[step].target, ROOT "/spisor/repos/111111111111") == 0));
        }
        naula_layout_plan_release(&plan);
    }
}

static void names_differing_only_in_case_collide(void) {
    scenario world;
    start_world(&world);
    add_repo(&world, repo_at(hex_of('1'), "Docs", "spisor", NULL));
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "name-collides", ROOT "/spisor/repos/111111111111") != NULL);
    CHECK(find_report(&plan, "name-collides", ROOT "/spisor/repos/bbbbbbbbbbbb") != NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/spisor/docs") == NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/spisor/Docs") == NULL);
    naula_layout_plan_release(&plan);
}

static naula_home shared_account[] = {{sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "Spisor", NULL, NULL}};

static void refuses_an_account_held_by_two_orgs(void) {
    scenario world;
    start_world(&world);
    world.orgs[2].accounts = shared_account;
    world.repos[6].primary.account = "Spisor";
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "account-held-twice", "account"));
    CHECK(strstr(refusal.detail, "spisor") != NULL && strstr(refusal.detail, "pinaxis") != NULL);
    CHECK(plan.step_count == 0);
}

static naula_org_declaration cycle_entries[2];

static void refuses_a_parent_cycle(void) {
    scenario world;
    start_world(&world);
    world.orgs[0].parent = &weivel_records_reference;
    world.orgs[1].parent = &spisor_docs_reference;
    cycle_entries[0] = forebay_suborg;
    cycle_entries[1] = declaration("weivel", NULL, weivel_accounts, NULL);
    world.orgs[0].suborgs = cycle_entries;
    world.orgs[0].suborg_count = 2;
    naula_org_declaration spisor_entry = declaration("spisor", NULL, spisor_accounts, NULL);
    world.orgs[1].suborgs = &spisor_entry;
    world.orgs[1].suborg_count = 1;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "parent-cycle", "cycle"));
    CHECK(strstr(refusal.detail, "spisor") != NULL && strstr(refusal.detail, "weivel") != NULL);
    start_world(&world);
    world.orgs[0].parent = &weivel_records_reference;
    world.orgs[1].parent = &spisor_docs_reference;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "parent-not-listing", ROOT "/spisor") != NULL);
    CHECK(find_report(&plan, "parent-not-listing", ROOT "/weivel") != NULL);
    naula_layout_plan_release(&plan);
    start_world(&world);
    world.orgs[0].parent = &spisor_docs_reference;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "parent-cycle", "own parent"));
}

static void plan_refuses_two_repos_with_one_id(void) {
    scenario world;
    start_world(&world);
    add_repo(&world, repo_at(hex_of('e'), "shelf-data", "spisor", ROOT "/old/shelf-data"));
    world.repos[3].current_path = ROOT "/old/library";
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    const naula_layout_step *first = find_report(&plan, "id-collision", ROOT "/old/library");
    const naula_layout_step *second = find_report(&plan, "id-collision", ROOT "/old/shelf-data");
    CHECK(first != NULL && second != NULL);
    if (first != NULL && second != NULL) {
        CHECK(check_text(first->target, ROOT "/old/shelf-data", "first names the second"));
        CHECK(strstr(first->content, "shelf-data") != NULL && strstr(first->content, "library") != NULL);
    }
    CHECK(count_kind(&plan, NAULA_MOVE_CHECKOUT) == 0);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/spisor/shared/library") == NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/spisor/shelf-data") == NULL);
    naula_layout_plan_release(&plan);
}

static void plan_refuses_a_twelve_hex_collision_naming_both(void) {
    scenario world;
    start_world(&world);
    add_repo(&world, repo_at("0123456789ab0000000000000000000000000000", "first", "spisor", NULL));
    add_repo(&world, repo_at("0123456789abffffffffffffffffffffffffffffffffffffffffffffffffffff", "second", "weivel",
                             ROOT "/old/second"));
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    const naula_layout_step *first = find_report(&plan, "twelve-hex-collision", ROOT "/spisor/repos/0123456789ab");
    const naula_layout_step *second = find_report(&plan, "twelve-hex-collision", ROOT "/old/second");
    CHECK(first != NULL && second != NULL);
    if (first != NULL) {
        CHECK(strstr(first->content, "first") != NULL && strstr(first->content, "second") != NULL);
    }
    CHECK(find_step(&plan, NAULA_MOVE_CHECKOUT, ROOT "/old/second") == NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/spisor/first") == NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/weivel/second") == NULL);
    naula_layout_plan_release(&plan);
}

static naula_home codeberg_spisor[] = {
    {sizeof(naula_home), NAULA_HOME_PROVIDER, "forgejo", "codeberg.org", "spisor", NULL, NULL}};

static void a_mirror_account_never_decides_the_org(void) {
    scenario world;
    start_world(&world);
    world.orgs[2].accounts = codeberg_spisor;
    world.repos[6].primary.provider = "forgejo";
    world.repos[6].primary.host = "codeberg.org";
    world.repos[6].primary.account = "spisor";
    naula_layout_repo *mirrored = add_repo(&world, repo_at(hex_of('1'), "mirrored", "spisor", NULL));
    naula_layout_repo *on_codeberg = add_repo(&world, repo_at(hex_of('2'), "on-codeberg", "spisor", NULL));
    on_codeberg->primary.provider = "forgejo";
    on_codeberg->primary.host = "Codeberg.org";
    naula_layout_repo *elsewhere = add_repo(&world, repo_at(hex_of('3'), "elsewhere", "spisor", ROOT "/old/elsewhere"));
    elsewhere->primary.provider = "forgejo";
    elsewhere->primary.host = "git.example.org";
    (void)mirrored;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(links_to(&plan, ROOT "/spisor/mirrored", ROOT "/spisor/repos/111111111111"));
    CHECK(links_to(&plan, ROOT "/pinaxis/on-codeberg", ROOT "/pinaxis/repos/222222222222"));
    CHECK(find_report(&plan, "not-held", ROOT "/old/elsewhere") != NULL);
    CHECK(find_step(&plan, NAULA_MOVE_CHECKOUT, ROOT "/old/elsewhere") == NULL);
    naula_layout_plan_release(&plan);
}

static void plan_keeps_an_archived_clone_with_unpushed_commits(void) {
    scenario world;
    start_world(&world);
    naula_layout_repo *old = add_repo(&world, repo_at(hex_of('1'), "backend", "spisor", ROOT "/old/backend"));
    old->archived = 1;
    old->clean_and_pushed = 0;
    add_link(&world, ROOT "/spisor/backend", ROOT "/old/backend");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_REMOVE_CLONE, ROOT "/old/backend") == NULL);
    const naula_layout_step *move = find_step(&plan, NAULA_MOVE_CHECKOUT, ROOT "/old/backend");
    CHECK(move != NULL && strcmp(move->target, ROOT "/spisor/repos/111111111111") == 0);
    CHECK(links_to(&plan, ROOT "/spisor/archive/backend", ROOT "/spisor/repos/111111111111"));
    CHECK(find_step(&plan, NAULA_MAKE_FOLDER, ROOT "/spisor/archive") != NULL);
    CHECK(find_report(&plan, "archived-kept-dirty", ROOT "/spisor/repos/111111111111") != NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/spisor/backend") == NULL);
    const naula_layout_step *old_link = find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/backend");
    CHECK(old_link != NULL && strcmp(old_link->target, ROOT "/old/backend") == 0);
    naula_layout_plan_release(&plan);
}

static void plan_removes_a_clean_pushed_archived_clone(void) {
    scenario world;
    start_world(&world);
    naula_layout_repo *old =
        add_repo(&world, repo_at(hex_of('1'), "backend", "spisor", ROOT "/spisor/repos/111111111111"));
    old->archived = 1;
    old->clean_and_pushed = 1;
    naula_layout_repo *never_cloned = add_repo(&world, repo_at(hex_of('2'), "argocd", "spisor", NULL));
    never_cloned->archived = 1;
    add_link(&world, ROOT "/spisor/archive/backend", ROOT "/spisor/repos/111111111111");
    add_foreign_link(&world, ROOT "/spisor/backend", ROOT "/spisor/repos/111111111111");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_REMOVE_CLONE, ROOT "/spisor/repos/111111111111") != NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/archive/backend") != NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/spisor/archive/backend") == NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/backend") == NULL);
    CHECK(step_index(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/archive/backend") <
          step_index(&plan, NAULA_REMOVE_CLONE, ROOT "/spisor/repos/111111111111"));
    for (size_t index = 0; index < plan.step_count; index++) {
        CHECK(plan.steps[index].path == NULL || strstr(plan.steps[index].path, "222222222222") == NULL);
        CHECK(plan.steps[index].path == NULL || strstr(plan.steps[index].path, "argocd") == NULL);
    }
    naula_layout_plan_release(&plan);
}

static void never_removes_a_clone_holding_another_checkout(void) {
    scenario world;
    start_world(&world);
    naula_layout_repo *old = add_repo(&world, repo_at(hex_of('1'), "kendo", "spisor", ROOT "/store/kendo"));
    old->archived = 1;
    old->clean_and_pushed = 1;
    add_repo(&world, repo_at(hex_of('2'), "inner", "nobody", ROOT "/store/kendo/inner"));
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_REMOVE_CLONE, ROOT "/store/kendo") == NULL);
    CHECK(find_step(&plan, NAULA_MOVE_CHECKOUT, ROOT "/store/kendo") == NULL);
    CHECK(find_report(&plan, "path-occupied", ROOT "/store/kendo") != NULL);
    CHECK(find_report(&plan, "not-held", ROOT "/store/kendo/inner") != NULL);
    naula_layout_plan_release(&plan);
}

static void places_an_unadopted_repo_from_its_host_with_a_note(void) {
    scenario world;
    start_world(&world);
    naula_layout_repo *fork = add_repo(&world, repo_at(hex_of('1'), "SensibleFork", "spisor", NULL));
    set_topics(fork, library_topics, 1);
    fork->adopted = 0;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(links_to(&plan, ROOT "/spisor/shared/SensibleFork", ROOT "/spisor/repos/111111111111"));
    CHECK(find_report(&plan, "unadopted", ROOT "/spisor/repos/111111111111") != NULL);
    naula_layout_plan_release(&plan);
}

static void a_first_shape_repo_reads_as_adopted(void) {
    scenario world;
    start_world(&world);
    for (size_t index = 0; index < world.repo_count; index++) {
        world.repos[index].size = (uint32_t)(offsetof(naula_layout_repo, current_path) + sizeof(char *));
        world.repos[index].adopted = 0;
    }
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "unadopted", NULL) == NULL);
    naula_layout_plan_release(&plan);
    world.repos[0].size = 8;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "layout-input-invalid", "short repo"));
}

static void a_dropped_org_is_not_laid_out(void) {
    scenario world;
    start_world(&world);
    world.dropped[0] = "Weivel";
    world.dropped[1] = "forebay";
    world.dropped_count = 2;
    world.repos[5].current_path = ROOT "/old/records";
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    for (size_t index = 0; index < plan.step_count; index++) {
        const naula_layout_step *step = &plan.steps[index];
        if (step->kind != NAULA_REPORT) {
            CHECK(strstr(step->path, "weivel") == NULL && strstr(step->path, "forebay") == NULL);
        }
    }
    CHECK(find_report(&plan, "not-held", ROOT "/old/records") != NULL);
    CHECK(find_step(&plan, NAULA_MOVE_CHECKOUT, ROOT "/old/records") == NULL);
    naula_layout_plan_release(&plan);
}

static naula_reference sensible_reference = {sizeof(naula_reference), "git:5555555555555555555555555555555555555555",
                                             NULL, NULL, 0};

static void an_extra_repo_is_kept_in_its_named_org(void) {
    scenario world;
    start_world(&world);
    add_repo(&world, repo_at(hex_of('5'), "SensibleToolbox", "Slimefun-Addon-Community", NULL));
    add_repo(&world, repo_at(hex_of('6'), "stranger", "someone", "T:/elsewhere/stranger"));
    world.extra[0] = sensible_reference;
    world.extra_orgs[0] = "weivel";
    world.extra_count = 1;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(links_to(&plan, ROOT "/weivel/SensibleToolbox", ROOT "/weivel/repos/555555555555"));
    CHECK(find_report(&plan, "not-held", "T:/elsewhere/stranger") != NULL);
    CHECK(find_step(&plan, NAULA_MOVE_CHECKOUT, "T:/elsewhere/stranger") == NULL);
    naula_layout_plan_release(&plan);
}

static void a_homeless_repo_is_kept_local_without_a_link(void) {
    scenario world;
    start_world(&world);
    naula_layout_repo *loose = add_repo(&world, repo_at(hex_of('1'), "scratch", "spisor", ROOT "/spisor/scratch"));
    memset(&loose->primary, 0, sizeof loose->primary);
    loose->primary.size = sizeof loose->primary;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    const naula_layout_step *move = find_step(&plan, NAULA_MOVE_CHECKOUT, ROOT "/spisor/scratch");
    CHECK(move != NULL && strcmp(move->target, ROOT "/spisor/repos/local-scratch") == 0);
    for (size_t index = 0; index < plan.step_count; index++) {
        CHECK(!(plan.steps[index].kind == NAULA_MAKE_LINK && strstr(plan.steps[index].target, "local-") != NULL));
    }
    naula_layout_plan_release(&plan);
    loose->current_path = ROOT "/spisor/repos/local-scratch";
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(count_kind(&plan, NAULA_MOVE_CHECKOUT) == 0);
    naula_layout_plan_release(&plan);
}

static void a_homeless_repo_outside_the_root_is_left_alone(void) {
    scenario world;
    start_world(&world);
    naula_layout_repo *loose = add_repo(&world, repo_at(hex_of('1'), "scratch", "spisor", "T:/elsewhere/scratch"));
    memset(&loose->primary, 0, sizeof loose->primary);
    loose->primary.size = sizeof loose->primary;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_MOVE_CHECKOUT, "T:/elsewhere/scratch") == NULL);
    CHECK(find_report(&plan, "not-held", "T:/elsewhere/scratch") != NULL);
    naula_layout_plan_release(&plan);
}

static void two_repos_claiming_one_checkout_are_left_alone(void) {
    scenario world;
    start_world(&world);
    world.repos[3].current_path = ROOT "/spisor/repos/eeeeeeeeeeee";
    add_repo(&world, repo_at(hex_of('1'), "impostor", "spisor", ROOT "/spisor/repos/eeeeeeeeeeee"));
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_MOVE_CHECKOUT, ROOT "/spisor/repos/eeeeeeeeeeee") == NULL);
    CHECK(find_report(&plan, "path-occupied", ROOT "/spisor/repos/eeeeeeeeeeee") != NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/spisor/impostor") == NULL);
    naula_layout_plan_release(&plan);
}

static void moves_a_checkout_out_of_its_link_path_before_linking(void) {
    scenario world;
    start_world(&world);
    world.repos[0].current_path = ROOT "/docs";
    world.repos[2].current_path = ROOT "/spisor/docs";
    world.repos[3].current_path = ROOT "/spisor/shared/library";
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    size_t top_move = step_index(&plan, NAULA_MOVE_CHECKOUT, ROOT "/docs");
    size_t top_link = step_index(&plan, NAULA_MAKE_LINK, ROOT "/docs");
    size_t docs_move = step_index(&plan, NAULA_MOVE_CHECKOUT, ROOT "/spisor/docs");
    size_t docs_link = step_index(&plan, NAULA_MAKE_LINK, ROOT "/spisor/docs");
    size_t library_move = step_index(&plan, NAULA_MOVE_CHECKOUT, ROOT "/spisor/shared/library");
    size_t library_link = step_index(&plan, NAULA_MAKE_LINK, ROOT "/spisor/shared/library");
    size_t project_folder = step_index(&plan, NAULA_MAKE_FOLDER, ROOT "/spisor/shared");
    size_t repos_folder = step_index(&plan, NAULA_MAKE_FOLDER, ROOT "/spisor/repos");
    CHECK(top_move != (size_t)-1 && top_link != (size_t)-1 && top_move < top_link);
    CHECK(docs_move != (size_t)-1 && docs_link != (size_t)-1 && docs_move < docs_link);
    CHECK(library_move != (size_t)-1 && project_folder != (size_t)-1 && library_link != (size_t)-1);
    CHECK(library_move < project_folder && project_folder < library_link);
    CHECK(repos_folder < docs_move);
    naula_layout_plan_release(&plan);
}

static void swapped_checkouts_are_reported_not_moved(void) {
    scenario world;
    start_world(&world);
    world.repos[2].current_path = ROOT "/spisor/repos/eeeeeeeeeeee";
    world.repos[3].current_path = ROOT "/spisor/repos/bbbbbbbbbbbb";
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(count_kind(&plan, NAULA_MOVE_CHECKOUT) == 0);
    CHECK(find_report(&plan, "path-occupied", ROOT "/spisor/repos/eeeeeeeeeeee") != NULL);
    CHECK(find_report(&plan, "path-occupied", ROOT "/spisor/repos/bbbbbbbbbbbb") != NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/spisor/docs") == NULL);
    naula_layout_plan_release(&plan);
}

static void a_checkout_moves_out_of_the_way_before_one_moves_in(void) {
    scenario world;
    start_world(&world);
    world.repos[2].current_path = ROOT "/spisor/repos/eeeeeeeeeeee";
    world.repos[3].current_path = ROOT "/old/library";
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    size_t out = step_index(&plan, NAULA_MOVE_CHECKOUT, ROOT "/spisor/repos/eeeeeeeeeeee");
    size_t in = step_index(&plan, NAULA_MOVE_CHECKOUT, ROOT "/old/library");
    CHECK(out != (size_t)-1 && in != (size_t)-1 && out < in);
    naula_layout_plan_release(&plan);
}

static void refuses_a_checkout_holding_an_org_folder(void) {
    scenario world;
    start_world(&world);
    world.repos[3].current_path = ROOT "/spisor";
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "path-occupied", "org folder"));
    start_world(&world);
    add_repo(&world, repo_at(hex_of('1'), "stray", "nobody", "T:/"));
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "path-occupied", "root"));
}

static int same_text(const char *left, const char *right) {
    return (left == NULL && right == NULL) || (left != NULL && right != NULL && strcmp(left, right) == 0);
}

static int same_step(const naula_layout_step *left, const naula_layout_step *right) {
    return left->kind == right->kind && same_text(left->path, right->path) && same_text(left->target, right->target) &&
           same_text(left->content, right->content) && strcmp(left->code, right->code) == 0;
}

static int plans_equal(const naula_layout_plan *left, const naula_layout_plan *right) {
    if (left->step_count != right->step_count) {
        return 0;
    }
    for (size_t index = 0; index < left->step_count; index++) {
        if (!same_step(&left->steps[index], &right->steps[index])) {
            return 0;
        }
    }
    return 1;
}

static void lay_out(scenario *world, const naula_layout_plan *plan) {
    for (size_t index = 0; index < plan->step_count; index++) {
        const naula_layout_step *step = &plan->steps[index];
        for (size_t repo = 0; repo < world->repo_count; repo++) {
            const char *current = world->repos[repo].current_path;
            if (current == NULL || strcmp(current, step->path) != 0) {
                continue;
            }
            if (step->kind == NAULA_MOVE_CHECKOUT) {
                world->repos[repo].current_path = step->target;
            } else if (step->kind == NAULA_REMOVE_CLONE) {
                world->repos[repo].current_path = NULL;
            }
        }
    }
}

static int is_compared(uint32_t kind) {
    return kind == NAULA_MOVE_CHECKOUT || kind == NAULA_WRITE_FILE || kind == NAULA_MAKE_LINK;
}

static int first_contains_each_compared_step(const naula_layout_plan *first, const naula_layout_plan *second) {
    int contained = 1;
    for (size_t index = 0; index < second->step_count; index++) {
        const naula_layout_step *step = &second->steps[index];
        if (!is_compared(step->kind)) {
            continue;
        }
        int found = 0;
        for (size_t other = 0; other < first->step_count && !found; other++) {
            found = same_step(step, &first->steps[other]);
        }
        if (!found) {
            fprintf(stderr, "  second plan adds or changes step %u at %s\n", step->kind, step->path);
            contained = 0;
        }
    }
    return contained;
}

static void plan_is_stable_for_unchanged_input(void) {
    scenario world;
    start_world(&world);
    world.repos[0].current_path = ROOT "/old/intisy-docs";
    add_link(&world, ROOT "/spisor/shared/lib", ROOT "/spisor/repos/eeeeeeeeeeee");
    world.repos[2].current_path = ROOT "/spisor/docs";
    world.repos[3].current_path = ROOT "/spisor/shared/library";
    naula_layout_repo *old = add_repo(&world, repo_at(hex_of('1'), "backend", "spisor", ROOT "/old/backend"));
    old->archived = 1;
    naula_layout_repo *gone = add_repo(&world, repo_at(hex_of('2'), "argocd", "spisor", ROOT "/old/argocd"));
    gone->archived = 1;
    gone->clean_and_pushed = 1;
    naula_layout_plan first;
    naula_refusal refusal;
    CHECK(plan_world(&world, &first, &refusal) == NAULA_OK);
    CHECK(count_kind(&first, NAULA_MOVE_CHECKOUT) == 4);
    naula_layout_plan again;
    CHECK(plan_world(&world, &again, &refusal) == NAULA_OK);
    CHECK(plans_equal(&first, &again));
    naula_layout_plan_release(&again);

    CHECK(find_step(&first, NAULA_REMOVE_LINK, ROOT "/spisor/shared/lib") != NULL);
    lay_out(&world, &first);
    world.link_count = 0;
    for (size_t index = 0; index < first.step_count && world.link_count < 8; index++) {
        if (first.steps[index].kind == NAULA_MAKE_LINK) {
            add_link(&world, first.steps[index].path, first.steps[index].target);
        }
    }
    naula_layout_plan second;
    CHECK(plan_world(&world, &second, &refusal) == NAULA_OK);
    CHECK(count_kind(&second, NAULA_MOVE_CHECKOUT) == 0);
    CHECK(count_kind(&second, NAULA_REMOVE_CLONE) == 0);
    for (size_t index = 0; index < second.step_count; index++) {
        const naula_layout_step *step = &second.steps[index];
        const naula_layout_step *made =
            step->kind == NAULA_REMOVE_LINK ? find_step(&second, NAULA_MAKE_LINK, step->path) : NULL;
        CHECK(made == NULL || strcmp(made->target, step->target) != 0);
        CHECK(!(step->kind == NAULA_REMOVE_LINK && strcmp(step->path, ROOT "/spisor/shared/library") == 0));
    }
    CHECK(first_contains_each_compared_step(&first, &second));
    CHECK(count_kind(&second, NAULA_MAKE_LINK) == count_kind(&first, NAULA_MAKE_LINK));
    CHECK(count_kind(&second, NAULA_WRITE_FILE) == count_kind(&first, NAULA_WRITE_FILE));
    naula_layout_plan third;
    CHECK(plan_world(&world, &third, &refusal) == NAULA_OK);
    CHECK(plans_equal(&second, &third));
    naula_layout_plan_release(&third);
    naula_layout_plan_release(&second);
    naula_layout_plan_release(&first);
}

static void insert_first(scenario *world, naula_layout_repo repo) {
    memmove(&world->repos[1], &world->repos[0], world->repo_count * sizeof world->repos[0]);
    world->repos[0] = repo;
    world->repo_count++;
}

static void a_copy_of_a_declaring_repo_elsewhere_takes_no_account(void) {
    for (int first = 0; first < 2; first++) {
        scenario world;
        start_world(&world);
        world.repos[4].member_of = &spisor_docs_reference;
        naula_layout_repo copy = repo_at(hex_of('b'), "docs", "evil", NULL);
        if (first) {
            insert_first(&world, copy);
        } else {
            add_repo(&world, copy);
        }
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
        CHECK(find_report(&plan, "account-not-owned", ROOT "/spisor") == NULL);
        CHECK(links_to(&plan, ROOT "/spisor/shared/library", ROOT "/spisor/repos/eeeeeeeeeeee"));
        CHECK(links_to(&plan, ROOT "/spisor/forebay/host", ROOT "/spisor/forebay/repos/ffffffffffff"));
        naula_layout_plan_release(&plan);
    }
}

static void plan_does_not_depend_on_input_order_with_a_copy(void) {
    scenario before;
    start_world(&before);
    insert_first(&before, repo_at(hex_of('b'), "docs", "evil", NULL));
    scenario after;
    start_world(&after);
    add_repo(&after, repo_at(hex_of('b'), "docs", "evil", NULL));
    naula_layout_plan first;
    naula_layout_plan last;
    naula_refusal refusal;
    CHECK(plan_world(&before, &first, &refusal) == NAULA_OK);
    CHECK(plan_world(&after, &last, &refusal) == NAULA_OK);
    CHECK(plans_equal(&first, &last));
    naula_layout_plan_release(&first);
    naula_layout_plan_release(&last);
}

static naula_home only_stranger_account[] = {
    {sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "stranger", NULL, NULL}};

static void a_stranger_named_like_a_listed_suborg_lands_at_the_root(void) {
    scenario world;
    start_world(&world);
    world.repos[4].member_of = &spisor_docs_reference;
    world.repos[4].current_path = ROOT "/spisor/forebay/repos/ffffffffffff";
    world.orgs[world.org_count] = declaration("forebay", &spisor_docs_reference, only_stranger_account, "RULES.md");
    world.org_repositories[world.org_count++] = git_id(hex_of('7'));
    add_repo(&world, repo_at(hex_of('7'), "records", "stranger", NULL));
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "parent-not-listing", ROOT "/forebay") != NULL);
    CHECK(find_report(&plan, "suborg-declares-itself", NULL) == NULL);
    CHECK(writes(&plan, ROOT "/spisor/forebay/CLAUDE.md", "@../docs/forebay/CLAUDE.md\n"));
    CHECK(links_to(&plan, ROOT "/spisor/forebay/host", ROOT "/spisor/forebay/repos/ffffffffffff"));
    CHECK(find_step(&plan, NAULA_MOVE_CHECKOUT, ROOT "/spisor/forebay/repos/ffffffffffff") == NULL);
    CHECK(links_to(&plan, ROOT "/forebay/records", ROOT "/forebay/repos/777777777777"));
    naula_layout_plan_release(&plan);
}

static void plan_does_not_depend_on_input_order(void) {
    scenario world;
    start_world(&world);
    naula_layout_plan forward;
    naula_refusal refusal;
    CHECK(plan_world(&world, &forward, &refusal) == NAULA_OK);
    for (size_t index = 0; index < world.repo_count / 2; index++) {
        naula_layout_repo swap = world.repos[index];
        world.repos[index] = world.repos[world.repo_count - 1 - index];
        world.repos[world.repo_count - 1 - index] = swap;
    }
    naula_layout_plan backward;
    CHECK(plan_world(&world, &backward, &refusal) == NAULA_OK);
    CHECK(plans_equal(&forward, &backward));
    naula_layout_plan_release(&forward);
    naula_layout_plan_release(&backward);
}


static naula_status plan_with_root(scenario *world, const char *root, naula_layout_plan *plan, naula_refusal *refusal) {
    memset(plan, 0, sizeof *plan);
    plan->size = sizeof *plan;
    memset(refusal, 0, sizeof *refusal);
    refusal->size = sizeof *refusal;
    naula_layout_input input = input_of(world);
    input.root = root;
    return naula_layout_plan_make(&input, plan, refusal);
}

static void refuses_an_unclean_root_or_current_path(void) {
    const char *roots[] = {"", "relative/root", "root", "T:root", "T:/root/../x", "T:/root/./x", "T:/root//x",
                           "T:/a:b", "/a/../b"};
    for (size_t index = 0; index < sizeof roots / sizeof roots[0]; index++) {
        scenario world;
        start_world(&world);
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_with_root(&world, roots[index], &plan, &refusal) == NAULA_REFUSED);
        CHECK(check_text(refusal.code, "layout-input-invalid", roots[index]));
    }
    const char *currents[] = {"relative/x", ROOT "/spisor/repos/../../../Users", ROOT "/x/./y", ROOT "//x",
                              ROOT "/x:stream", ""};
    for (size_t index = 0; index < sizeof currents / sizeof currents[0]; index++) {
        scenario world;
        start_world(&world);
        add_repo(&world, repo_at(hex_of('1'), "stray", "nobody", currents[index]));
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
        CHECK(check_text(refusal.code, "layout-input-invalid", currents[index]));
    }
    scenario world;
    start_world(&world);
    add_link(&world, "relative/link", ROOT "/repos/aaaaaaaaaaaa");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "layout-input-invalid", "link path"));
    start_world(&world);
    add_link(&world, ROOT "/docs", ROOT "/repos/../../x");
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "layout-input-invalid", "link target"));
}

static void a_held_repo_outside_the_root_is_left_alone(void) {
    scenario world;
    start_world(&world);
    world.repos[1].current_path = "T:/old/naula";
    naula_layout_repo *old = add_repo(&world, repo_at(hex_of('1'), "backend", "spisor", "C:/Users/finn"));
    old->archived = 1;
    old->clean_and_pushed = 1;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_MOVE_CHECKOUT, "T:/old/naula") == NULL);
    CHECK(find_report(&plan, "path-outside-root", "T:/old/naula") != NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/tools/naula") == NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_CLONE, "C:/Users/finn") == NULL);
    CHECK(find_report(&plan, "path-outside-root", "C:/Users/finn") != NULL);
    for (size_t index = 0; index < plan.step_count; index++) {
        const naula_layout_step *step = &plan.steps[index];
        if (step->kind == NAULA_MOVE_CHECKOUT || step->kind == NAULA_REMOVE_CLONE || step->kind == NAULA_REMOVE_LINK) {
            CHECK(strncmp(step->path, ROOT "/", strlen(ROOT "/")) == 0);
        }
    }
    naula_layout_plan_release(&plan);
}

static void a_filesystem_root_is_refused_as_the_root(void) {
    const char *roots[] = {"/", "T:/", "T:\\", "T://"};
    for (size_t index = 0; index < sizeof roots / sizeof roots[0]; index++) {
        scenario world;
        start_world(&world);
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_with_root(&world, roots[index], &plan, &refusal) == NAULA_REFUSED);
        CHECK(check_text(refusal.code, "layout-input-invalid", roots[index]));
        CHECK(plan.step_count == 0);
    }
}

static void never_removes_or_moves_a_clone_at_a_naula_made_path(void) {
    const char *removed_at[] = {ROOT "/spisor/archive/backend", ROOT "/spisor/shared", ROOT "/spisor/backend",
                                ROOT "/spisor/shared/library"};
    for (size_t index = 0; index < sizeof removed_at / sizeof removed_at[0]; index++) {
        scenario world;
        start_world(&world);
        naula_layout_repo *old = add_repo(&world, repo_at(hex_of('1'), "backend", "spisor", removed_at[index]));
        old->archived = 1;
        old->clean_and_pushed = 1;
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
        if (find_step(&plan, NAULA_REMOVE_CLONE, removed_at[index]) != NULL) {
            fprintf(stderr, "  the clone at %s was removed\n", removed_at[index]);
            check_failures++;
        }
        CHECK(find_report(&plan, "path-occupied", removed_at[index]) != NULL);
        naula_layout_plan_release(&plan);
    }
    const char *moved_from[] = {ROOT "/spisor/shared", ROOT "/spisor/archive/x", ROOT "/spisor/archive"};
    for (size_t index = 0; index < sizeof moved_from / sizeof moved_from[0]; index++) {
        scenario world;
        start_world(&world);
        add_repo(&world, repo_at(hex_of('1'), "mover", "spisor", moved_from[index]));
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
        if (find_step(&plan, NAULA_MOVE_CHECKOUT, moved_from[index]) != NULL) {
            fprintf(stderr, "  the checkout at %s was moved\n", moved_from[index]);
            check_failures++;
        }
        CHECK(find_report(&plan, "path-occupied", moved_from[index]) != NULL);
        naula_layout_plan_release(&plan);
    }
}

static void a_foreign_checkout_in_a_target_folder_blocks_the_repo(void) {
    scenario world;
    start_world(&world);
    add_repo(&world, repo_at(hex_of('5'), "docs", "evil", ROOT "/repos/aaaaaaaaaaaa"));
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/docs") == NULL);
    CHECK(find_step(&plan, NAULA_WRITE_FILE, ROOT "/CLAUDE.md") == NULL);
    CHECK(find_report(&plan, "path-occupied", ROOT "/repos/aaaaaaaaaaaa") != NULL);
    naula_layout_plan_release(&plan);

    start_world(&world);
    add_repo(&world, repo_at(hex_of('5'), "inner", "evil", ROOT "/repos/dddddddddddd/inner"));
    world.repos[1].current_path = ROOT "/old/naula";
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_MOVE_CHECKOUT, ROOT "/old/naula") == NULL);
    CHECK(find_report(&plan, "path-occupied", ROOT "/old/naula") != NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/tools/naula") == NULL);
    naula_layout_plan_release(&plan);
}

static void a_checkout_nested_in_another_is_left_alone(void) {
    scenario world;
    start_world(&world);
    world.repos[2].current_path = ROOT "/spisor/repos/bbbbbbbbbbbb";
    world.repos[3].current_path = ROOT "/spisor/repos/bbbbbbbbbbbb/siblings/library";
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(count_kind(&plan, NAULA_MOVE_CHECKOUT) == 0);
    CHECK(find_report(&plan, "nested-checkout", ROOT "/spisor/repos/bbbbbbbbbbbb/siblings/library") != NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/spisor/shared/library") == NULL);
    CHECK(links_to(&plan, ROOT "/spisor/docs", ROOT "/spisor/repos/bbbbbbbbbbbb"));
    naula_layout_plan_release(&plan);

    start_world(&world);
    world.repos[3].current_path = ROOT "/old/library";
    world.repos[2].current_path = ROOT "/old/library/docs";
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(count_kind(&plan, NAULA_MOVE_CHECKOUT) == 0);
    CHECK(find_report(&plan, "nested-checkout", ROOT "/old/library/docs") != NULL);
    CHECK(find_report(&plan, "path-occupied", ROOT "/old/library") != NULL);
    naula_layout_plan_release(&plan);
}

static void removes_the_link_left_by_a_rename(void) {
    scenario world;
    start_world(&world);
    add_link(&world, ROOT "/spisor/shared/lib", ROOT "/spisor/repos/eeeeeeeeeeee");
    add_link(&world, ROOT "/spisor/shared/library", ROOT "/spisor/repos/eeeeeeeeeeee");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    const naula_layout_step *removal = find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/shared/lib");
    CHECK(removal != NULL && strcmp(removal->target, ROOT "/spisor/repos/eeeeeeeeeeee") == 0);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/shared/library") == NULL);
    CHECK(step_index(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/shared/lib") <
          step_index(&plan, NAULA_MAKE_LINK, ROOT "/spisor/shared/library"));
    naula_layout_plan_release(&plan);
}

static void removes_the_link_left_by_a_topic_change(void) {
    scenario world;
    start_world(&world);
    add_link(&world, ROOT "/spisor/platform/library", ROOT "/spisor/repos/eeeeeeeeeeee");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/platform/library") != NULL);
    naula_layout_plan_release(&plan);
}

static void removes_the_links_of_a_dropped_org(void) {
    scenario world;
    start_world(&world);
    world.dropped[0] = "weivel";
    world.dropped_count = 1;
    add_link(&world, ROOT "/weivel/records", ROOT "/weivel/repos/cccccccccccc");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/weivel/records") != NULL);
    CHECK(count_kind(&plan, NAULA_REMOVE_CLONE) == 0);
    naula_layout_plan_release(&plan);
}

static void removes_the_links_of_a_renamed_org(void) {
    scenario world;
    start_world(&world);
    add_link(&world, ROOT "/oldspisor/docs", ROOT "/oldspisor/repos/bbbbbbbbbbbb");
    add_link(&world, ROOT "/oldspisor/forebay/host", ROOT "/oldspisor/forebay/repos/local-host");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/oldspisor/docs") != NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/oldspisor/forebay/host") != NULL);
    naula_layout_plan_release(&plan);
}

static void a_checkout_reached_through_an_existing_link_is_left_alone(void) {
    scenario world;
    start_world(&world);
    world.repos[0].current_path = ROOT "/repos/aaaaaaaaaaaa";
    naula_layout_repo *old = add_repo(&world, repo_at(hex_of('1'), "backend", "spisor", ROOT "/oldlink/x"));
    old->archived = 1;
    old->clean_and_pushed = 1;
    add_link(&world, ROOT "/oldlink", ROOT "/repos/aaaaaaaaaaaa");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_REMOVE_CLONE, ROOT "/oldlink/x") == NULL);
    CHECK(find_report(&plan, "under-existing-link", ROOT "/oldlink/x") != NULL);
    naula_layout_plan_release(&plan);

    start_world(&world);
    world.repos[0].current_path = ROOT "/repos/aaaaaaaaaaaa";
    world.repos[3].current_path = ROOT "/docs/nested";
    world.repos[2].current_path = ROOT "/docs";
    add_link(&world, ROOT "/docs", ROOT "/repos/aaaaaaaaaaaa");
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_MOVE_CHECKOUT, ROOT "/docs/nested") == NULL);
    CHECK(find_report(&plan, "under-existing-link", ROOT "/docs/nested") != NULL);
    CHECK(find_step(&plan, NAULA_MOVE_CHECKOUT, ROOT "/docs") == NULL);
    CHECK(find_report(&plan, "under-existing-link", ROOT "/docs") != NULL);
    naula_layout_plan_release(&plan);
}

static void removes_only_links_in_the_places_naula_links(void) {
    scenario world;
    start_world(&world);
    world.repos[0].current_path = ROOT "/repos/aaaaaaaaaaaa";
    add_foreign_link(&world, ROOT "/views/naula", ROOT "/repos/dddddddddddd");
    add_link(&world, ROOT "/repos/aaaaaaaaaaaa/siblings/naula", ROOT "/repos/dddddddddddd");
    add_link(&world, ROOT "/some/repos/x", ROOT "/some/repos/x2");
    add_link(&world, ROOT "/spisor/shared/deep/lib", ROOT "/spisor/repos/eeeeeeeeeeee");
    add_link(&world, ROOT "/spisor/archive/old", ROOT "/spisor/repos/111111111111");
    add_link(&world, ROOT "/tools/naula-old", ROOT "/repos/dddddddddddd");
    add_link(&world, ROOT "/elsewhere/repos/k/x", ROOT "/elsewhere/repos/k/repos/222222222222");
    add_link(&world, ROOT "/spisor/forebay/lib", ROOT "/spisor/repos/eeeeeeeeeeee");
    add_link(&world, ROOT "/spisor/shared/odd", ROOT "/spisor/repos/not-a-checkout");
    add_link(&world, ROOT "/spisor/shared/upper", ROOT "/spisor/repos/EEEEEEEEEEEE");
    add_repo(&world, repo_at(hex_of('3'), "toolbox", "intisy", ROOT "/spisor/platform"));
    add_link(&world, ROOT "/spisor/platform/thing", ROOT "/spisor/repos/999999999999");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/views/naula") == NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/forebay/lib") == NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/shared/odd") == NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/shared/upper") == NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/repos/aaaaaaaaaaaa/siblings/naula") == NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/some/repos/x") == NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/shared/deep/lib") == NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/archive/old") != NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/tools/naula-old") != NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/elsewhere/repos/k/x") == NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/platform/thing") == NULL);
    naula_layout_plan_release(&plan);
}

static void removes_only_links_naula_recorded_making(void) {
    scenario world;
    start_world(&world);
    add_link(&world, ROOT "/views/naula", ROOT "/repos/dddddddddddd");
    add_foreign_link(&world, ROOT "/spisor/old/library", ROOT "/spisor/repos/eeeeeeeeeeee");
    add_foreign_link(&world, ROOT "/spisor/docs-old", ROOT "/spisor/repos/bbbbbbbbbbbb");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/views/naula") != NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/old/library") == NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/docs-old") == NULL);
    naula_layout_plan_release(&plan);
}

static void a_foreign_link_where_naula_links_is_reported_and_left(void) {
    const char *targets[] = {ROOT "/spisor/repos/eeeeeeeeeeee", "T:/somewhere/else"};
    for (size_t index = 0; index < 2; index++) {
        scenario world;
        start_world(&world);
        add_foreign_link(&world, ROOT "/spisor/shared/library", targets[index]);
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
        CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/spisor/shared/library") == NULL);
        CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/shared/library") == NULL);
        CHECK(find_report(&plan, "path-occupied", ROOT "/spisor/shared/library") != NULL);
        CHECK(links_to(&plan, ROOT "/spisor/docs", ROOT "/spisor/repos/bbbbbbbbbbbb"));
        naula_layout_plan_release(&plan);
    }
}

static void rules_never_load_through_a_foreign_link(void) {
    const char *targets[] = {ROOT "/spisor/repos/bbbbbbbbbbbb", "T:/evil/rules"};
    for (size_t index = 0; index < 2; index++) {
        scenario world;
        start_world(&world);
        world.repos[2].current_path = ROOT "/spisor/repos/bbbbbbbbbbbb";
        add_foreign_link(&world, ROOT "/spisor/docs", targets[index]);
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
        CHECK(writes(&plan, ROOT "/spisor/CLAUDE.md", "@repos/bbbbbbbbbbbb/RULES.md\n"));
        CHECK(writes(&plan, ROOT "/spisor/forebay/CLAUDE.md", "@../repos/bbbbbbbbbbbb/forebay/CLAUDE.md\n"));
        naula_layout_plan_release(&plan);

        world.repos[2].current_path = NULL;
        CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
        CHECK(find_step(&plan, NAULA_WRITE_FILE, ROOT "/spisor/CLAUDE.md") == NULL);
        CHECK(find_report(&plan, "rules-repo-missing", ROOT "/spisor") != NULL);
        naula_layout_plan_release(&plan);
    }
}

static void leaves_links_naula_did_not_make(void) {
    scenario world;
    start_world(&world);
    add_link(&world, ROOT "/mine", "T:/somewhere/else");
    add_link(&world, ROOT "/tools/thing", ROOT "/notrepos/thing");
    add_link(&world, "T:/other/link", ROOT "/spisor/repos/eeeeeeeeeeee");
    add_link(&world, ROOT "/r", ROOT "/repos");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(count_kind(&plan, NAULA_REMOVE_LINK) == 0);
    naula_layout_plan_release(&plan);
}

static void a_current_path_differing_only_in_case_is_in_place(void) {
    scenario world;
    start_world(&world);
    world.repos[2].current_path = "t:/ROOT/spisor/repos/BBBBBBBBBBBB";
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(count_kind(&plan, NAULA_MOVE_CHECKOUT) == 0);
    naula_layout_plan_release(&plan);
}

static void a_checkout_inside_its_own_target_is_left_alone(void) {
    scenario world;
    start_world(&world);
    world.repos[1].current_path = ROOT "/repos/dddddddddddd/x";
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(count_kind(&plan, NAULA_MOVE_CHECKOUT) == 0);
    CHECK(find_report(&plan, "path-occupied", ROOT "/repos/dddddddddddd/x") != NULL);
    naula_layout_plan_release(&plan);
}

static void an_extra_naming_an_unknown_or_ambiguous_org_is_reported(void) {
    const char *names[] = {"nosuch", "weivel"};
    for (size_t index = 0; index < 2; index++) {
        scenario world;
        start_world(&world);
        if (index == 1) {
            forebay_suborg.name = "weivel";
        }
        add_repo(&world, repo_at(hex_of('5'), "SensibleToolbox", "Slimefun-Addon-Community", ROOT "/old/stb"));
        world.extra[0] = sensible_reference;
        world.extra_orgs[0] = names[index];
        world.extra_count = 1;
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
        CHECK(find_report(&plan, "org-unknown", ROOT "/old/stb") != NULL);
        CHECK(find_step(&plan, NAULA_MOVE_CHECKOUT, ROOT "/old/stb") == NULL);
        naula_layout_plan_release(&plan);
        forebay_suborg.name = "forebay";
    }
}

static void a_held_org_named_like_a_top_link_takes_the_folder(void) {
    scenario world;
    start_world(&world);
    world.orgs[2].name = "docs";
    world.repos[0].current_path = ROOT "/repos/aaaaaaaaaaaa";
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/docs") == NULL);
    CHECK(find_report(&plan, "name-collides", ROOT "/repos/aaaaaaaaaaaa") != NULL);
    CHECK(writes(&plan, ROOT "/CLAUDE.md", "@repos/aaaaaaaaaaaa/RULES.md\n"));
    naula_layout_plan_release(&plan);
}

static void a_sixty_four_hex_id_collides_on_twelve_hex(void) {
    scenario world;
    start_world(&world);
    add_repo(&world, repo_at(hex_of('1'), "wide", "spisor", NULL));
    char long_hex[65];
    memset(long_hex, '1', 64);
    long_hex[64] = '\0';
    add_repo(&world, repo_at(long_hex, "long", "intisy", NULL));
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "twelve-hex-collision", ROOT "/spisor/repos/111111111111") != NULL);
    CHECK(find_report(&plan, "twelve-hex-collision", ROOT "/repos/111111111111") != NULL);
    naula_layout_plan_release(&plan);
}

static void an_extra_may_name_the_top_org(void) {
    scenario world;
    start_world(&world);
    world.extra[0] = sensible_reference;
    world.extra_orgs[0] = "INTISY";
    world.extra_count = 1;
    add_repo(&world, repo_at(hex_of('5'), "fork", "evil", NULL));
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(links_to(&plan, ROOT "/fork", ROOT "/repos/555555555555"));
    naula_layout_plan_release(&plan);
}

static naula_home stranger_accounts[] = {
    {sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "stranger", NULL, NULL},
    {sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "spisor", NULL, NULL}};

static void a_strangers_org_file_captures_nothing(void) {
    scenario world;
    start_world(&world);
    world.orgs[world.org_count] = declaration("stranger", &intisy_docs_reference, stranger_accounts, NULL);
    world.orgs[world.org_count].account_count = 2;
    world.org_repositories[world.org_count++] = git_id(hex_of('8'));
    add_repo(&world, repo_at(hex_of('8'), "docs", "stranger", NULL));
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "account-not-owned", ROOT "/stranger") != NULL);
    CHECK(links_to(&plan, ROOT "/spisor/docs", ROOT "/spisor/repos/bbbbbbbbbbbb"));
    CHECK(links_to(&plan, ROOT "/stranger/docs", ROOT "/stranger/repos/888888888888"));
    for (size_t index = 0; index < plan.step_count; index++) {
        CHECK(!(plan.steps[index].kind == NAULA_MAKE_LINK && strstr(plan.steps[index].path, "/stranger/") != NULL &&
                strstr(plan.steps[index].path, "/stranger/docs") == NULL));
    }
    naula_layout_plan_release(&plan);
}

static naula_home lab_accounts[] = {{sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "lab", NULL, NULL}};

static void a_self_declared_suborg_the_parent_does_not_list_lands_at_the_root(void) {
    scenario world;
    start_world(&world);
    world.orgs[world.org_count] = declaration("lab", &spisor_docs_reference, lab_accounts, "RULES.md");
    world.org_repositories[world.org_count++] = git_id(hex_of('8'));
    add_repo(&world, repo_at(hex_of('8'), "docs", "lab", NULL));
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "parent-not-listing", ROOT "/lab") != NULL);
    CHECK(links_to(&plan, ROOT "/lab/docs", ROOT "/lab/repos/888888888888"));
    CHECK(find_step(&plan, NAULA_MAKE_FOLDER, ROOT "/spisor/lab") == NULL);
    naula_layout_plan_release(&plan);
}

static void a_self_declared_suborg_nests_when_its_repo_is_in_a_parent_account(void) {
    scenario world;
    start_world(&world);
    world.orgs[world.org_count] = declaration("lab", &spisor_docs_reference, lab_accounts, NULL);
    world.org_repositories[world.org_count++] = git_id(hex_of('8'));
    add_repo(&world, repo_at(hex_of('8'), "lab-docs", "spisor", NULL));
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_MAKE_FOLDER, ROOT "/spisor/lab") != NULL);
    CHECK(find_report(&plan, "parent-not-listing", NULL) == NULL);
    CHECK(find_report(&plan, "account-not-owned", ROOT "/spisor/lab") != NULL);
    naula_layout_plan_release(&plan);
}

static void refuses_input_smaller_than_its_first_shape(void) {
    scenario world;
    start_world(&world);
    naula_layout_input input = input_of(&world);
    input.size = (uint32_t)offsetof(naula_layout_input, existing_link_count);
    naula_layout_plan plan;
    memset(&plan, 0, sizeof plan);
    plan.size = sizeof plan;
    naula_refusal refusal;
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    CHECK(naula_layout_plan_make(&input, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "layout-input-invalid", "short input"));
    input = input_of(&world);
    input.root = NULL;
    CHECK(naula_layout_plan_make(&input, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "layout-input-invalid", "no root"));
    input = input_of(&world);
    input.top = NULL;
    CHECK(naula_layout_plan_make(&input, &plan, &refusal) == NAULA_REFUSED);
    naula_reference short_reference = spisor_docs_reference;
    short_reference.size = (uint32_t)offsetof(naula_reference, at_count);
    world.repos[4].member_of = &short_reference;
    input = input_of(&world);
    CHECK(naula_layout_plan_make(&input, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "layout-input-invalid", "short memberOf"));
    start_world(&world);
    add_link(&world, ROOT "/spisor/old", ROOT "/spisor/repos/eeeeeeeeeeee");
    world.links[0].size = (uint32_t)offsetof(naula_layout_link, made_by_naula);
    input = input_of(&world);
    CHECK(naula_layout_plan_make(&input, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "layout-input-invalid", "short link"));
}

static void refuses_an_id_that_could_name_a_path(void) {
    const char *commits[] = {"../../../../../../../../../../etc/passwd", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                             "aaaa", ""};
    for (size_t index = 0; index < sizeof commits / sizeof commits[0]; index++) {
        scenario world;
        start_world(&world);
        naula_layout_repo *hostile = add_repo(&world, repo_at(hex_of('1'), "hostile", "spisor", NULL));
        snprintf(hostile->id.commit, sizeof hostile->id.commit, "%s", commits[index]);
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
        CHECK(check_text(refusal.code, "layout-input-invalid", commits[index]));
    }
    scenario world;
    start_world(&world);
    world.repos[3].id.kind = 0;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "layout-input-invalid", "kind 0"));
}

static void removes_a_link_left_in_a_renamed_project_folder(void) {
    scenario world;
    start_world(&world);
    world.repos[4].member_of = &spisor_docs_reference;
    add_link(&world, ROOT "/spisor/old/library", ROOT "/spisor/repos/eeeeeeeeeeee");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/old/library") != NULL);
    naula_layout_plan_release(&plan);
}

static void removes_the_project_links_of_a_released_org(void) {
    scenario world;
    start_world(&world);
    world.orgs[0] = world.orgs[1];
    world.org_repositories[0] = world.org_repositories[1];
    world.orgs[1] = world.orgs[2];
    world.org_repositories[1] = world.org_repositories[2];
    world.org_count = 2;
    add_link(&world, ROOT "/spisor/docs", ROOT "/spisor/repos/bbbbbbbbbbbb");
    add_link(&world, ROOT "/spisor/shared/library", ROOT "/spisor/repos/eeeeeeeeeeee");
    add_link(&world, ROOT "/spisor/forebay/host", ROOT "/spisor/forebay/repos/ffffffffffff");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/docs") != NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/shared/library") != NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/forebay/host") != NULL);
    naula_layout_plan_release(&plan);
}

static void a_sub_org_never_nests_through_a_claim_set_aside(void) {
    for (int contested = 0; contested < 2; contested++) {
        scenario world;
        start_world(&world);
        world.repos[4].member_of = &spisor_docs_reference;
        if (contested) {
            world.orgs[1].accounts = weivel_and_spisor_accounts;
            world.orgs[1].account_count = 2;
            world.repos[3].member_of = &weivel_records_reference;
        }
        world.orgs[world.org_count] = declaration("evilsub", &weivel_records_reference, only_stranger_account,
                                                  "RULES.md");
        world.org_repositories[world.org_count++] = git_id(hex_of('3'));
        add_repo(&world, repo_at(hex_of('3'), "evil-docs", "spisor", NULL));
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
        CHECK(find_step(&plan, NAULA_MAKE_FOLDER, ROOT "/weivel/evilsub") == NULL);
        CHECK(find_report(&plan, "parent-not-listing", ROOT "/evilsub") != NULL);
        naula_layout_plan_release(&plan);
    }
}

static naula_home lab_entry_accounts[] = {{sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "lab", NULL, NULL}};
static naula_org_declaration lab_entry;

static void a_stranger_named_like_the_tops_entry_is_left_out(void) {
    scenario world;
    start_world(&world);
    lab_entry = declaration("lab", NULL, lab_entry_accounts, NULL);
    world.top.suborgs = &lab_entry;
    world.top.suborg_count = 1;
    world.orgs[world.org_count] = declaration("lab", &intisy_docs_reference, only_stranger_account, "RULES.md");
    world.org_repositories[world.org_count++] = git_id(hex_of('3'));
    add_repo(&world, repo_at(hex_of('3'), "records", "stranger", NULL));
    add_repo(&world, repo_at(hex_of('4'), "bench", "lab", NULL))->member_of = &intisy_docs_reference;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "org-name-taken", ROOT "/lab") != NULL);
    CHECK(links_to(&plan, ROOT "/lab/bench", ROOT "/lab/repos/444444444444"));
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/lab/records") == NULL);
    CHECK(find_step(&plan, NAULA_WRITE_FILE, ROOT "/lab/CLAUDE.md") == NULL);
    naula_layout_plan_release(&plan);
}

static void two_orgs_of_equal_standing_with_one_folder_refuse_the_plan(void) {
    scenario world;
    start_world(&world);
    world.orgs[1].name = "pinaxis";
    world.orgs[1].parent = NULL;
    world.orgs[2].parent = NULL;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "name-collides", "two orgs at the root by default"));
}

static void a_report_always_names_its_subject(void) {
    scenario world;
    start_world(&world);
    add_repo(&world, repo_at(hex_of('2'), "claimer", "stranger", NULL))->member_of = &intisy_docs_reference;
    world.extra_orgs[0] = "nobody";
    world.extra[0] = (naula_reference){sizeof(naula_reference), "git:3333333333333333333333333333333333333333", NULL,
                                       NULL, 0};
    world.extra_count = 1;
    add_repo(&world, repo_at(hex_of('3'), "kept", "elsewhere", NULL));
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "member-of-unlisted", NULL) != NULL);
    CHECK(find_report(&plan, "org-unknown", NULL) != NULL);
    for (size_t index = 0; index < plan.step_count; index++) {
        CHECK(plan.steps[index].kind != NAULA_REPORT || plan.steps[index].path != NULL);
    }
    naula_layout_plan_release(&plan);
}

static void two_different_declarations_from_one_repo_are_refused(void) {
    scenario world;
    start_world(&world);
    world.orgs[world.org_count] = world.orgs[1];
    world.org_repositories[world.org_count++] = world.org_repositories[1];
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(links_to(&plan, ROOT "/weivel/records", ROOT "/weivel/repos/cccccccccccc"));
    naula_layout_plan_release(&plan);

    world.orgs[world.org_count - 1].rules = "OTHER.md";
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "layout-input-invalid", "one repo, two declarations"));

    start_world(&world);
    world.orgs[world.org_count] = declaration("again", NULL, intisy_accounts, NULL);
    world.org_repositories[world.org_count++] = world.top_repository;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
    CHECK(check_text(refusal.code, "layout-input-invalid", "a declaration beside the top's"));
}

static void an_unrecorded_link_at_an_org_or_repos_folder_refuses_the_plan(void) {
    const char *links[] = {ROOT "/spisor", ROOT "/spisor/repos", ROOT "/Spisor/Forebay", ROOT "/repos"};
    for (size_t index = 0; index < sizeof links / sizeof links[0]; index++) {
        scenario world;
        start_world(&world);
        world.repos[4].member_of = &spisor_docs_reference;
        world.repos[3].current_path = "T:/old/library";
        add_foreign_link(&world, links[index], "D:/foreign");
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_world(&world, &plan, &refusal) == NAULA_REFUSED);
        CHECK(check_text(refusal.code, "path-occupied", links[index]));
        CHECK(strstr(refusal.detail, links[index]) != NULL);
        CHECK(plan.step_count == 0 && plan.steps == NULL);
    }
}

static void nothing_is_made_through_an_unrecorded_link_at_a_project_folder(void) {
    scenario world;
    start_world(&world);
    world.repos[4].member_of = &spisor_docs_reference;
    add_foreign_link(&world, ROOT "/spisor/shared", "D:/foreign");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/spisor/shared/library") == NULL);
    CHECK(find_step(&plan, NAULA_MAKE_FOLDER, ROOT "/spisor/shared") == NULL);
    CHECK(find_report(&plan, "path-occupied", ROOT "/spisor/shared/library") != NULL);
    CHECK(links_to(&plan, ROOT "/spisor/docs", ROOT "/spisor/repos/bbbbbbbbbbbb"));
    naula_layout_plan_release(&plan);
}

static void rules_never_import_through_an_unrecorded_link_above_their_link(void) {
    scenario world;
    start_world(&world);
    world.repos[4].member_of = &spisor_docs_reference;
    set_topics(&world.repos[0], naula_topics, 1);
    world.repos[0].current_path = ROOT "/repos/aaaaaaaaaaaa";
    add_foreign_link(&world, ROOT "/tools", "D:/foreign");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(writes(&plan, ROOT "/CLAUDE.md", "@repos/aaaaaaaaaaaa/RULES.md\n"));
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/tools/docs") == NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/tools/naula") == NULL);
    CHECK(find_step(&plan, NAULA_MAKE_FOLDER, ROOT "/tools") == NULL);
    CHECK(find_report(&plan, "path-occupied", ROOT "/tools/docs") != NULL);
    naula_layout_plan_release(&plan);

    world.repos[0].current_path = NULL;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_WRITE_FILE, ROOT "/CLAUDE.md") == NULL);
    CHECK(find_report(&plan, "rules-repo-missing", ROOT) != NULL);
    naula_layout_plan_release(&plan);
}

static void a_checkout_folder_reached_through_an_unrecorded_link_is_left_alone(void) {
    scenario world;
    start_world(&world);
    world.repos[1].current_path = ROOT "/old/naula";
    add_foreign_link(&world, ROOT "/repos/aaaaaaaaaaaa", "D:/foreign");
    add_foreign_link(&world, ROOT "/repos/dddddddddddd", "D:/foreign");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/docs") == NULL);
    CHECK(find_report(&plan, "path-occupied", ROOT "/repos/aaaaaaaaaaaa") != NULL);
    CHECK(find_step(&plan, NAULA_WRITE_FILE, ROOT "/CLAUDE.md") == NULL);
    CHECK(find_step(&plan, NAULA_MOVE_CHECKOUT, ROOT "/old/naula") == NULL);
    CHECK(find_step(&plan, NAULA_MAKE_LINK, ROOT "/tools/naula") == NULL);
    CHECK(find_report(&plan, "path-occupied", ROOT "/old/naula") != NULL);
    naula_layout_plan_release(&plan);
}

static void no_rules_file_is_written_through_an_unrecorded_link(void) {
    scenario world;
    start_world(&world);
    add_foreign_link(&world, ROOT "/spisor/CLAUDE.md", "D:/foreign/CLAUDE.md");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_WRITE_FILE, ROOT "/spisor/CLAUDE.md") == NULL);
    CHECK(find_report(&plan, "path-occupied", ROOT "/spisor/CLAUDE.md") != NULL);
    CHECK(writes(&plan, ROOT "/weivel/CLAUDE.md", "@records/RULES.md\n"));
    naula_layout_plan_release(&plan);
}

static void no_link_is_removed_through_an_unrecorded_link(void) {
    scenario world;
    start_world(&world);
    add_link(&world, ROOT "/spisor/old/library", ROOT "/spisor/repos/eeeeeeeeeeee");
    add_foreign_link(&world, ROOT "/spisor/old", "D:/foreign");
    naula_layout_repo *old = add_repo(&world, repo_at(hex_of('1'), "backend", "spisor", ROOT "/spisor/repos/111111111111"));
    old->archived = 1;
    old->clean_and_pushed = 1;
    add_link(&world, ROOT "/spisor/archive/backend", ROOT "/spisor/repos/111111111111");
    add_foreign_link(&world, ROOT "/spisor/archive", "D:/foreign");
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/old/library") == NULL);
    CHECK(find_report(&plan, "path-occupied", ROOT "/spisor/old/library") != NULL);
    CHECK(find_step(&plan, NAULA_REMOVE_LINK, ROOT "/spisor/archive/backend") == NULL);
    CHECK(find_report(&plan, "path-occupied", ROOT "/spisor/archive/backend") != NULL);
    naula_layout_plan_release(&plan);
}

static void a_held_org_named_like_a_top_project_or_kept_folder_is_left_out(void) {
    const char *names[] = {"tools", "Tools", "archive"};
    for (size_t index = 0; index < sizeof names / sizeof names[0]; index++) {
        scenario world;
        start_world(&world);
        world.repos[4].member_of = &spisor_docs_reference;
        world.orgs[world.org_count] =
            declaration(names[index], &intisy_docs_reference, only_stranger_account, "RULES.md");
        world.org_repositories[world.org_count++] = git_id(hex_of('3'));
        add_repo(&world, repo_at(hex_of('3'), "records", "stranger", NULL));
        naula_layout_plan plan;
        naula_refusal refusal;
        CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
        char folder[64];
        snprintf(folder, sizeof folder, "%s/%s", ROOT, names[index]);
        CHECK(find_report(&plan, "org-name-taken", folder) != NULL);
        CHECK(find_step(&plan, NAULA_MAKE_FOLDER, ROOT "/tools/repos") == NULL);
        CHECK(find_step(&plan, NAULA_MAKE_FOLDER, ROOT "/archive/repos") == NULL);
        CHECK(find_step(&plan, NAULA_WRITE_FILE, ROOT "/tools/CLAUDE.md") == NULL);
        CHECK(links_to(&plan, ROOT "/tools/naula", ROOT "/repos/dddddddddddd"));
        naula_layout_plan_release(&plan);
    }
}

static naula_home deep_entry_accounts[] = {{sizeof(naula_home), NAULA_HOME_PROVIDER, "github", NULL, "deep", NULL, NULL}};
static naula_org_declaration deep_entry;
static naula_reference left_out_reference = {sizeof(naula_reference), "git:3333333333333333333333333333333333333333",
                                             NULL, NULL, 0};

static void a_member_of_naming_a_left_out_org_says_it_was_left_out(void) {
    scenario world;
    start_world(&world);
    world.repos[4].member_of = &spisor_docs_reference;
    lab_entry = declaration("lab", NULL, lab_entry_accounts, NULL);
    deep_entry = declaration("deeper", NULL, deep_entry_accounts, NULL);
    world.top.suborgs = &lab_entry;
    world.top.suborg_count = 1;
    world.orgs[world.org_count] = declaration("lab", &intisy_docs_reference, only_stranger_account, "RULES.md");
    world.orgs[world.org_count].suborgs = &deep_entry;
    world.orgs[world.org_count].suborg_count = 1;
    world.org_repositories[world.org_count++] = git_id(hex_of('3'));
    add_repo(&world, repo_at(hex_of('3'), "records", "stranger", NULL));
    add_repo(&world, repo_at(hex_of('4'), "x", "deep", NULL))->member_of = &left_out_reference;
    naula_layout_plan plan;
    naula_refusal refusal;
    CHECK(plan_world(&world, &plan, &refusal) == NAULA_OK);
    CHECK(find_report(&plan, "member-of-unlisted", NULL) == NULL);
    const naula_layout_step *report = find_report(&plan, "org-name-taken", "x");
    CHECK(report != NULL && strstr(report->content, "memberOf") != NULL && strstr(report->content, "left out") != NULL);
    naula_layout_plan_release(&plan);
}

static void survives_every_allocation_failure(void) {
    scenario world;
    start_world(&world);
    world.repos[2].current_path = ROOT "/spisor/docs";
    naula_layout_repo *old = add_repo(&world, repo_at(hex_of('1'), "backend", "spisor", ROOT "/old/backend"));
    old->archived = 1;
    add_repo(&world, repo_at(hex_of('2'), "Docs", "spisor", NULL));
    int finished = 0;
    for (uint64_t attempt = 1; attempt < 100000 && !finished; attempt++) {
        naula_memory_fail_at(attempt);
        naula_layout_plan plan;
        naula_refusal refusal;
        naula_status status = plan_world(&world, &plan, &refusal);
        naula_memory_fail_at(0);
        if (status == NAULA_OK) {
            finished = 1;
            naula_layout_plan_release(&plan);
        } else {
            CHECK(status == NAULA_OUT_OF_MEMORY);
            CHECK(plan.steps == NULL && plan.step_count == 0);
        }
    }
    CHECK(finished);
}

int main(void) {
    nests_a_suborg_under_its_parent();
    a_suborg_holds_its_account_once_a_repo_there_confirms_it();
    an_org_holds_an_extra_account_through_a_confirming_repo();
    a_strangers_member_of_confirms_nothing();
    a_member_of_naming_an_org_nobody_holds_is_reported();
    two_confirmation_only_claims_hold_neither();
    a_declaring_repo_wins_over_a_confirmation_only_claim();
    a_suborg_entry_listing_its_parents_account_is_reported();
    a_member_of_by_an_unknown_url_confirms_nothing();
    places_a_held_org_without_its_parent_at_the_root();
    generates_one_import_line_per_org_with_rules();
    forebay_suborg_imports_through_its_parents_docs();
    org_without_rules_gets_no_claude_file();
    a_listed_suborg_declaring_itself_takes_its_members_from_its_own_declaration();
    links_by_the_one_project_topic();
    links_at_the_org_top_with_no_project_topic();
    two_project_topics_get_no_link();
    refuses_a_name_colliding_with_repos();
    refuses_an_org_folder_colliding_with_repos();
    refuses_an_unsafe_org_or_project_name();
    a_hostile_repo_name_is_never_a_path();
    names_differing_only_in_case_collide();
    refuses_an_account_held_by_two_orgs();
    refuses_a_parent_cycle();
    plan_refuses_two_repos_with_one_id();
    plan_refuses_a_twelve_hex_collision_naming_both();
    a_mirror_account_never_decides_the_org();
    plan_keeps_an_archived_clone_with_unpushed_commits();
    plan_removes_a_clean_pushed_archived_clone();
    never_removes_a_clone_holding_another_checkout();
    places_an_unadopted_repo_from_its_host_with_a_note();
    a_first_shape_repo_reads_as_adopted();
    a_dropped_org_is_not_laid_out();
    an_extra_repo_is_kept_in_its_named_org();
    a_homeless_repo_is_kept_local_without_a_link();
    a_homeless_repo_outside_the_root_is_left_alone();
    two_repos_claiming_one_checkout_are_left_alone();
    moves_a_checkout_out_of_its_link_path_before_linking();
    swapped_checkouts_are_reported_not_moved();
    a_checkout_moves_out_of_the_way_before_one_moves_in();
    refuses_a_checkout_holding_an_org_folder();
    plan_is_stable_for_unchanged_input();
    plan_does_not_depend_on_input_order();
    plan_does_not_depend_on_input_order_with_a_copy();
    a_copy_of_a_declaring_repo_elsewhere_takes_no_account();
    a_stranger_named_like_a_listed_suborg_lands_at_the_root();
    refuses_an_unclean_root_or_current_path();
    a_held_repo_outside_the_root_is_left_alone();
    a_filesystem_root_is_refused_as_the_root();
    never_removes_or_moves_a_clone_at_a_naula_made_path();
    a_foreign_checkout_in_a_target_folder_blocks_the_repo();
    a_checkout_nested_in_another_is_left_alone();
    removes_the_link_left_by_a_rename();
    removes_the_link_left_by_a_topic_change();
    removes_the_links_of_a_dropped_org();
    removes_the_links_of_a_renamed_org();
    leaves_links_naula_did_not_make();
    removes_only_links_naula_recorded_making();
    a_foreign_link_where_naula_links_is_reported_and_left();
    rules_never_load_through_a_foreign_link();
    a_checkout_reached_through_an_existing_link_is_left_alone();
    removes_only_links_in_the_places_naula_links();
    a_current_path_differing_only_in_case_is_in_place();
    a_checkout_inside_its_own_target_is_left_alone();
    an_extra_naming_an_unknown_or_ambiguous_org_is_reported();
    a_held_org_named_like_a_top_link_takes_the_folder();
    a_sixty_four_hex_id_collides_on_twelve_hex();
    an_extra_may_name_the_top_org();
    a_strangers_org_file_captures_nothing();
    a_self_declared_suborg_the_parent_does_not_list_lands_at_the_root();
    a_self_declared_suborg_nests_when_its_repo_is_in_a_parent_account();
    refuses_input_smaller_than_its_first_shape();
    refuses_an_id_that_could_name_a_path();
    removes_a_link_left_in_a_renamed_project_folder();
    removes_the_project_links_of_a_released_org();
    a_sub_org_never_nests_through_a_claim_set_aside();
    a_stranger_named_like_the_tops_entry_is_left_out();
    two_orgs_of_equal_standing_with_one_folder_refuse_the_plan();
    a_report_always_names_its_subject();
    two_different_declarations_from_one_repo_are_refused();
    an_unrecorded_link_at_an_org_or_repos_folder_refuses_the_plan();
    nothing_is_made_through_an_unrecorded_link_at_a_project_folder();
    rules_never_import_through_an_unrecorded_link_above_their_link();
    a_checkout_folder_reached_through_an_unrecorded_link_is_left_alone();
    no_rules_file_is_written_through_an_unrecorded_link();
    no_link_is_removed_through_an_unrecorded_link();
    a_held_org_named_like_a_top_project_or_kept_folder_is_left_out();
    a_member_of_naming_a_left_out_org_says_it_was_left_out();
    survives_every_allocation_failure();
    return check_summary("layout_plan_test");
}
