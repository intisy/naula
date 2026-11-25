#include <stdio.h>
#include <string.h>

#include "check.h"
#include "cJSON.h"
#include "naula/home.h"
#include "naula/json.h"

static naula_status parse_home(const char *json_text, naula_home *home, naula_refusal *refusal) {
    cJSON *json = cJSON_Parse(json_text);
    memset(home, 0, sizeof *home);
    home->size = sizeof *home;
    memset(refusal, 0, sizeof *refusal);
    refusal->size = sizeof *refusal;
    naula_status status = naula_home_from_json(json, home, refusal);
    cJSON_Delete(json);
    return status;
}

static naula_status parse_place(const char *text, naula_place *place, naula_refusal *refusal) {
    memset(place, 0, sizeof *place);
    place->size = sizeof *place;
    memset(refusal, 0, sizeof *refusal);
    refusal->size = sizeof *refusal;
    return naula_place_parse(text, place, refusal);
}

static void a_github_account_home_parses(void) {
    naula_home home;
    naula_refusal refusal;
    CHECK(parse_home("{ \"provider\": \"github\", \"account\": \"spisor\" }", &home, &refusal) == NAULA_OK);
    CHECK(home.kind == NAULA_HOME_PROVIDER && home.host == NULL);
    check_text(home.provider, "github", "provider");
    check_text(home.account, "spisor", "account");
    naula_home_release(&home);
}

static void a_forgejo_home_keeps_its_host(void) {
    naula_home home;
    naula_refusal refusal;
    CHECK(parse_home("{ \"provider\": \"forgejo\", \"host\": \"codeberg.org\", \"account\": \"spisor\" }", &home,
                     &refusal) == NAULA_OK);
    check_text(home.host, "codeberg.org", "host");
    naula_home_release(&home);
}

static void a_web_home_parses(void) {
    naula_home home;
    naula_refusal refusal;
    CHECK(parse_home("{ \"provider\": \"web\", \"domain\": \"naula.dev\" }", &home, &refusal) == NAULA_OK);
    CHECK(home.kind == NAULA_HOME_WEB);
    check_text(home.domain, "naula.dev", "domain");
    naula_home_release(&home);
}

static void a_url_home_parses(void) {
    naula_home home;
    naula_refusal refusal;
    CHECK(parse_home("{ \"url\": \"https://git.example.org/a/b.git\" }", &home, &refusal) == NAULA_OK);
    CHECK(home.kind == NAULA_HOME_URL && home.provider == NULL);
    check_text(home.url, "https://git.example.org/a/b.git", "url");
    naula_home_release(&home);
}

static void expect_home_refused(const char *json_text, const char *label) {
    naula_home home;
    naula_refusal refusal;
    CHECK(parse_home(json_text, &home, &refusal) == NAULA_REFUSED);
    if (check_text(refusal.code, "invalid_home", label)) {
        CHECK(refusal.fix != NULL && refusal.detail[0] != '\0');
    }
    CHECK(home.provider == NULL && home.account == NULL && home.domain == NULL && home.url == NULL);
}

static void homes_that_name_too_much_or_too_little_are_refused(void) {
    expect_home_refused("{ \"provider\": \"github\", \"account\": \"a\", \"url\": \"https://x.org/a/b.git\" }",
                        "account with url");
    expect_home_refused("{ \"url\": \"https://x.org/a/b.git\", \"provider\": \"github\" }", "url with provider");
    expect_home_refused("{ \"provider\": \"web\", \"domain\": \"naula.dev/docs\" }", "web domain with a path");
    expect_home_refused("{ \"provider\": \"web\", \"account\": \"a\", \"domain\": \"naula.dev\" }", "web with account");
    expect_home_refused("{ \"url\": \"http://git.example.org/a/b.git\" }", "http url");
    expect_home_refused("\"github:spisor\"", "string home");
    expect_home_refused("{ \"provider\": \"github\" }", "no account");
    expect_home_refused("{ \"account\": \"spisor\" }", "no provider");
    expect_home_refused("{ \"provider\": \"github\", \"account\": \"a/b\" }", "account with a slash");
    expect_home_refused("{ \"provider\": \"github\", \"account\": \"a\", \"number\": \"1\" }", "unknown member");
    expect_home_refused("{ \"provider\": \"github\", \"account\": 5 }", "account not a string");
}

