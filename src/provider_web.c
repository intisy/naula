#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "forge_common.h"
#include "naula/id.h"
#include "naula/memory.h"
#include "naula/provider.h"
#include "provider_web_hooks.h"

#define DOCUMENT_ROUTE "%s/.well-known/repo-identity/%s.json"
#define URL_SIZE 2048

static const char *const request_headers[] = {"Accept: application/json", NULL};

static const forge_flavor flavor = {"The domain", "check the domain the home names", request_headers};

static char root_override[FORGE_ROOT_SIZE];

#ifdef NAULA_TEST_HOOKS
void naula_web_use_root(const char *root) {
    if (root == NULL) {
        root_override[0] = '\0';
    } else {
        snprintf(root_override, sizeof root_override, "%s", root);
    }
}
#endif

static int is_hex_identity(const char *hex) {
    size_t length = strlen(hex);
    if (length != 40 && length != 64) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        if (!((hex[index] >= '0' && hex[index] <= '9') || (hex[index] >= 'a' && hex[index] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static naula_status build_url(const char *domain, const char *hex, char *url, naula_refusal *refusal) {
    char root[FORGE_ROOT_SIZE];
    if (root_override[0] != '\0') {
        snprintf(root, sizeof root, "%s", root_override);
    } else if (snprintf(root, sizeof root, "https://%s", domain) >= (int)sizeof root) {
        return forge_refuse_input(&flavor, refusal, "the domain");
    }
    return snprintf(url, URL_SIZE, DOCUMENT_ROUTE, root, hex) < URL_SIZE
               ? NAULA_OK
               : forge_refuse_input(&flavor, refusal, "the request address (too long)");
}

/** A document is read for one repo and used for that repo only, so one that names another id is not this repo's. */
static naula_status check_document(const forge_reply *reply, const char *hex, naula_refusal *refusal) {
    naula_status status = NAULA_OK;
    cJSON *json = forge_parse_reply(&flavor, reply, &status, refusal);
    if (json == NULL) {
        return status;
    }
    const char *text = cJSON_IsObject(json) ? forge_text_member(json, "id") : NULL;
    naula_id id;
    memset(&id, 0, sizeof id);
    id.size = sizeof id;
    if (text == NULL || !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(json, "homes")) ||
        naula_id_parse(text, strlen(text), &id, NULL) != NAULA_OK || id.kind != NAULA_ID_GIT) {
        status = forge_refuse_malformed(&flavor, refusal, "a document with no readable id and homes");
    } else if (strcmp(id.commit, hex) != 0) {
        naula_refuse(refusal, "document_for_another_id", "check that the domain serves the repo the home names",
                     "the document at this domain names %s, not git:%s", text, hex);
        status = NAULA_REFUSED;
    }
    naula_id_release(&id);
    cJSON_Delete(json);
    return status;
}

naula_status naula_web_document(const char *domain, const char *hex, uint8_t **bytes, size_t *length,
                                naula_refusal *refusal) {
    char url[URL_SIZE];
    forge_reply reply;
    *bytes = NULL;
    *length = 0;
    if (domain == NULL || !forge_is_host_text(domain) || hex == NULL || !is_hex_identity(hex)) {
        return forge_refuse_input(&flavor, refusal, "the domain or the identity");
    }
    naula_status status = build_url(domain, hex, url, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    status = forge_fetch(&flavor, url, NULL, FORGE_SMALL_BODY_CAPACITY, &reply, refusal);
    if (status == NAULA_OK) {
        status = check_document(&reply, hex, refusal);
    }
    if (status == NAULA_OK) {
        *bytes = reply.body;
        *length = reply.length;
        reply.body = NULL;
    }
    forge_reply_release(&reply);
    return status;
}

static naula_status unsupported(const char *operation, naula_refusal *refusal) {
    return forge_refuse_unsupported(&flavor, refusal, operation, "read the identity document with naula_web_document");
}

static naula_status web_describe(void *context, uint32_t *supported, int *anonymous_lookup) {
    (void)context;
    *supported = 0;
    *anonymous_lookup = 0;
    return NAULA_OK;
}

static naula_status web_lookup(void *context, const char *host, uint64_t number, const char *token,
                               naula_repository *out, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)number;
    (void)token;
    (void)out;
    return unsupported("lookup", refusal);
}

static naula_status web_find(void *context, const char *host, const char *account, const char *name,
                             const char *token, uint64_t *number, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)account;
    (void)name;
    (void)token;
    *number = 0;
    return unsupported("find", refusal);
}

static naula_status web_read_file(void *context, const char *host, uint64_t number, const char *ref,
                                  const char *path, const char *token, uint8_t **bytes, size_t *length,
                                  naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)number;
    (void)ref;
    (void)path;
    (void)token;
    *bytes = NULL;
    *length = 0;
    return unsupported("read_file", refusal);
}

static naula_status web_contains(void *context, const char *host, uint64_t number, const char *ref,
                                 const char *commit, const char *token, int *contained, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)number;
    (void)ref;
    (void)commit;
    (void)token;
    *contained = 0;
    return unsupported("contains", refusal);
}

static naula_status web_parent_count(void *context, const char *host, uint64_t number, const char *commit,
                                     const char *token, size_t *count, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)number;
    (void)commit;
    (void)token;
    *count = 0;
    return unsupported("parent_count", refusal);
}

static naula_status web_list_repos(void *context, const char *host, const char *account, const char *token,
                                   naula_repository **out, size_t *count, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)account;
    (void)token;
    *out = NULL;
    *count = 0;
    return unsupported("list_repos", refusal);
}

static naula_status web_release_index(void *context, const char *host, uint64_t number, const char *tag,
                                      const char *token, uint8_t **bytes, size_t *length, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)number;
    (void)tag;
    (void)token;
    *bytes = NULL;
    *length = 0;
    return unsupported("release_index", refusal);
}

static const naula_provider web_provider = {sizeof(naula_provider), "web",           NULL,           web_describe,
                                            web_lookup,             web_find,        web_read_file,  web_contains,
                                            web_parent_count,       web_list_repos,  web_release_index};

const naula_provider *naula_provider_web(void) {
    return &web_provider;
}
