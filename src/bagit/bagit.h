/*
 * bagit: BagIt bags (RFC 8493) checked, and the digests they use, in C99 and POSIX with no
 * libraries. Used by arv (src/arvc) for its own discs, and by the bagit program for any bag.
 */
#ifndef BAGIT_H
#define BAGIT_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ digests (FIPS 180-4, RFC 1321) */

typedef struct {
    uint32_t h[8];
    uint64_t length;        /* bytes hashed so far */
    unsigned char block[64];
    size_t used;            /* bytes waiting in block */
} sha256_ctx;

typedef struct {
    uint64_t h[8];
    uint64_t length;
    unsigned char block[128];
    size_t used;
} sha512_ctx;

typedef struct {
    uint32_t h[5];
    uint64_t length;
    unsigned char block[64];
    size_t used;
} sha1_ctx;

typedef struct {
    uint32_t h[4];
    uint64_t length;
    unsigned char block[64];
    size_t used;
} md5_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, unsigned char digest[32]);
void sha256_hex(const unsigned char digest[32], char out[65]);     /* 64 lowercase hex digits and a NUL */

void sha512_init(sha512_ctx *c);
void sha512_update(sha512_ctx *c, const void *data, size_t len);
void sha512_final(sha512_ctx *c, unsigned char digest[64]);
void sha512_hex(const unsigned char digest[64], char out[129]);

void sha1_init(sha1_ctx *c);
void sha1_update(sha1_ctx *c, const void *data, size_t len);
void sha1_final(sha1_ctx *c, unsigned char digest[20]);

void md5_init(md5_ctx *c);
void md5_update(md5_ctx *c, const void *data, size_t len);
void md5_final(md5_ctx *c, unsigned char digest[16]);

/* ------------------------------------------------------------------ checking a bag */

/* What bagit_validate found, one call per file (path relative to the bag) or problem. */
enum bagit_event {
    BAGIT_OK,           /* every manifest's checksum matches (reported only when asked: BAGIT_VERBOSE) */
    BAGIT_FAILED,       /* a checksum differs; detail: the algorithm */
    BAGIT_MISSING,      /* a manifest names it, and it is not there (or cannot be read); detail: why */
    BAGIT_EXTRA,        /* a payload file no payload manifest names; detail: which manifest */
    BAGIT_INVALID       /* the bag itself: bagit.txt, data/, a manifest line, Payload-Oxum, fetch.txt */
};
typedef void (*bagit_callback)(void *ctx, enum bagit_event what, const char *path, const char *detail);

typedef struct {
    size_t payload_ok, tag_ok;              /* files whose checksums all match */
    size_t failed, missing, extra, invalid;
    char algorithms[64];                    /* the payload manifests', e.g. "sha256 sha512" */
} bagit_report;

#define BAGIT_FAST          1u  /* only completeness and Payload-Oxum, no checksums (as bagit.py --fast) */
#define BAGIT_VERBOSE       2u  /* report BAGIT_OK too */

/* Checks the bag at `bag` as RFC 8493 asks: bagit.txt, data/, every payload manifest (md5, sha1,
 * sha256, sha512) naming every payload file and nothing else, every checksum (each file read once
 * for all of them), the tag manifests, Payload-Oxum in bag-info.txt, and the files fetch.txt
 * names. cb (may be NULL) hears every problem. Returns 0 when the bag is valid, 1 when not. */
int bagit_validate(const char *bag, unsigned flags, bagit_report *report, bagit_callback cb, void *ctx);

#endif