static void a_home_struct_smaller_than_its_first_shape_is_refused(void) {
    naula_home home;
    naula_refusal refusal;
    cJSON *json = cJSON_Parse("{ \"provider\": \"github\", \"account\": \"spisor\" }");
    memset(&home, 0, sizeof home);
    home.size = sizeof(uint32_t);
    memset(&refusal, 0, sizeof refusal);
    CHECK(naula_home_from_json(json, &home, &refusal) == NAULA_REFUSED);
    CHECK(home.account == NULL);
    cJSON_Delete(json);
}

static void a_provider_number_place_parses(void) {
    naula_place place;
    naula_refusal refusal;
    CHECK(parse_place("github:1222336612", &place, &refusal) == NAULA_OK);
    CHECK(place.kind == NAULA_HOME_PROVIDER && place.number == 1222336612u && place.host == NULL);
    check_text(place.provider, "github", "provider");
    naula_place_release(&place);
}

static void a_hosted_place_parses(void) {
    naula_place place;
    naula_refusal refusal;
    CHECK(parse_place("gitea@git.example.com:12", &place, &refusal) == NAULA_OK);
    check_text(place.provider, "gitea", "provider");
    check_text(place.host, "git.example.com", "host");
    CHECK(place.number == 12);
    naula_place_release(&place);
    CHECK(parse_place("gitea@git.example.com:3000:12", &place, &refusal) == NAULA_OK);
    check_text(place.host, "git.example.com:3000", "host with a port");
    CHECK(place.number == 12);
    naula_place_release(&place);
}

static void a_web_place_and_a_url_place_parse(void) {
    naula_place place;
    naula_refusal refusal;
    CHECK(parse_place("web:naula.dev", &place, &refusal) == NAULA_OK);
    CHECK(place.kind == NAULA_HOME_WEB);
    check_text(place.domain, "naula.dev", "domain");
    naula_place_release(&place);
    CHECK(parse_place("https://git.example.org/a/b.git", &place, &refusal) == NAULA_OK);
    CHECK(place.kind == NAULA_HOME_URL);
    check_text(place.url, "https://git.example.org/a/b.git", "url");
    naula_place_release(&place);
}

static void expect_place_refused(const char *text) {
    naula_place place;
    naula_refusal refusal;
    CHECK(parse_place(text, &place, &refusal) == NAULA_REFUSED);
    check_text(refusal.code, "invalid_place", text);
    CHECK(refusal.fix != NULL);
    CHECK(place.provider == NULL && place.domain == NULL && place.url == NULL);
}

static void a_place_without_a_readable_number_is_refused(void) {
    expect_place_refused("github:");
    expect_place_refused("github:abc");
    expect_place_refused("github:0");
    expect_place_refused("github:12x");
    expect_place_refused("github:99999999999999999999");
    expect_place_refused("github");
    expect_place_refused("GitHub:12");
    expect_place_refused("web:naula.dev/docs");
    expect_place_refused("web@host:naula.dev");
    expect_place_refused("http://git.example.org/a/b.git");
}

int main(void) {
    naula_json_use_allocator();
    a_github_account_home_parses();
    a_forgejo_home_keeps_its_host();
    a_web_home_parses();
    a_url_home_parses();
    homes_that_name_too_much_or_too_little_are_refused();
    a_home_struct_smaller_than_its_first_shape_is_refused();
    a_provider_number_place_parses();
    a_hosted_place_parses();
    a_web_place_and_a_url_place_parse();
    a_place_without_a_readable_number_is_refused();
    return check_summary("home_test");
}
