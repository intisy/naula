#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "cli.h"
#include "naula/home.h"
#include "naula/memory.h"

static const char *state_name(uint32_t state) {
    return state == NAULA_CONFIRMED ? "confirmed" : state == NAULA_STALE ? "stale" : "domain-only";
}

static void print_text(const char *id, const naula_resolution *resolution) {
    printf("id %s\nstate %s\n", id, state_name(resolution->state));
    for (size_t index = 0; index < resolution->home_count; index++) {
        char *text = naula_place_text(&resolution->homes[index]);
        printf("%s %s\n", index == resolution->primary ? "primary" : "home", text == NULL ? "(home)" : text);
        naula_memory_release(text);
    }
    for (size_t index = 0; index < resolution->verdict_count; index++) {
        const naula_verdict *verdict = &resolution->verdicts[index];
        char *text = naula_place_text(&verdict->home);
        printf("verdict %s %s: %s\n", text == NULL ? "(home)" : text, verdict->code, verdict->detail);
        naula_memory_release(text);
    }
    if (resolution->cache_problem[0] != '\0') {
        printf("cache %s\n", resolution->cache_problem);
    }
}

static int add_place(cJSON *array, const naula_place *place) {
    char *text = naula_place_text(place);
    cJSON *item = text == NULL ? NULL : cJSON_CreateString(text);
    naula_memory_release(text);
    return item != NULL && cJSON_AddItemToArray(array, item);
}

static cJSON *verdicts_json(const naula_resolution *resolution) {
    cJSON *verdicts = cJSON_CreateArray();
    for (size_t index = 0; verdicts != NULL && index < resolution->verdict_count; index++) {
        const naula_verdict *verdict = &resolution->verdicts[index];
        char *text = naula_place_text(&verdict->home);
        cJSON *item = cJSON_CreateObject();
        int made = text != NULL && item != NULL && cJSON_AddStringToObject(item, "home", text) != NULL &&
                   cJSON_AddBoolToObject(item, "confirmed", verdict->confirmed) != NULL &&
                   cJSON_AddStringToObject(item, "code", verdict->code) != NULL &&
                   cJSON_AddStringToObject(item, "detail", verdict->detail) != NULL;
        naula_memory_release(text);
        if (!made || !cJSON_AddItemToArray(verdicts, item)) {
            cJSON_Delete(item);
            cJSON_Delete(verdicts);
            return NULL;
        }
    }
    return verdicts;
}

/** The resolution as one JSON object on standard output; `refusal` set adds what refused it. */
static int print_json(const char *id, const naula_resolution *resolution, const naula_refusal *refusal) {
    cJSON *root = cJSON_CreateObject();
    cJSON *homes = cJSON_CreateArray();
    int made = root != NULL && homes != NULL && cJSON_AddStringToObject(root, "id", id) != NULL;
    if (made && refusal == NULL) {
        made = cJSON_AddStringToObject(root, "state", state_name(resolution->state)) != NULL;
        for (size_t index = 0; made && index < resolution->home_count; index++) {
            made = add_place(homes, &resolution->homes[index]);
        }
        if (made && resolution->home_count > 0) {
            char *primary = naula_place_text(&resolution->homes[resolution->primary]);
            made = primary != NULL && cJSON_AddStringToObject(root, "primary", primary) != NULL;
            naula_memory_release(primary);
        }
    }
    if (made && refusal != NULL) {
        made = cJSON_AddStringToObject(root, "refused", refusal->code) != NULL &&
               cJSON_AddStringToObject(root, "detail", refusal->detail) != NULL &&
               (refusal->fix == NULL || cJSON_AddStringToObject(root, "fix", refusal->fix) != NULL);
    }
    made = made && cJSON_AddItemToObject(root, "homes", homes);
    if (!made) {
        cJSON_Delete(homes);
    }
    cJSON *verdicts = made ? verdicts_json(resolution) : NULL;
    made = made && verdicts != NULL && cJSON_AddItemToObject(root, "verdicts", verdicts);
    char *printed = made ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (printed == NULL) {
        return 0;
    }
    printf("%s\n", printed);
    cJSON_free(printed);
    return 1;
}

int cli_command_resolve(int count, char **values) {
    cli_arguments arguments;
    if (!cli_parse_arguments("resolve", count, values, CLI_ALLOW_AT | CLI_ALLOW_JSON | CLI_ALLOW_OFFLINE | CLI_ALLOW_ACCEPT,
                             1, 1, &arguments)) {
        return CLI_USAGE;
    }
    if (strncmp(arguments.positional[0], "git:", 4) != 0) {
        return cli_usage_error("resolve", "takes a git: id, not %s", arguments.positional[0]);
    }
    cli_context context;
    naula_refusal refusal = cli_fresh_refusal();
    naula_status status = cli_context_open(&arguments, &context, &refusal);
    if (status != NAULA_OK) {
        return cli_refused(status, &refusal);
    }
    naula_reference reference;
    naula_resolution resolution;
    memset(&resolution, 0, sizeof resolution);
    resolution.size = sizeof resolution;
    status = cli_reference_from_arguments(&context, arguments.positional[0], arguments.at, arguments.at_count,
                                          &reference, &refusal);
    if (status == NAULA_OK) {
        status = cli_resolve(&context, &reference, &resolution, &refusal);
        cli_reference_release(&reference);
    }
    int exit_code = CLI_DONE;
    if (status == NAULA_OK) {
        if (arguments.json) {
            exit_code = print_json(arguments.positional[0], &resolution, NULL) ? CLI_DONE : CLI_REFUSED;
        } else {
            print_text(arguments.positional[0], &resolution);
        }
    } else {
        if (arguments.json && status != NAULA_OUT_OF_MEMORY) {
            print_json(arguments.positional[0], &resolution, &refusal);
        }
        cli_print_verdicts(&resolution);
        cli_print_questions(&context);
        exit_code = cli_refused(status, &refusal);
    }
    naula_resolution_release(&resolution);
    cli_context_close(&context);
    return exit_code;
}
