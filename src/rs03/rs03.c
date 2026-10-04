/*
 * rs03: dvdisaster's RS03 error correction for disc images (see rs03.h), in C99 and POSIX.
 *
 * The format, all from dvdisaster 0.79.10 / dvdisaster Light (GPLv3):
 *   - the image is seen as 255 layers of sectors_per_layer = medium / 255 sectors each;
 *   - layers 0 .. ndata-2 are the data: the image, then the ecc header (2 sectors), then padding
 *     sectors up to the CRC layer;
 *   - layer ndata-1 is the CRC layer: its sector n is a CrcBlock holding the CRC-32 of sector n+1
 *     of every data layer (sector 0's for the last n), and the layout;
 *   - layers ndata .. 254 hold the parity: for each sector position n and byte b, the 255 bytes
 *     b of the sectors at position n of all layers form one Reed-Solomon codeword over GF(2^8)
 *     (generator polynomial 0x187, first root 112, primitive element 11, as in CCSDS).
 * A scratch across many neighbouring sectors thus costs each codeword one symbol at most.
 */
#define _XOPEN_SOURCE 700
#include "rs03.h"

#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SECTOR 2048
#define FIELDMAX 255
#define A0 FIELDMAX                     /* log(0) */
#define GENERATOR 0x187
#define FIRST_ROOT 112
#define PRIM_ELEM 11
#define FINGERPRINT_SECTOR 16
#define CREATOR_VERSION 7910            /* what dvdisaster 0.79.10 and dvdisaster Light write */
#define NEEDED_VERSION 7900
#define RELEASE_FLAGS 1                 /* methodFlags[3]: MFLAG_DEVEL, as those builds set it */
#define SELF_CRC_PLACEHOLDER 0x4c5047u  /* selfCRC while the CRC is computed */
#define CHUNK 128                       /* sector positions encoded at once */
#define MAX_THREADS 32

/* ------------------------------------------------------------------ CRC-32 (dvdisaster's: no final inversion) */

static uint32_t crctab[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crctab[i] = c;
    }
}

static uint32_t crc32_dv(const unsigned char *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) crc = crctab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

/* ------------------------------------------------------------------ MD5 (RFC 1321), for the medium fingerprint */

typedef struct {
    uint32_t a, b, c, d;
    uint64_t len;
    unsigned char buf[64];
} md5_ctx;

static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }

static void md5_block(md5_ctx *m, const unsigned char *p)
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const int S[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                               5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                               4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                               6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t w[16], a = m->a, b = m->b, c = m->c, d = m->d;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
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
        b = b + rol(a + f + K[i] + w[g], S[i]);
        a = t;
    }
    m->a += a; m->b += b; m->c += c; m->d += d;
}

static void md5(const unsigned char *p, size_t n, unsigned char out[16])
{
    md5_ctx m = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0, { 0 } };
    size_t i = 0;
    for (; i + 64 <= n; i += 64) md5_block(&m, p + i);
    unsigned char tail[128] = { 0 };
    size_t rest = n - i;
    memcpy(tail, p + i, rest);
    tail[rest] = 0x80;
    size_t tl = rest + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int k = 0; k < 8; k++) tail[tl - 8 + k] = (unsigned char)(bits >> (8 * k));
    md5_block(&m, tail);
    if (tl == 128) md5_block(&m, tail + 64);
    uint32_t v[4] = { m.a, m.b, m.c, m.d };
    for (int k = 0; k < 16; k++) out[k] = (unsigned char)(v[k / 4] >> (8 * (k % 4)));
}

/* ------------------------------------------------------------------ GF(2^8) and the RS generator */

static int alpha_to[FIELDMAX + 1], index_of[FIELDMAX + 1];

static void gf_init(void)
{
    int b = 1;
    for (int log = 0; log < FIELDMAX; log++) {
        index_of[b] = log;
        alpha_to[log] = b;
        b <<= 1;
        if (b & 256) b ^= GENERATOR;
    }
    index_of[0] = A0;
    alpha_to[A0] = 0;
}

