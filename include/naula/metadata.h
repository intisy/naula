#ifndef NAULA_METADATA_H
#define NAULA_METADATA_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bits of naula_metadata's `present`. Enums append only. */
typedef enum naula_metadata_member {
    NAULA_METADATA_DESCRIPTION = 1u << 0,
    NAULA_METADATA_TOPICS = 1u << 1,
    NAULA_METADATA_HOMEPAGE = 1u << 2,
    NAULA_METADATA_SETTINGS = 1u << 3
} naula_metadata_member;

/**
 * What a host shows about a repo. A member whose bit is not in `present` is left alone on the host, so an empty
 * `topics` with its bit set clears them while an absent one does not. `settings` is a cJSON object released with
 * cJSON_Delete. First shape ends at `present`.
 */
typedef struct naula_metadata {
    uint32_t size;
    char *description;
    char **topics;
    size_t topic_count;
    char *homepage;
    cJSON *settings;
    uint32_t present;
} naula_metadata;

void naula_metadata_release(naula_metadata *metadata);

#ifdef __cplusplus
}
#endif

#endif
