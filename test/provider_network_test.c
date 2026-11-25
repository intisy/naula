#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "naula/memory.h"
#include "naula/provider.h"

static naula_refusal fresh_refusal(void) {
    naula_refusal refusal;
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    return refusal;
}

static void web_a_domain_that_does_not_resolve_is_unreachable(void) {
    naula_refusal refusal = fresh_refusal();
    uint8_t *bytes = NULL;
    size_t length = 0;
    CHECK(naula_web_document("naula-test.invalid", "dd92836cca7c292258635112ecfe1acb7b4f6ecd", &bytes, &length,
                             &refusal) == NAULA_UNREACHABLE);
    if (!check_text(refusal.code, "host_not_resolved", "unresolved domain")) {
        fprintf(stderr, "refusal detail: %s\n", refusal.detail);
    }
}

static void codeberg_answers_a_find_but_asks_for_a_token_by_number(void) {
    const naula_provider *forgejo = naula_provider_forgejo();
    naula_refusal refusal = fresh_refusal();
    uint64_t number = 0;
    CHECK(forgejo->find(forgejo->context, "codeberg.org", "Codeberg", "pages-server", NULL, &number, &refusal) ==
          NAULA_OK);
    CHECK(number == 16490);
    naula_repository repository;
    memset(&repository, 0, sizeof repository);
    repository.size = sizeof repository;
    CHECK(forgejo->lookup(forgejo->context, "codeberg.org", number, NULL, &repository, &refusal) == NAULA_REFUSED);
    if (!check_text(refusal.code, "token_needed", "anonymous lookup by number")) {
        fprintf(stderr, "refusal detail: %s\n", refusal.detail);
    }
}

int main(void) {
    web_a_domain_that_does_not_resolve_is_unreachable();
    codeberg_answers_a_find_but_asks_for_a_token_by_number();
    return check_summary("provider_network_test");
}
