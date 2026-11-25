#include <string.h>

#include "naula/memory.h"
#include "naula/metadata.h"

void naula_metadata_release(naula_metadata *metadata) {
    uint32_t size = metadata->size;
    naula_memory_release(metadata->description);
    for (size_t index = 0; index < metadata->topic_count; index++) {
        naula_memory_release(metadata->topics[index]);
    }
    naula_memory_release(metadata->topics);
    naula_memory_release(metadata->homepage);
    cJSON_Delete(metadata->settings);
    memset(metadata, 0, sizeof *metadata);
    metadata->size = size;
}