static int modmax(int x)
{
    while (x >= FIELDMAX) x -= FIELDMAX;
    return x;
}

/* the generator polynomial's coefficients, as logarithms (CreateReedSolomonTables) */
static void make_gpoly(int nroots, int *gpoly)
{
    gpoly[0] = 1;
    for (int i = 0, root = FIRST_ROOT * PRIM_ELEM; i < nroots; i++, root += PRIM_ELEM) {
        gpoly[i + 1] = 1;
        for (int j = i; j > 0; j--)
            gpoly[j] = gpoly[j] ? gpoly[j - 1] ^ alpha_to[modmax(index_of[gpoly[j]] + root)] : gpoly[j - 1];
        gpoly[0] = alpha_to[modmax(index_of[gpoly[0]] + root)];
    }
    for (int i = 0; i <= nroots; i++) gpoly[i] = index_of[gpoly[i]];
}

/* ------------------------------------------------------------------ layout */

static int roots_for(uint64_t data_sectors, uint64_t medium)
{
    uint64_t spl = medium / FIELDMAX;
    if (!spl) return -1;
    return FIELDMAX - (int)((data_sectors + 2 + spl - 1) / spl) - 1;
}

int rs03_layout_for(uint64_t data_sectors, uint64_t medium, int no_dm, rs03_layout *lay, const char **err)
{
    memset(lay, 0, sizeof *lay);
    if (medium) {
        if (data_sectors >= medium) { *err = "the medium is smaller than the image"; return -1; }
        if (medium < FIELDMAX) { *err = "the medium is too small (at least 255 sectors)"; return -1; }
        if (roots_for(data_sectors, medium) < 8) { *err = "not enough room on the medium for error correction (8 roots at least)"; return -1; }
    } else {                        /* the smallest standard medium with 8 roots or more */
        const uint64_t ladder[] = { RS03_CDR, RS03_DVD_SL, RS03_DVD_DL, RS03_BD_SL_NODM, RS03_BD_SL, RS03_BD_DL_NODM,
                                    RS03_BD_DL, RS03_BDXL_TL_NODM, RS03_BDXL_TL, RS03_BDXL_QL_NODM, RS03_BDXL_QL };
        const int nodm_only[] = { 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0 };
        for (size_t i = 0; i < sizeof ladder / sizeof *ladder && !medium; i++)
            if ((no_dm || !nodm_only[i]) && roots_for(data_sectors, ladder[i]) >= 8) medium = ladder[i];
        if (!medium) medium = no_dm ? RS03_BDXL_QL_NODM : RS03_BDXL_QL;
    }
    lay->data_sectors = data_sectors;
    lay->medium_sectors = medium;
    lay->sectors_per_layer = medium / FIELDMAX;
    lay->total_sectors = FIELDMAX * lay->sectors_per_layer;
    uint64_t ndata = (data_sectors + 2 + lay->sectors_per_layer - 1) / lay->sectors_per_layer;
    if (ndata < 84) ndata = 84;     /* redundancy is clipped at 170 roots */
    lay->data_padding = ndata * lay->sectors_per_layer - data_sectors - 2;
    lay->ndata = (int)ndata + 1;    /* the CRC layer counts as data */
    lay->nroots = FIELDMAX - lay->ndata;
    lay->redundancy = lay->nroots * 100.0 / lay->ndata;
    lay->first_crc = (uint64_t)(lay->ndata - 1) * lay->sectors_per_layer;
    lay->first_ecc = lay->first_crc + lay->sectors_per_layer;
    return 0;
}

void rs03_describe(const rs03_layout *lay, char *out, size_t outlen)
{
    unsigned long long ecc = (unsigned long long)lay->nroots * lay->sectors_per_layer;
    if (lay->data_padding)
        snprintf(out, outlen, "%llu MiB data, %llu MiB ecc (%d roots; %4.1f%% redundancy), %llu MiB padding.",
                 (unsigned long long)lay->data_sectors / 512, ecc / 512, lay->nroots, lay->redundancy,
                 (unsigned long long)lay->data_padding / 512);
    else
        snprintf(out, outlen, "%llu MiB data, %llu MiB ecc (%d roots; %4.1f%% redundancy).",
                 (unsigned long long)lay->data_sectors / 512, ecc / 512, lay->nroots, lay->redundancy);
}

