/* SHA-256 (FIPS 180-4), for checking BagIt manifests. */
#ifndef ARV_SHA256_H
#define ARV_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t length;        /* bytes hashed so far */
    unsigned char block[64];
    size_t used;            /* bytes waiting in block */
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, unsigned char digest[32]);
/* digest as 64 lowercase hex digits and a NUL */
void sha256_hex(const unsigned char digest[32], char out[65]);

#endif
