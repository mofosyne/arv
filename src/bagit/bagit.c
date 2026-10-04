/*
 * bagit: BagIt bags (RFC 8493) checked, and the digests they use (see bagit.h).
 *
 * A bag is a folder with bagit.txt, the payload in data/, and one or more payload manifests
 * (manifest-<algorithm>.txt: "<checksum> <path>" per line) that between them name every payload
 * file and nothing else; tag manifests (tagmanifest-<algorithm>.txt) do the same for any of the
 * other files they choose to name. Paths in manifests are relative to the bag, with "/" between
 * folders and %0A, %0D and %25 standing for line feed, carriage return and "%".
 */
#define _XOPEN_SOURCE 700
#include "bagit.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ SHA-256 */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha256_compress(uint32_t h[8], const unsigned char *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, k, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
    for (i = 0; i < 64; i++) {
        t1 = k + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

/* the 64-byte-block buffering shared by SHA-256, SHA-1 and MD5 */
static void update64(uint64_t *length, unsigned char block[64], size_t *used, const void *data, size_t len,
                     void (*compress)(uint32_t *, const unsigned char *), uint32_t *h)
{
    const unsigned char *p = data;
    *length += len;
    if (*used) {
        size_t take = 64 - *used < len ? 64 - *used : len;
        memcpy(block + *used, p, take);
        *used += take;
        p += take;
        len -= take;
        if (*used < 64) return;
        compress(h, block);
        *used = 0;
    }
    for (; len >= 64; p += 64, len -= 64) compress(h, p);
    memcpy(block, p, len);
    *used = len;
}

/* the final padding: 0x80, zeros, the length in bits (big-endian for SHA, little for MD5) */
static void pad64(uint64_t length, unsigned char block[64], size_t used, int little,
                  void (*compress)(uint32_t *, const unsigned char *), uint32_t *h)
{
    uint64_t bits = length * 8;
    block[used++] = 0x80;
    if (used > 56) {
        memset(block + used, 0, 64 - used);
        compress(h, block);
        used = 0;
    }
    memset(block + used, 0, 56 - used);
    for (int i = 0; i < 8; i++) block[56 + i] = (unsigned char)(little ? bits >> (8 * i) : bits >> (56 - 8 * i));
    compress(h, block);
}

static void hex_of(const unsigned char *digest, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15];
    }
    out[2 * n] = 0;
}

