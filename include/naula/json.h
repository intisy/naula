#ifndef NAULA_JSON_H
#define NAULA_JSON_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Points cJSON's allocation at naula_memory_allocate and naula_memory_release, so a test's counting allocator sees
 * every block a JSON parse makes. Call it first in every binary that links naula, before any thread starts.
 *
 * @implNote cJSON_InitHooks is process-global, so the library never installs hooks itself: a binary that also links
 * another user of cJSON decides once which allocator they share.
 */
void naula_json_use_allocator(void);

#ifdef __cplusplus
}
#endif

#endif
