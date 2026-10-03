/* SHA-512 (FIPS 180-4), for BagIt's manifest-sha512.txt. */
#ifndef ARV_SHA512_H
#define ARV_SHA512_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t h[8];
    uint64_t length;        /* bytes hashed so far (2^64 bytes is enough) */
    unsigned char block[128];
    size_t used;
} sha512_ctx;

void sha512_init(sha512_ctx *c);
void sha512_update(sha512_ctx *c, const void *data, size_t len);
void sha512_final(sha512_ctx *c, unsigned char digest[64]);
void sha512_hex(const unsigned char digest[64], char out[129]);

#endif
