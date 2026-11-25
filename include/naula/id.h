#ifndef NAULA_ID_H
#define NAULA_ID_H

#include <stddef.h>
#include <stdint.h>

#include "naula/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum naula_id_kind { NAULA_ID_GIT = 1, NAULA_ID_LOCAL = 2 } naula_id_kind;

/**
 * A repo id: `git:<40 or 64 lowercase hex>` or `local:<name>`, then any number of `/<segment>`. `commit` holds the
 * hex of a git id and `local_name` the name of a local one, NULL otherwise. Release it with naula_id_release.
 * First shape ends at `segment_count`.
 */
typedef struct naula_id {
    uint32_t size;
    uint32_t kind;
    char commit[65];
    char *local_name;
    char **segments;
    size_t segment_count;
} naula_id;

/**
 * Reads `text` as an id. A refusal says `offset <n>: ...` in its `detail`, the byte at which the text stops
 * matching, and `id` is left empty.
 */
naula_status naula_id_parse(const char *text, size_t length, naula_id *id, naula_refusal *refusal);

void naula_id_release(naula_id *id);

/** -1, 0 or 1: a local id before a git id, then by name or hex text, then segment by segment, a prefix first. */
int naula_id_compare(const naula_id *left, const naula_id *right);

/** The first twelve hex digits of a git id, the name of its checkout folder; empty for a local id. */
void naula_id_folder(const naula_id *id, char folder[13]);

#ifdef __cplusplus
}
#endif

#endif