/* ------------------------------------------------------------------ the sectors dvdisaster writes */

static void put32(unsigned char *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

static void put64(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* the EccHeader struct, 4096 bytes (dvdisaster.h; the field table in docs/rs03.md) */
static void make_header(unsigned char h[4096], const rs03_layout *lay, const unsigned char fp[16], uint32_t in_last)
{
    memset(h, 0, 4096);
    memcpy(h, "*dvdisaster*", 12);
    memcpy(h + 12, "RS03", 4);
    h[16 + 3] = RELEASE_FLAGS;
    memcpy(h + 20, fp, 16);                     /* mediumFP; mediumSum and eccSum stay zero */
    put64(h + 68, lay->data_sectors);
    put32(h + 76, (uint32_t)lay->ndata);
    put32(h + 80, (uint32_t)lay->nroots);
    put32(h + 84, CREATOR_VERSION);
    put32(h + 88, NEEDED_VERSION);
    put32(h + 92, FINGERPRINT_SECTOR);
    put32(h + 116, in_last);                    /* inLast: bytes in the image's last sector */
    put64(h + 120, lay->sectors_per_layer);
    put32(h + 96, SELF_CRC_PLACEHOLDER);
    put32(h + 96, crc32_dv(h, 4096));
}

/* the CrcBlock fields after the 256 CRCs (dvdisaster.h), and its own CRC */
static void finish_crc_block(unsigned char b[SECTOR], const rs03_layout *lay, const unsigned char fp[16], uint32_t in_last)
{
    unsigned char *p = b + 1024;
    memset(p, 0, SECTOR - 1024);
    memcpy(p, "*dvdisaster*", 12);
    memcpy(p + 12, "RS03", 4);
    p[16 + 3] = RELEASE_FLAGS;
    put32(p + 20, CREATOR_VERSION);
    put32(p + 24, NEEDED_VERSION);
    put32(p + 28, FINGERPRINT_SECTOR);
    memcpy(p + 32, fp, 16);                     /* mediumFP, then mediumSum (zero) */
    put64(p + 64, lay->data_sectors);           /* offset 1088: 8-aligned */
    put32(p + 72, in_last);
    put32(p + 76, (uint32_t)lay->ndata);
    put32(p + 80, (uint32_t)lay->nroots);
    put64(p + 88, lay->sectors_per_layer);      /* offset 1112: 8-aligned */
    put32(p + 96, SELF_CRC_PLACEHOLDER);
    put32(p + 96, crc32_dv(b, SECTOR));
}

/* CreatePaddingSector (ds-marker.c) */
static void make_padding(unsigned char s[SECTOR], uint64_t sector, const unsigned char fp[16])
{
    static const char end[] = "dvdisaster padding sector end marker";
    memset(s, 0, SECTOR);
    strcpy((char *)s, "dvdisaster padding sector       "
                      "This is a padding sector needed for augmenting the image with error correction data.");
    memcpy(s + 2047 - (sizeof end - 1), end, sizeof end - 1);
    strcpy((char *)s + 0x100, "Padding sector marker version");
    strcpy((char *)s + 0x120, "1.00");
    strcpy((char *)s + 0x140, "Padding sector number");
    snprintf((char *)s + 0x160, 0x20, "%llu", (unsigned long long)sector);
    strcpy((char *)s + 0x180, "Medium fingerprint");
    memcpy(s + 0x1a0, fp, 16);
    strcpy((char *)s + 0x1c0, "Medium fingerprint sector");
    snprintf((char *)s + 0x1e0, 0x20, "%d", FINGERPRINT_SECTOR);
}

/* ------------------------------------------------------------------ the encoder */

/* One thread's share of a chunk: codewords from..to (byte positions), all layers. The shift
   register of each codeword is `stride` bytes (nroots rounded up to 8, for 8-byte XORs); it is
   circular, and its head moves one place per layer (as in dvdisaster), so after the last layer it
   is back at 0 and byte k of the register is parity layer k. */
typedef struct {
    const unsigned char *const *layers;     /* ndata pointers to this chunk's sectors */
    unsigned char *parity;
    const unsigned char (*lut)[2 * FIELDMAX + 8];
    size_t from, to, stride;
    int ndata, nroots, shift_init, g0;
} job;

static void *encode(void *arg)
{
    const job *jb = arg;
    size_t stride = jb->stride, words = stride / 8;
    for (int l = 0; l < jb->ndata; l++) {
        const unsigned char *d = jb->layers[l];
        int shift = (jb->shift_init + l) % jb->nroots, off = jb->nroots - shift - 1;
        unsigned char *reg = jb->parity + jb->from * stride;
        for (size_t i = jb->from; i < jb->to; i++, reg += stride) {
            int f = index_of[d[i] ^ reg[shift]];
            if (f != A0) {
                const unsigned char *row = jb->lut[f] + off;
                for (size_t w = 0; w < words; w++) {       /* reg ^= row, 8 bytes at a time */
                    uint64_t a, b;
                    memcpy(&a, reg + 8 * w, 8);
                    memcpy(&b, row + 8 * w, 8);
                    a ^= b;
                    memcpy(reg + 8 * w, &a, 8);
                }
                reg[shift] = (unsigned char)alpha_to[modmax(f + jb->g0)];
            } else {
                reg[shift] = 0;
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ augmenting */

static int io_error(char *err, size_t errlen, const char *what, const char *path)
{
    snprintf(err, errlen, "%s %s: %s", what, path, strerror(errno));
    return -1;
}

static int pread_all(int fd, unsigned char *buf, size_t n, uint64_t off)
{
    while (n) {
        ssize_t r = pread(fd, buf, n, (off_t)off);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            if (!r) errno = EIO;
            return -1;
        }
        buf += r;
        n -= (size_t)r;
        off += (uint64_t)r;
    }
    return 0;
}

static int pwrite_all(int fd, const unsigned char *buf, size_t n, uint64_t off)
{
    while (n) {
        ssize_t r = pwrite(fd, buf, n, (off_t)off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += r;
        n -= (size_t)r;
        off += (uint64_t)r;
    }
    return 0;
}

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The CRC layer and the parity of the whole image, written (check NULL) or compared with what the
   image holds (check: the counts of what differs). */
static int encode_image(int fd, const char *path, const rs03_layout *lay, const unsigned char fp[16], uint32_t in_last,
                        rs03_report *check, char *err, size_t errlen)
{
    int rc = -1, ndata = lay->ndata, nroots = lay->nroots;
    uint64_t spl = lay->sectors_per_layer;
    size_t stride = ((size_t)nroots + 7) & ~(size_t)7;
    int *gpoly = malloc((size_t)(nroots + 1) * sizeof *gpoly);
    unsigned char (*lut)[2 * FIELDMAX + 8] = calloc(FIELDMAX, sizeof *lut);
    unsigned char *data = malloc((size_t)(ndata - 1) * (CHUNK + 1) * SECTOR), *crcs = malloc((size_t)CHUNK * SECTOR);
    unsigned char *parity = malloc((size_t)CHUNK * SECTOR * stride), *slice = malloc((size_t)CHUNK * SECTOR);
    unsigned char *stored = check ? malloc((size_t)CHUNK * SECTOR) : NULL;
    uint32_t *first = calloc(256, sizeof *first);
    /* checking: positions whose data or CRC sector is bad (their parity cannot agree), one more for the
       chain into the next chunk, and what position 0's parity counted (it is found bad only at the end) */
    unsigned char badpos[CHUNK + 1];
    int carry = 0, pos0_bad = 0;
    uint64_t pos0_ecc = 0;
    if (!gpoly || !lut || !data || !crcs || !parity || !slice || !first || (check && !stored)) {
        errno = ENOMEM;
        io_error(err, errlen, "out of memory for", path);
        goto done;
    }
    /* the generator, and per feedback value its products, twice over (no wrap-around) */
    make_gpoly(nroots, gpoly);
    for (int f = 0; f < FIELDMAX; f++)
        for (int i = 0; i < nroots; i++)
            lut[f][i] = lut[f][nroots + i] = (unsigned char)alpha_to[modmax(f + gpoly[nroots - 1 - i])];
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int threads = cpus < 1 ? 1 : cpus > MAX_THREADS ? MAX_THREADS : (int)cpus;
    int shift_init = nroots - ndata % nroots;
    if (shift_init == nroots) shift_init = 0;

    for (uint64_t c = 0; c < spl; c += CHUNK) {
        uint64_t m = spl - c < CHUNK ? spl - c : CHUNK, extra = c + m < spl;
        /* the data layers' sectors at these positions, and one more for the CRC chain */
        for (int l = 0; l < ndata - 1; l++)
            if (pread_all(fd, data + (size_t)l * (CHUNK + 1) * SECTOR, (size_t)(m + extra) * SECTOR, ((uint64_t)l * spl + c) * SECTOR)) {
                io_error(err, errlen, "cannot read", path);
                goto done;
            }
        if (!c)
            for (int l = 0; l < ndata - 1; l++) first[l] = crc32_dv(data + (size_t)l * (CHUNK + 1) * SECTOR, SECTOR);
        /* the CRC layer: each sector holds the CRCs of the next position (the first, for the last) */
        memset(crcs, 0, (size_t)m * SECTOR);
        for (uint64_t n = 0; n < m; n++) {
            unsigned char *b = crcs + n * SECTOR;
            for (int l = 0; l < ndata - 1; l++)
                put32(b + 4 * l, c + n < spl - 1 ? crc32_dv(data + ((size_t)l * (CHUNK + 1) + n + 1) * SECTOR, SECTOR) : first[l]);
            finish_crc_block(b, lay, fp, in_last);
        }
        uint64_t crc_at = (lay->first_crc + c) * SECTOR;
        if (!check) {
            if (pwrite_all(fd, crcs, (size_t)m * SECTOR, crc_at)) { io_error(err, errlen, "cannot write", path); goto done; }
        } else {
            if (pread_all(fd, stored, (size_t)m * SECTOR, crc_at)) { io_error(err, errlen, "cannot read", path); goto done; }
            memset(badpos, 0, sizeof badpos);
            badpos[0] = (unsigned char)carry;
            carry = 0;
            for (uint64_t n = 0; n < m; n++) {
                const unsigned char *want = crcs + n * SECTOR, *have = stored + n * SECTOR;
                unsigned char block[SECTOR];             /* its fields, and its own CRC */
                memcpy(block, have, SECTOR);
                put32(block + 1024 + 96, SELF_CRC_PLACEHOLDER);
                if (memcmp(want + 1024, have + 1024, 96) || crc32_dv(block, SECTOR) != get32(have + 1024 + 96)) {
                    check->bad_crc++;                   /* a damaged CRC sector: its CRCs say nothing */
                    badpos[n] = 1;
                    continue;
                }
                int bad = 0;                            /* the CRCs are of the next position's sectors */
                for (int l = 0; l < ndata - 1; l++)
                    if (memcmp(want + 4 * l, have + 4 * l, 4)) { check->bad_data++; bad = 1; }
                if (bad) {
                    if (c + n == spl - 1) pos0_bad = 1;
                    else if (n + 1 < m) badpos[n + 1] = 1;
                    else carry = 1;
                }
            }
        }
        /* Reed-Solomon: one codeword per byte position, the layers as its symbols in order,
           shared out among the processors */
        size_t nbytes = (size_t)m * SECTOR;
        memset(parity, 0, nbytes * stride);
        const unsigned char *layers[FIELDMAX];
        for (int l = 0; l < ndata - 1; l++) layers[l] = data + (size_t)l * (CHUNK + 1) * SECTOR;
        layers[ndata - 1] = check ? stored : crcs;     /* the parity protects the CRC layer as it is */
        job jobs[MAX_THREADS];
        pthread_t tid[MAX_THREADS];
        int started[MAX_THREADS] = { 0 };
        for (int t = 0; t < threads; t++) {
            jobs[t] = (job){ layers, parity, (const unsigned char (*)[2 * FIELDMAX + 8])lut, nbytes * (size_t)t / (size_t)threads,
                             nbytes * (size_t)(t + 1) / (size_t)threads, stride, ndata, nroots, shift_init, gpoly[0] };
            started[t] = t && !pthread_create(&tid[t], NULL, encode, &jobs[t]);
            if (t && !started[t]) encode(&jobs[t]);          /* no thread: do it here */
        }
        encode(&jobs[0]);
        for (int t = 1; t < threads; t++)
            if (started[t]) pthread_join(tid[t], NULL);
        for (int k = 0; k < nroots; k++) {
            for (size_t i = 0; i < nbytes; i++) slice[i] = parity[i * stride + (size_t)k];
            uint64_t at = (lay->first_ecc + (uint64_t)k * spl + c) * SECTOR;
            if (!check) {
                if (pwrite_all(fd, slice, nbytes, at)) { io_error(err, errlen, "cannot write", path); goto done; }
            } else {
                if (pread_all(fd, crcs, nbytes, at)) { io_error(err, errlen, "cannot read", path); goto done; }
                for (uint64_t n = 0; n < m; n++)
                    if (!badpos[n] && memcmp(slice + n * SECTOR, crcs + n * SECTOR, SECTOR)) {
                        check->bad_ecc++;
                        if (!c && !n) pos0_ecc++;
                    }
            }
        }
    }
    if (check && pos0_bad) check->bad_ecc -= pos0_ecc;
    rc = 0;
done:
    free(gpoly); free(lut); free(data); free(crcs); free(parity); free(slice); free(stored); free(first);
    return rc;
}

static void setup(void)
{
    static int ready;
    if (!ready) {
        crc_init();
        gf_init();
        ready = 1;
    }
}

int rs03_augment(const char *path, uint64_t medium, int no_dm, rs03_layout *out, char *err, size_t errlen)
{
    setup();
    int fd = open(path, O_RDWR);
    if (fd < 0) return io_error(err, errlen, "cannot open", path);
    struct stat st;
    if (fstat(fd, &st)) { close(fd); return io_error(err, errlen, "cannot read", path); }
    uint64_t size = (uint64_t)st.st_size, data_sectors = (size + SECTOR - 1) / SECTOR;
    if (data_sectors <= FINGERPRINT_SECTOR) {
        close(fd);
        snprintf(err, errlen, "%s is too small for a disc image", path);
        return -1;
    }
    rs03_layout lay;
    const char *why = NULL;
    if (rs03_layout_for(data_sectors, medium, no_dm, &lay, &why)) {
        close(fd);
        snprintf(err, errlen, "%s: %s", path, why);
        return -1;
    }
    if (out) *out = lay;
    int rc = -1;
    unsigned char fp[16], sector[4096];
    /* the image to whole sectors, then the header, padding, and room for the rest */
    if (size % SECTOR) {
        memset(sector, 0, SECTOR);
        if (pwrite_all(fd, sector, SECTOR - size % SECTOR, size)) { io_error(err, errlen, "cannot write", path); goto done; }
    }
    if (pread_all(fd, sector, SECTOR, (uint64_t)FINGERPRINT_SECTOR * SECTOR)) { io_error(err, errlen, "cannot read", path); goto done; }
    md5(sector, SECTOR, fp);
    uint32_t in_last = size % SECTOR ? (uint32_t)(size % SECTOR) : SECTOR;
    make_header(sector, &lay, fp, in_last);
    if (pwrite_all(fd, sector, 4096, data_sectors * SECTOR)) { io_error(err, errlen, "cannot write", path); goto done; }
    for (uint64_t s = data_sectors + 2; s < lay.first_crc; s++) {
        make_padding(sector, s, fp);
        if (pwrite_all(fd, sector, SECTOR, s * SECTOR)) { io_error(err, errlen, "cannot write", path); goto done; }
    }
    if (ftruncate(fd, (off_t)(lay.total_sectors * SECTOR))) { io_error(err, errlen, "cannot extend", path); goto done; }
    rc = encode_image(fd, path, &lay, fp, in_last, NULL, err, errlen);
done:
    if (close(fd) && !rc) rc = io_error(err, errlen, "cannot write", path);
    return rc;
}

static uint64_t get64(const unsigned char *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}


int rs03_verify(const char *path, rs03_report *r, char *err, size_t errlen)
{
    setup();
    memset(r, 0, sizeof *r);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return io_error(err, errlen, "cannot open", path);
    struct stat st;
    int rc = -1;
    unsigned char b[4096];
    if (fstat(fd, &st)) { io_error(err, errlen, "cannot read", path); goto done; }
    uint64_t sectors = (uint64_t)st.st_size / SECTOR;
    if ((uint64_t)st.st_size % SECTOR || sectors % FIELDMAX || !sectors) {
        snprintf(err, errlen, "%s is not an RS03 augmented image (its size is not 255 layers of sectors)", path);
        goto done;
    }
    /* the layout, from the first CRC sector: at layer ndata-1 for some ndata of 85..254 */
    uint64_t spl = sectors / FIELDMAX, data_sectors = 0;
    int ndata = 0;
    for (int nd = 85; nd < FIELDMAX && !ndata; nd++) {
        if (pread_all(fd, b, SECTOR, (uint64_t)(nd - 1) * spl * SECTOR)) continue;
        uint32_t want = get32(b + 1024 + 96);
        put32(b + 1024 + 96, SELF_CRC_PLACEHOLDER);
        if (!memcmp(b + 1024, "*dvdisaster*RS03", 16) && get32(b + 1024 + 76) == (uint32_t)nd
            && get64(b + 1024 + 88) == spl && crc32_dv(b, SECTOR) == want) {
            ndata = nd;
            data_sectors = get64(b + 1024 + 64);
        }
    }
    if (!ndata) {
        snprintf(err, errlen, "%s: no RS03 CRC layer found (not augmented, or its first CRC sector is damaged)", path);
        goto done;
    }
    const char *why = NULL;
    if (rs03_layout_for(data_sectors, spl * FIELDMAX, 0, &r->lay, &why) || r->lay.ndata != ndata) {
        snprintf(err, errlen, "%s: the RS03 layout does not add up%s%s", path, why ? ": " : "", why ? why : "");
        goto done;
    }
    /* the header */
    if (pread_all(fd, b, 4096, data_sectors * SECTOR)) { io_error(err, errlen, "cannot read", path); goto done; }
    uint32_t want = get32(b + 96);
    put32(b + 96, SELF_CRC_PLACEHOLDER);
    r->header_ok = !memcmp(b, "*dvdisaster*RS03", 16) && crc32_dv(b, 4096) == want && get64(b + 68) == data_sectors
                   && get32(b + 76) == (uint32_t)ndata && get64(b + 120) == spl;
    unsigned char fp[16];
    memcpy(fp, b + 20, 16);
    uint32_t in_last = get32(b + 116);
    if (!r->header_ok) {                /* the fingerprint and inLast as the CRC layer has them */
        if (pread_all(fd, b, SECTOR, r->lay.first_crc * SECTOR)) { io_error(err, errlen, "cannot read", path); goto done; }
        memcpy(fp, b + 1024 + 32, 16);
        in_last = get32(b + 1024 + 72);
    }
    rc = encode_image(fd, path, &r->lay, fp, in_last, r, err, errlen);
done:
    close(fd);
    return rc;
}
