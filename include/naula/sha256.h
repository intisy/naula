#ifndef NAULA_SHA256_H
#define NAULA_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAULA_SHA256_SIZE 32
#define NAULA_SHA256_HEX_SIZE 65

typedef struct naula_sha256 {
    uint32_t state[8];
    uint64_t length;
    uint8_t block[64];
    size_t filled;
} naula_sha256;

void naula_sha256_start(naula_sha256 *hash);

void naula_sha256_add(naula_sha256 *hash, const uint8_t *bytes, size_t length);

/** Ends the hash and writes its 32 byte digest. The hash is spent: start it again before reuse. */
void naula_sha256_finish(naula_sha256 *hash, uint8_t out_digest[NAULA_SHA256_SIZE]);

/** naula_sha256_finish written as 64 lowercase hex digits and a NUL. */
void naula_sha256_hex(naula_sha256 *hash, char out_hex[NAULA_SHA256_HEX_SIZE]);

#ifdef __cplusplus
}
#endif

#endif
