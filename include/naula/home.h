#ifndef NAULA_HOME_H
#define NAULA_HOME_H

/**
 * Where a repo lives, as repo.json writes it (a home) and as a consumer's `at` writes it (a place).
 * Both structs are read only up to their `size`; the caller sets it and releases them with their release function.
 */

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "naula/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum naula_home_kind { NAULA_HOME_PROVIDER = 1, NAULA_HOME_WEB = 2, NAULA_HOME_URL = 3 } naula_home_kind;

/** A home by account (`provider`, `host` or NULL, `account`), by web `domain`, or by git `url`. First shape ends at `url`. */
typedef struct naula_home {
    uint32_t size;
    uint32_t kind;
    char *provider;
    char *host;
    char *account;
    char *domain;
    char *url;
} naula_home;

/** A provider number (`provider`, `host` or NULL, `number`), a web `domain`, or a git `url`. First shape ends at `url`. */
typedef struct naula_place {
    uint32_t size;
    uint32_t kind;
    char *provider;
    char *host;
    uint64_t number;
    char *domain;
    char *url;
} naula_place;

/** Reads one member of `homes`. Refuses a string, a member it does not know, an account beside a url, a domain with a path and a url that is not https or ssh. */
naula_status naula_home_from_json(const cJSON *json, naula_home *home, naula_refusal *refusal);

/** Reads `github:1222336612`, `gitea@git.example.com:12`, `web:naula.dev` or a git url. */
naula_status naula_place_parse(const char *text, naula_place *place, naula_refusal *refusal);

/**
 * A place in the normal text form an `at` is kept in: `github:7`, `gitea@git.example.com:12`, `web:naula.dev` or the
 * url as written, the host and domain lowercased and GitHub's own host left out, since a GitHub place may write it or
 * not. Release it with naula_memory_release; NULL only when memory runs out.
 */
char *naula_place_text(const naula_place *place);

void naula_home_release(naula_home *home);
void naula_place_release(naula_place *place);

#ifdef __cplusplus
}
#endif

#endif