void sha256_init(sha256_ctx *c)
{
    static const uint32_t init[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(c->h, init, sizeof init);
    c->length = 0;
    c->used = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t len)
{
    update64(&c->length, c->block, &c->used, data, len, sha256_compress, c->h);
}

void sha256_final(sha256_ctx *c, unsigned char digest[32])
{
    pad64(c->length, c->block, c->used, 0, sha256_compress, c->h);
    for (int i = 0; i < 32; i++) digest[i] = (unsigned char)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

void sha256_hex(const unsigned char digest[32], char out[65])
{
    hex_of(digest, 32, out);
}

/* ------------------------------------------------------------------ SHA-512 */

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void sha512_compress(uint64_t h[8], const unsigned char *p)
{
    uint64_t w[80], a, b, c, d, e, f, g, k, t1, t2;
    int i, j;
    for (i = 0; i < 16; i++)
        for (w[i] = 0, j = 0; j < 8; j++) w[i] = w[i] << 8 | p[8 * i + j];
    for (i = 16; i < 80; i++) {
        uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
    for (i = 0; i < 80; i++) {
        t1 = k + (ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        t2 = (ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void sha512_init(sha512_ctx *c)
{
    static const uint64_t init[8] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
    };
    memcpy(c->h, init, sizeof init);
    c->length = 0;
    c->used = 0;
}

void sha512_update(sha512_ctx *c, const void *data, size_t len)
{
    const unsigned char *p = data;
    c->length += len;
    if (c->used) {
        size_t take = 128 - c->used < len ? 128 - c->used : len;
        memcpy(c->block + c->used, p, take);
        c->used += take;
        p += take;
        len -= take;
        if (c->used < 128) return;
        sha512_compress(c->h, c->block);
        c->used = 0;
    }
    for (; len >= 128; p += 128, len -= 128) sha512_compress(c->h, p);
    memcpy(c->block, p, len);
    c->used = len;
}

void sha512_final(sha512_ctx *c, unsigned char digest[64])
{
    uint64_t bits = c->length * 8;
    int i;
    c->block[c->used++] = 0x80;
    if (c->used > 112) {
        memset(c->block + c->used, 0, 128 - c->used);
        sha512_compress(c->h, c->block);
        c->used = 0;
    }
    memset(c->block + c->used, 0, 120 - c->used);   /* the high 64 bits of the length are 0 */
    for (i = 0; i < 8; i++) c->block[120 + i] = (unsigned char)(bits >> (56 - 8 * i));
    sha512_compress(c->h, c->block);
    for (i = 0; i < 64; i++) digest[i] = (unsigned char)(c->h[i / 8] >> (56 - 8 * (i % 8)));
}

void sha512_hex(const unsigned char digest[64], char out[129])
{
    hex_of(digest, 64, out);
}

/* ------------------------------------------------------------------ SHA-1 (older bags) */

static void sha1_compress(uint32_t h[5], const unsigned char *p)
{
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (i = 16; i < 80; i++) w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        uint32_t t = ROL32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL32(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_init(sha1_ctx *c)
{
    static const uint32_t init[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    memcpy(c->h, init, sizeof init);
    c->length = 0;
    c->used = 0;
}

void sha1_update(sha1_ctx *c, const void *data, size_t len)
{
    update64(&c->length, c->block, &c->used, data, len, sha1_compress, c->h);
}

void sha1_final(sha1_ctx *c, unsigned char digest[20])
{
    pad64(c->length, c->block, c->used, 0, sha1_compress, c->h);
    for (int i = 0; i < 20; i++) digest[i] = (unsigned char)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

/* ------------------------------------------------------------------ MD5 (older bags) */

static void md5_compress(uint32_t h[4], const unsigned char *p)
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    static const int S[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                               5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                               4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                               6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t m[16], a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 16; i++)
        m[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        uint32_t t = d;
        d = c;
        c = b;
        b = b + ROL32(a + f + K[i] + m[g], S[i]);
        a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

void md5_init(md5_ctx *c)
{
    static const uint32_t init[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    memcpy(c->h, init, sizeof init);
    c->length = 0;
    c->used = 0;
}

void md5_update(md5_ctx *c, const void *data, size_t len)
{
    update64(&c->length, c->block, &c->used, data, len, md5_compress, c->h);
}

void md5_final(md5_ctx *c, unsigned char digest[16])
{
    pad64(c->length, c->block, c->used, 1, md5_compress, c->h);
    for (int i = 0; i < 16; i++) digest[i] = (unsigned char)(c->h[i / 4] >> (8 * (i % 4)));
}

/* ------------------------------------------------------------------ small helpers */

static void *grow(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("bagit: out of memory\n", stderr);
        exit(2);
    }
    return q;
}

static char *copy_str(const char *s)
{
    size_t n = strlen(s) + 1;
    return memcpy(grow(NULL, n), s, n);
}

static char *path_join(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b);
    char *p = grow(NULL, na + nb + 2);
    memcpy(p, a, na);
    p[na] = '/';
    memcpy(p + na + 1, b, nb + 1);
    return p;
}

enum { MD5, SHA1, SHA256, SHA512, NALG };
static const char *const ALG[NALG] = { "md5", "sha1", "sha256", "sha512" };
static const int HEXLEN[NALG] = { 32, 40, 64, 128 };

static int alg_of(const char *name)
{
    for (int a = 0; a < NALG; a++)
        if (!strcmp(name, ALG[a])) return a;
    return -1;
}

/* the checksums of a file, for the algorithms in `want` (bits); -1 with errno when unreadable */
static int hash_file(const char *path, unsigned want, char hex[NALG][129], uint64_t *size)
{
    static unsigned char buf[1 << 20];
    md5_ctx m5;
    sha1_ctx s1;
    sha256_ctx s256;
    sha512_ctx s512;
    unsigned char d[64];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    md5_init(&m5); sha1_init(&s1); sha256_init(&s256); sha512_init(&s512);
    ssize_t n;
    *size = 0;
    while ((n = read(fd, buf, sizeof buf)) != 0) {
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            close(fd);
            errno = e;
            return -1;
        }
        *size += (uint64_t)n;
        if (want & 1u << MD5) md5_update(&m5, buf, (size_t)n);
        if (want & 1u << SHA1) sha1_update(&s1, buf, (size_t)n);
        if (want & 1u << SHA256) sha256_update(&s256, buf, (size_t)n);
        if (want & 1u << SHA512) sha512_update(&s512, buf, (size_t)n);
    }
    close(fd);
    if (want & 1u << MD5) { md5_final(&m5, d); hex_of(d, 16, hex[MD5]); }
    if (want & 1u << SHA1) { sha1_final(&s1, d); hex_of(d, 20, hex[SHA1]); }
    if (want & 1u << SHA256) { sha256_final(&s256, d); hex_of(d, 32, hex[SHA256]); }
    if (want & 1u << SHA512) { sha512_final(&s512, d); hex_of(d, 64, hex[SHA512]); }
    return 0;
}

/* ------------------------------------------------------------------ manifests */

typedef struct {
    char *path;
    char *hex[NALG];        /* NULL where that algorithm's manifest does not name it */
} entry;

typedef struct {
    entry *v;
    size_t n, cap;
    unsigned algs;          /* the manifests read (bits) */
} table;

typedef struct {
    const char *bag;
    unsigned flags;
    bagit_report *r;
    bagit_callback cb;
    void *ctx;
} job;

static void tell(job *j, enum bagit_event what, const char *path, const char *detail)
{
    if (what == BAGIT_FAILED) j->r->failed++;
    else if (what == BAGIT_MISSING) j->r->missing++;
    else if (what == BAGIT_EXTRA) j->r->extra++;
    else if (what == BAGIT_INVALID) j->r->invalid++;
    if (j->cb) j->cb(j->ctx, what, path, detail ? detail : "");
}

static int by_path(const void *a, const void *b)
{
    return strcmp(((const entry *)a)->path, ((const entry *)b)->path);
}

static entry *find(table *t, const char *path)
{
    entry key = { (char *)path, { 0 } };
    return t->n ? bsearch(&key, t->v, t->n, sizeof *t->v, by_path) : NULL;
}

/* %0A, %0D and %25 back to what they stand for (RFC 8493 2.1.3), in place */
static void percent_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (s[0] == '%' && s[1] && s[2]) {
            char a = s[1], b = (char)(s[2] | 0x20);
            if (a == '0' && b == 'a') { *o++ = '\n'; s += 2; continue; }
            if (a == '0' && b == 'd') { *o++ = '\r'; s += 2; continue; }
            if (a == '2' && s[2] == '5') { *o++ = '%'; s += 2; continue; }
        }
        *o++ = *s;
    }
    *o = 0;
}

/* a path a manifest may name: relative, no "." or ".." parts, no empty parts */
static int safe_path(const char *p)
{
    if (!*p || *p == '/') return 0;
    for (const char *s = p; *s;) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (!n || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
        s += n + (e ? 1 : 0);
        if (e && !*s) return 0;
    }
    return 1;
}

/* reads one manifest (file: its name in the bag) into t; payload: its paths must be under data/ */
static void read_manifest(job *j, const char *file, int alg, int payload, table *t)
{
    char *full = path_join(j->bag, file), *line = NULL;
    FILE *fp = fopen(full, "rb");
    size_t cap = 0, lineno = 0;
    ssize_t len;
    free(full);
    if (!fp) {
        tell(j, BAGIT_INVALID, file, strerror(errno));
        return;
    }
    t->algs |= 1u << alg;
    size_t before = t->n;
    while ((len = getline(&line, &cap, fp)) >= 0) {
        lineno++;
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (!len) continue;
        char *sep = line;
        while (*sep && *sep != ' ' && *sep != '\t') sep++;
        char *path = sep;
        while (*path == ' ' || *path == '\t') path++;
        if (*path == '*') path++;               /* sha256sum's binary-mode marker */
        char detail[96];
        if (!*sep || !*path || sep - line != HEXLEN[alg]) {
            snprintf(detail, sizeof detail, "%s line %zu: not \"<checksum> <path>\"", file, lineno);
            tell(j, BAGIT_INVALID, file, detail);
            continue;
        }
        *sep = 0;
        for (char *h = line; *h; h++) *h = (char)(*h >= 'A' && *h <= 'F' ? *h + 32 : *h);
        percent_decode(path);
        if (!safe_path(path) || (payload && strncmp(path, "data/", 5)) || (!payload && !strncmp(path, "data/", 5))) {
            snprintf(detail, sizeof detail, "%s line %zu: a path it may not name", file, lineno);
            tell(j, BAGIT_INVALID, path, detail);
            continue;
        }
        if (t->n == t->cap) t->v = grow(t->v, (t->cap = t->cap ? 2 * t->cap : 1024) * sizeof *t->v);
        memset(&t->v[t->n], 0, sizeof *t->v);
        t->v[t->n].path = copy_str(path);
        t->v[t->n].hex[alg] = copy_str(line);
        t->n++;
    }
    free(line);
    fclose(fp);
    (void)before;
}

/* entries of the same path (from several manifests) merged into one; sorted */
static void merge(table *t)
{
    if (!t->n) return;
    qsort(t->v, t->n, sizeof *t->v, by_path);
    size_t o = 0;
    for (size_t i = 1; i < t->n; i++) {
        if (!strcmp(t->v[i].path, t->v[o].path)) {
            for (int a = 0; a < NALG; a++)
                if (t->v[i].hex[a]) {
                    free(t->v[o].hex[a]);       /* a path named twice in one manifest: the last wins */
                    t->v[o].hex[a] = t->v[i].hex[a];
                }
            free(t->v[i].path);
        } else {
            t->v[++o] = t->v[i];
        }
    }
    t->n = o + 1;
}

static void free_table(table *t)
{
    for (size_t i = 0; i < t->n; i++) {
        free(t->v[i].path);
        for (int a = 0; a < NALG; a++) free(t->v[i].hex[a]);
    }
    free(t->v);
}

/* ------------------------------------------------------------------ the payload on disk */

typedef struct {
    char **v;
    size_t n, cap;
    uint64_t bytes;
} files;

static int by_string(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void walk(job *j, const char *rel, files *f)
{
    char *dir = path_join(j->bag, rel);
    DIR *dp = opendir(dir);
    struct dirent *e;
    if (!dp) {
        tell(j, BAGIT_INVALID, rel, strerror(errno));
        free(dir);
        return;
    }
    while ((e = readdir(dp))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *r = path_join(rel, e->d_name), *full = path_join(j->bag, r);
        struct stat st;
        if (stat(full, &st)) {
            tell(j, BAGIT_INVALID, r, strerror(errno));
            free(r);
        } else if (S_ISDIR(st.st_mode)) {
            walk(j, r, f);
            free(r);
        } else {
            if (f->n == f->cap) f->v = grow(f->v, (f->cap = f->cap ? 2 * f->cap : 1024) * sizeof *f->v);
            f->v[f->n++] = r;
            f->bytes += (uint64_t)st.st_size;
        }
        free(full);
    }
    closedir(dp);
    free(dir);
}

/* ------------------------------------------------------------------ the tag files */

/* the value of `name` in a "Name: value" tag file (first one; NULL if none) */
static char *tag_value(const char *bag, const char *file, const char *name)
{
    char *full = path_join(bag, file), *line = NULL, *found = NULL;
    FILE *fp = fopen(full, "rb");
    size_t cap = 0, n = strlen(name);
    ssize_t len;
    free(full);
    if (!fp) return NULL;
    while (!found && (len = getline(&line, &cap, fp)) >= 0) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (!strncmp(line, name, n) && line[n] == ':') {
            char *v = line + n + 1;
            while (*v == ' ' || *v == '\t') v++;
            found = copy_str(v);
        }
    }
    free(line);
    fclose(fp);
    return found;
}

/* checks the files in t (payload or tag) against their checksums; ok counts the good ones */
static void check(job *j, table *t, size_t *ok)
{
    for (size_t i = 0; i < t->n; i++) {
        entry *e = &t->v[i];
        char hex[NALG][129], *full = path_join(j->bag, e->path);
        unsigned want = 0;
        uint64_t size;
        for (int a = 0; a < NALG; a++)
            if (e->hex[a]) want |= 1u << a;
        if (j->flags & BAGIT_FAST) {
            struct stat st;
            if (stat(full, &st)) tell(j, BAGIT_MISSING, e->path, strerror(errno));
            else (*ok)++;
        } else if (hash_file(full, want, hex, &size)) {
            tell(j, BAGIT_MISSING, e->path, strerror(errno));
        } else {
            char bad[64] = "";
            for (int a = 0; a < NALG; a++)
                if (e->hex[a] && strcmp(e->hex[a], hex[a]))
                    snprintf(bad + strlen(bad), sizeof bad - strlen(bad), "%s%s", *bad ? " " : "", ALG[a]);
            if (*bad) tell(j, BAGIT_FAILED, e->path, bad);
            else {
                (*ok)++;
                if (j->flags & BAGIT_VERBOSE) tell(j, BAGIT_OK, e->path, NULL);
            }
        }
        free(full);
    }
}

int bagit_validate(const char *bag, unsigned flags, bagit_report *report, bagit_callback cb, void *ctx)
{
    bagit_report local;
    if (!report) report = &local;
    memset(report, 0, sizeof *report);
    job j = { bag, flags, report, cb, ctx };

    /* bagit.txt: the version and the tag files' encoding */
    char *version = tag_value(bag, "bagit.txt", "BagIt-Version"), *enc = tag_value(bag, "bagit.txt", "Tag-File-Character-Encoding");
    if (!version || !enc) tell(&j, BAGIT_INVALID, "bagit.txt", "missing, or without BagIt-Version and Tag-File-Character-Encoding");
    free(version);
    free(enc);

    /* the manifests there are */
    table payload = { 0 }, tags = { 0 };
    DIR *dp = opendir(bag);
    struct dirent *e;
    if (!dp) {
        tell(&j, BAGIT_INVALID, ".", strerror(errno));
        return 1;
    }
    char *names[64];
    size_t nnames = 0;
    while ((e = readdir(dp)) && nnames < 64) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (len > 4 && !strcmp(n + len - 4, ".txt") && (!strncmp(n, "manifest-", 9) || !strncmp(n, "tagmanifest-", 12)))
            names[nnames++] = copy_str(n);
    }
    closedir(dp);
    qsort(names, nnames, sizeof *names, by_string);
    for (size_t i = 0; i < nnames; i++) {
        int is_tag = !strncmp(names[i], "tag", 3);
        char alg[16];
        snprintf(alg, sizeof alg, "%.*s", (int)(strlen(names[i]) - 4 - (is_tag ? 12 : 9)), names[i] + (is_tag ? 12 : 9));
        int a = alg_of(alg);
        if (a < 0) {
            char detail[128];
            snprintf(detail, sizeof detail, "algorithm %s is not supported (md5, sha1, sha256, sha512 are)", alg);
            tell(&j, BAGIT_INVALID, names[i], detail);
        } else {
            read_manifest(&j, names[i], a, !is_tag, is_tag ? &tags : &payload);
            if (!is_tag) snprintf(report->algorithms + strlen(report->algorithms), sizeof report->algorithms - strlen(report->algorithms),
                                  "%s%s", *report->algorithms ? " " : "", alg);
        }
        free(names[i]);
    }
    if (!payload.algs) tell(&j, BAGIT_INVALID, "manifest-<algorithm>.txt", "no payload manifest");
    merge(&payload);
    merge(&tags);

    /* the payload: every file named by every payload manifest, and none other */
    files f = { 0 };
    struct stat st;
    char *data = path_join(bag, "data");
    if (stat(data, &st) || !S_ISDIR(st.st_mode)) tell(&j, BAGIT_INVALID, "data/", "no payload folder");
    else walk(&j, "data", &f);
    free(data);
    if (f.n) qsort(f.v, f.n, sizeof *f.v, by_string);
    for (size_t i = 0; i < f.n; i++) {
        entry *m = find(&payload, f.v[i]);
        if (!m) {
            tell(&j, BAGIT_EXTRA, f.v[i], "no payload manifest names it");
            continue;
        }
        for (int a = 0; a < NALG; a++)
            if ((payload.algs & 1u << a) && !m->hex[a]) {
                char detail[48];
                snprintf(detail, sizeof detail, "not in manifest-%s.txt", ALG[a]);
                tell(&j, BAGIT_EXTRA, f.v[i], detail);
            }
    }

    /* Payload-Oxum: <bytes>.<files> */
    char *oxum = tag_value(bag, "bag-info.txt", "Payload-Oxum");
    if (oxum) {
        char want[64];
        snprintf(want, sizeof want, "%llu.%zu", (unsigned long long)f.bytes, f.n);
        if (strcmp(oxum, want)) {
            char detail[160];
            snprintf(detail, sizeof detail, "Payload-Oxum is %s, the payload is %s (bytes.files)", oxum, want);
            tell(&j, BAGIT_INVALID, "bag-info.txt", detail);
        }
        free(oxum);
    }

    /* fetch.txt: "<url> <length> <path>": every file it names must be in the payload manifests */
    char *fetch = path_join(bag, "fetch.txt"), *line = NULL;
    FILE *fp = fopen(fetch, "rb");
    if (fp) {
        size_t cap = 0;
        ssize_t len;
        while ((len = getline(&line, &cap, fp)) >= 0) {
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
            char *p = line;
            for (int k = 0; k < 2 && p; k++) {
                p = strpbrk(p, " \t");
                while (p && (*p == ' ' || *p == '\t')) p++;
            }
            if (!p || !*p) continue;
            percent_decode(p);
            if (!find(&payload, p))         /* one that is: checked with the manifests (missing until fetched) */
                tell(&j, BAGIT_INVALID, p, "named in fetch.txt but not in the payload manifests");
        }
        free(line);
        fclose(fp);
    }
    free(fetch);

    /* the checksums */
    check(&j, &payload, &report->payload_ok);
    check(&j, &tags, &report->tag_ok);

    for (size_t i = 0; i < f.n; i++) free(f.v[i]);
    free(f.v);
    free_table(&payload);
    free_table(&tags);
    return report->failed || report->missing || report->extra || report->invalid ? 1 : 0;
}
