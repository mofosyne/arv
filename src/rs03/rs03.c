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
#define PRIMTH_ROOT 116                 /* the PRIM_ELEM-th root of 1: 11 * 116 = 1 mod 255 */
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

/* the EccHeader struct, 4096 bytes (dvdisaster.h) */
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

/* The parity of a run of codewords, shared out among the processors */
typedef struct {
    const unsigned char (*lut)[2 * FIELDMAX + 8];
    size_t stride;
    int ndata, nroots, shift_init, g0, threads;
} parity_coder;

/* the generator, and per feedback value its products, twice over (no wrap-around) */
static void parity_setup(parity_coder *pc, const rs03_layout *lay, int *gpoly, unsigned char (*lut)[2 * FIELDMAX + 8])
{
    int ndata = lay->ndata, nroots = lay->nroots;
    make_gpoly(nroots, gpoly);
    for (int f = 0; f < FIELDMAX; f++)
        for (int i = 0; i < nroots; i++)
            lut[f][i] = lut[f][nroots + i] = (unsigned char)alpha_to[modmax(f + gpoly[nroots - 1 - i])];
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    pc->lut = (const unsigned char (*)[2 * FIELDMAX + 8])lut;
    pc->stride = ((size_t)nroots + 7) & ~(size_t)7;
    pc->ndata = ndata;
    pc->nroots = nroots;
    pc->shift_init = nroots - ndata % nroots;
    if (pc->shift_init == nroots) pc->shift_init = 0;
    pc->g0 = gpoly[0];
    pc->threads = cpus < 1 ? 1 : cpus > MAX_THREADS ? MAX_THREADS : (int)cpus;
}

/* parity (nbytes * stride): byte k of codeword i at i * stride + k, i.e. parity layer k */
static void run_parity(const parity_coder *pc, const unsigned char *const *layers, size_t nbytes, unsigned char *parity)
{
    int threads = pc->threads;
    job jobs[MAX_THREADS];
    pthread_t tid[MAX_THREADS];
    int started[MAX_THREADS] = { 0 };
    memset(parity, 0, nbytes * pc->stride);
    for (int t = 0; t < threads; t++) {
        jobs[t] = (job){ layers, parity, pc->lut, nbytes * (size_t)t / (size_t)threads, nbytes * (size_t)(t + 1) / (size_t)threads,
                         pc->stride, pc->ndata, pc->nroots, pc->shift_init, pc->g0 };
        started[t] = t && !pthread_create(&tid[t], NULL, encode, &jobs[t]);
        if (t && !started[t]) encode(&jobs[t]);          /* no thread: do it here */
    }
    encode(&jobs[0]);
    for (int t = 1; t < threads; t++)
        if (started[t]) pthread_join(tid[t], NULL);
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
void (*rs03_progress)(uint64_t done, uint64_t total);

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
    parity_coder pc;
    parity_setup(&pc, lay, gpoly, lut);

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
        const unsigned char *layers[FIELDMAX];
        for (int l = 0; l < ndata - 1; l++) layers[l] = data + (size_t)l * (CHUNK + 1) * SECTOR;
        layers[ndata - 1] = check ? stored : crcs;     /* the parity protects the CRC layer as it is */
        run_parity(&pc, layers, nbytes, parity);
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
        if (rs03_progress) rs03_progress(c + m, spl);
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

/* a CRC sector whose cookie, layout fields (when nd and spl are given) and own CRC are right */
static int crc_block_ok(const unsigned char *b, int nd, uint64_t spl)
{
    unsigned char c[SECTOR];
    if (memcmp(b + 1024, "*dvdisaster*RS03", 16)) return 0;
    if (nd && (get32(b + 1024 + 76) != (uint32_t)nd || get64(b + 1024 + 88) != spl)) return 0;
    memcpy(c, b, SECTOR);
    put32(c + 1024 + 96, SELF_CRC_PLACEHOLDER);
    return crc32_dv(c, SECTOR) == get32(b + 1024 + 96);
}

/* an ecc header (4096 bytes) with its cookie and own CRC right, found at sector `at` */
static int header_ok(const unsigned char *h, uint64_t at)
{
    unsigned char c[4096];
    if (memcmp(h, "*dvdisaster*RS03", 16) || get64(h + 68) != at) return 0;
    memcpy(c, h, 4096);
    put32(c + 96, SELF_CRC_PLACEHOLDER);
    return crc32_dv(c, 4096) == get32(h + 96);
}

/* The layout of an augmented image of `size` bytes: from a CRC sector (any of the first few at
   layer ndata-1, for each ndata of 85..254) when the image is 255 whole layers; otherwise (cut
   short, or those sectors damaged) from the ecc header, looked for sector by sector. */
static int find_layout(int fd, uint64_t size, rs03_layout *lay)
{
    uint64_t sectors = size / SECTOR, data_sectors = 0, spl = 0;
    unsigned char b[4096];
    int ndata = 0;
    if (!(size % SECTOR) && sectors && !(sectors % FIELDMAX)) {
        uint64_t s = sectors / FIELDMAX;
        for (int nd = 85; nd < FIELDMAX && !ndata; nd++)
            for (uint64_t n = 0; n < 8 && n < s && !ndata; n++)
                if (!pread_all(fd, b, SECTOR, ((uint64_t)(nd - 1) * s + n) * SECTOR) && crc_block_ok(b, nd, s)) {
                    ndata = nd;
                    spl = s;
                    data_sectors = get64(b + 1024 + 64);
                }
    }
    if (!ndata) {
        enum { RUN = 512 };
        unsigned char *run = malloc((size_t)RUN * SECTOR);
        for (uint64_t at = 0; run && at < sectors && !ndata; at += RUN) {
            uint64_t m = sectors - at < RUN ? sectors - at : RUN;
            if (pread_all(fd, run, (size_t)m * SECTOR, at * SECTOR)) break;
            for (uint64_t n = 0; n < m && !ndata; n++)
                if (!memcmp(run + n * SECTOR, "*dvdisaster*RS03", 16) && !pread_all(fd, b, 4096, (at + n) * SECTOR)
                    && header_ok(b, at + n)) {
                    ndata = (int)get32(b + 76);
                    spl = get64(b + 120);
                    data_sectors = at + n;
                }
        }
        free(run);
    }
    if (!ndata || !spl) return -1;
    const char *why;
    if (rs03_layout_for(data_sectors, spl * FIELDMAX, 0, lay, &why) || lay->ndata != ndata) return -1;
    return 0;
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
    if (find_layout(fd, (uint64_t)st.st_size, &r->lay) || r->lay.total_sectors != sectors) {
        snprintf(err, errlen, "%s: no RS03 layout found (not augmented, or its CRC sectors and header are damaged)", path);
        goto done;
    }
    uint64_t data_sectors = r->lay.data_sectors;
    /* the header */
    if (pread_all(fd, b, 4096, data_sectors * SECTOR)) { io_error(err, errlen, "cannot read", path); goto done; }
    r->header_ok = header_ok(b, data_sectors) && get32(b + 76) == (uint32_t)r->lay.ndata
                   && get64(b + 120) == r->lay.sectors_per_layer;
    unsigned char fp[16];
    memcpy(fp, b + 20, 16);
    uint32_t in_last = get32(b + 116);
    if (!r->header_ok) {                /* the fingerprint and inLast as a CRC sector has them */
        for (uint64_t n = 0; n < r->lay.sectors_per_layer; n++)
            if (!pread_all(fd, b, SECTOR, (r->lay.first_crc + n) * SECTOR) && crc_block_ok(b, r->lay.ndata, r->lay.sectors_per_layer))
                break;
        memcpy(fp, b + 1024 + 32, 16);
        in_last = get32(b + 1024 + 72);
    }
    rc = encode_image(fd, path, &r->lay, fp, in_last, r, err, errlen);
done:
    close(fd);
    return rc;
}

/* ------------------------------------------------------------------ repair */

/* dvdisaster's dead sector marker (ds-marker.c), which its reader writes for a sector it could not read */
static int dead_sector(const unsigned char *p)
{
    static const char head[] = "dvdisaster dead sector marker\n", end[] = "dvdisaster dead sector end marker\n";
    return !memcmp(p, head, sizeof head - 1) && !memcmp(p + 2046 - (sizeof end - 1), end, sizeof end - 1);
}

static int all_zero(const unsigned char *p)
{
    for (int i = 0; i < SECTOR; i++)
        if (p[i]) return 0;
    return 1;
}

/* One codeword's errors and erasures, corrected in place (dvdisaster's RS03Fix, after Phil Karn's
   decode_rs): sym[j] is the symbol of layer j; erasures are layer numbers. Returns the layers
   changed (count, into changed[]), or -1 when the codeword cannot be corrected. */
static int decode(unsigned char *const *sym, size_t at, int nroots, const unsigned char (*mul)[256], const int *erasures,
                  int nerasures, int *changed)
{
    int lambda[FIELDMAX + 1], syn[FIELDMAX], b[FIELDMAX + 1], t[FIELDMAX + 1], omega[FIELDMAX + 1];
    int root[FIELDMAX], reg[FIELDMAX + 1], loc[FIELDMAX];
    int syn_error = 0, count = 0, deg_lambda = 0, el, r;
    unsigned char sy[FIELDMAX];
    /* the syndromes: the codeword at each root of the generator, by Horner's rule (mul[i]: times root i) */
    for (int i = 0; i < nroots; i++) sy[i] = sym[0][at];
    for (int j = 1; j < FIELDMAX; j++) {
        unsigned char d = sym[j][at];
        for (int i = 0; i < nroots; i++) sy[i] = mul[i][sy[i]] ^ d;
    }
    for (int i = 0; i < nroots; i++) syn[i] = sy[i];
    for (int i = 0; i < nroots; i++) {
        syn_error |= syn[i];
        syn[i] = index_of[syn[i]];
    }
    if (!syn_error) return 0;
    memset(lambda, 0, sizeof lambda);
    lambda[0] = 1;
    if (nerasures > 0) {
        lambda[1] = alpha_to[modmax(PRIM_ELEM * (FIELDMAX - 1 - erasures[0]) % FIELDMAX)];
        for (int i = 1; i < nerasures; i++) {
            int u = modmax(PRIM_ELEM * (FIELDMAX - 1 - erasures[i]) % FIELDMAX);
            for (int j = i + 1; j > 0; j--) {
                int tmp = index_of[lambda[j - 1]];
                if (tmp != A0) lambda[j] ^= alpha_to[modmax(u + tmp)];
            }
        }
    }
    for (int i = 0; i <= nroots; i++) b[i] = index_of[lambda[i]];
    /* Berlekamp-Massey: the error and erasure locator */
    r = el = nerasures;
    while (++r <= nroots) {
        int discr = 0;
        for (int i = 0; i < r; i++)
            if (lambda[i] && syn[r - i - 1] != A0) discr ^= alpha_to[modmax(index_of[lambda[i]] + syn[r - i - 1])];
        discr = index_of[discr];
        if (discr == A0) {
            memmove(b + 1, b, (size_t)nroots * sizeof b[0]);
            b[0] = A0;
        } else {
            t[0] = lambda[0];
            for (int i = 0; i < nroots; i++)
                t[i + 1] = b[i] != A0 ? lambda[i + 1] ^ alpha_to[modmax(discr + b[i])] : lambda[i + 1];
            if (2 * el <= r + nerasures - 1) {
                el = r + nerasures - el;
                for (int i = 0; i <= nroots; i++) b[i] = lambda[i] ? modmax(index_of[lambda[i]] - discr + FIELDMAX) : A0;
            } else {
                memmove(b + 1, b, (size_t)nroots * sizeof b[0]);
                b[0] = A0;
            }
            memcpy(lambda, t, (size_t)(nroots + 1) * sizeof t[0]);
        }
    }
    for (int i = 0; i <= nroots; i++) {
        lambda[i] = index_of[lambda[i]];
        if (lambda[i] != A0) deg_lambda = i;
    }
    /* Chien search: the roots of the locator are the error locations */
    memcpy(reg + 1, lambda + 1, (size_t)nroots * sizeof reg[0]);
    for (int i = 1, k = PRIMTH_ROOT - 1; i <= FIELDMAX; i++, k = modmax(k + PRIMTH_ROOT)) {
        int q = 1;
        for (int j = deg_lambda; j > 0; j--)
            if (reg[j] != A0) {
                reg[j] = modmax(reg[j] + j);
                q ^= alpha_to[reg[j]];
            }
        if (q) continue;
        root[count] = i;
        loc[count] = k;
        if (++count == deg_lambda) break;
    }
    if (deg_lambda != count) return -1;
    /* Forney: the error values, from omega(x) = syn(x) * lambda(x) mod x^nroots */
    int deg_omega = deg_lambda - 1, nchanged = 0;
    for (int i = 0; i <= deg_omega; i++) {
        int tmp = 0;
        for (int j = i; j >= 0; j--)
            if (syn[i - j] != A0 && lambda[j] != A0) tmp ^= alpha_to[modmax(syn[i - j] + lambda[j])];
        omega[i] = index_of[tmp];
    }
    for (int j = count - 1; j >= 0; j--) {
        int num1 = 0, den = 0;
        for (int i = deg_omega; i >= 0; i--)
            if (omega[i] != A0) num1 ^= alpha_to[modmax(omega[i] + i * root[j] % FIELDMAX)];
        int num2 = alpha_to[modmax((root[j] * (FIRST_ROOT - 1) + FIELDMAX) % FIELDMAX)];
        for (int i = (deg_lambda < nroots - 1 ? deg_lambda : nroots - 1) & ~1; i >= 0; i -= 2)
            if (lambda[i + 1] != A0) den ^= alpha_to[modmax(lambda[i + 1] + i * root[j] % FIELDMAX)];
        if (num1) {
            if (!den || loc[j] >= FIELDMAX) return -1;
            sym[loc[j]][at] ^= (unsigned char)alpha_to[modmax(index_of[num1] + index_of[num2] + FIELDMAX - index_of[den])];
            changed[nchanged++] = loc[j];
        }
    }
    return nchanged;
}

/* One thread's share of a damaged position: bytes from..to of its 255 sectors */
typedef struct {
    unsigned char *const *sym;
    const unsigned char (*mul)[256];
    const int *erasures;
    int nerasures, nroots, fail;
    size_t from, to;
    unsigned char was[FIELDMAX];        /* the layers it changed */
} fix_job;

static void *fix(void *arg)
{
    fix_job *fj = arg;
    for (size_t i = fj->from; i < fj->to && !fj->fail; i++) {
        int changed[FIELDMAX], k = decode(fj->sym, i, fj->nroots, fj->mul, fj->erasures, fj->nerasures, changed);
        if (k < 0) fj->fail = 1;
        for (int j = 0; j < k; j++) fj->was[changed[j]] = 1;
    }
    return NULL;
}

int rs03_repair(const char *path, rs03_repair_report *r, char *err, size_t errlen)
{
    setup();
    memset(r, 0, sizeof *r);
    int fd = open(path, O_RDWR);
    if (fd < 0) return io_error(err, errlen, "cannot open", path);
    struct stat st;
    int rc = -1;
    int *gpoly = NULL;
    unsigned char (*lut)[2 * FIELDMAX + 8] = NULL;
    unsigned char *buf = NULL, *parity = NULL, *last_crc = NULL, *prev_crc = NULL, (*mul)[256] = NULL;
    if (fstat(fd, &st)) { io_error(err, errlen, "cannot read", path); goto done; }
    uint64_t size = (uint64_t)st.st_size, have = size / SECTOR;    /* a part-sector at the end counts as missing */
    rs03_layout *lay = &r->lay;
    if (find_layout(fd, size, lay)) {
        snprintf(err, errlen, "%s: no RS03 layout found (not augmented, or its CRC sectors and header are damaged)", path);
        rc = -2;
        goto done;
    }
    uint64_t spl = lay->sectors_per_layer, total = lay->total_sectors;
    int ndata = lay->ndata, nroots = lay->nroots;
    if (have > total) {
        snprintf(err, errlen, "%s is %llu sectors longer than its RS03 layout (%llu sectors); cut it to %llu bytes first",
                 path, (unsigned long long)(have - total), (unsigned long long)total, (unsigned long long)total * SECTOR);
        goto done;
    }
    if (size < total * SECTOR && ftruncate(fd, (off_t)(total * SECTOR))) { io_error(err, errlen, "cannot extend", path); goto done; }
    r->missing = total - have;
    /* every layer's sectors at CHUNK positions; the parity the data and CRC layers give, to find the
       positions that need decoding at all */
    parity_coder pc;
    gpoly = malloc((size_t)(nroots + 1) * sizeof *gpoly);
    lut = calloc(FIELDMAX, sizeof *lut);
    buf = malloc((size_t)FIELDMAX * CHUNK * SECTOR);
    parity = malloc((size_t)CHUNK * SECTOR * (((size_t)nroots + 7) & ~(size_t)7));
    last_crc = malloc(SECTOR);
    prev_crc = malloc(SECTOR);
    mul = malloc((size_t)nroots * sizeof *mul);
    if (!gpoly || !lut || !buf || !parity || !last_crc || !prev_crc || !mul) {
        errno = ENOMEM;
        io_error(err, errlen, "out of memory for", path);
        goto done;
    }
    parity_setup(&pc, lay, gpoly, lut);
    for (int i = 0; i < nroots; i++)          /* times the generator's root i, alpha^((FIRST_ROOT + i) * PRIM_ELEM) */
        for (int x = 0; x < 256; x++)
            mul[i][x] = x ? (unsigned char)alpha_to[modmax(index_of[x] + (FIRST_ROOT + i) * PRIM_ELEM % FIELDMAX)] : 0;
    /* position 0's CRCs are in the last CRC sector */
    uint64_t last_at = lay->first_crc + spl - 1;
    if (pread_all(fd, last_crc, SECTOR, last_at * SECTOR)) { io_error(err, errlen, "cannot read", path); goto done; }
    int last_ok = last_at < have && crc_block_ok(last_crc, ndata, spl);
    for (uint64_t c = 0; c < spl; c += CHUNK) {
        uint64_t m = spl - c < CHUNK ? spl - c : CHUNK;
        size_t nbytes = (size_t)m * SECTOR;
        unsigned char *layer[FIELDMAX];
        for (int l = 0; l < FIELDMAX; l++) {
            layer[l] = buf + (size_t)l * CHUNK * SECTOR;
            if (pread_all(fd, layer[l], nbytes, ((uint64_t)l * spl + c) * SECTOR)) { io_error(err, errlen, "cannot read", path); goto done; }
        }
        run_parity(&pc, (const unsigned char *const *)layer, nbytes, parity);
        for (uint64_t n = 0; n < m; n++) {
            uint64_t s = c + n;
            /* the CRCs of this position: the CRC sector before it (repaired already), or the last one */
            const unsigned char *crcs = !s ? last_crc : n ? layer[ndata - 1] + (n - 1) * SECTOR : prev_crc;
            int crcs_ok = !s ? last_ok : crc_block_ok(crcs, ndata, spl);
            int erasures[FIELDMAX], nerasures = 0, need = !crcs_ok;
            unsigned char *sym[FIELDMAX];
            for (int l = 0; l < FIELDMAX; l++) {
                unsigned char *p = sym[l] = layer[l] + n * SECTOR;
                uint64_t at = (uint64_t)l * spl + s;
                int gone = at >= have || dead_sector(p);
                if (!gone && l < ndata - 1) gone = crcs_ok && crc32_dv(p, SECTOR) != get32(crcs + 4 * l);
                else if (!gone && l == ndata - 1) gone = !crc_block_ok(p, ndata, spl);
                else if (!gone) gone = all_zero(p);     /* a sector a reader filled with zeros */
                if (gone) erasures[nerasures++] = l;
            }
            need |= nerasures > 0;
            for (size_t i = 0; i < SECTOR && !need; i++)
                for (int k = 0; k < nroots && !need; k++)
                    need = parity[(n * SECTOR + i) * pc.stride + (size_t)k] != sym[ndata + k][i];
            if (!need) continue;
            if (nerasures > nroots) {
                r->unrepaired_positions++;
                r->unrepaired_sectors += (uint64_t)nerasures;
                continue;
            }
            unsigned char was[FIELDMAX];        /* the layers this position changed */
            int fail = 0, threads = pc.threads;
            fix_job jobs[MAX_THREADS];
            pthread_t tid[MAX_THREADS];
            int started[MAX_THREADS] = { 0 };
            for (int t = 0; t < threads; t++) {
                jobs[t] = (fix_job){ sym, (const unsigned char (*)[256])mul, erasures, nerasures, nroots, 0,
                                     (size_t)SECTOR * (size_t)t / (size_t)threads, (size_t)SECTOR * (size_t)(t + 1) / (size_t)threads, { 0 } };
                started[t] = t && !pthread_create(&tid[t], NULL, fix, &jobs[t]);
                if (t && !started[t]) fix(&jobs[t]);
            }
            fix(&jobs[0]);
            memset(was, 0, sizeof was);
            for (int t = 0; t < threads; t++) {
                if (t && started[t]) pthread_join(tid[t], NULL);
                fail |= jobs[t].fail;
                for (int l = 0; l < FIELDMAX; l++) was[l] |= jobs[t].was[l];
            }
            if (fail) {                         /* leave it as it was (the copy in memory is not written) */
                for (int l = 0; l < FIELDMAX; l++)
                    if (pread_all(fd, sym[l], SECTOR, ((uint64_t)l * spl + s) * SECTOR)) { io_error(err, errlen, "cannot read", path); goto done; }
                r->unrepaired_positions++;
                r->unrepaired_sectors += (uint64_t)(nerasures ? nerasures : 1);
                continue;
            }
            for (int l = 0; l < FIELDMAX; l++) {
                if (!was[l]) continue;
                if (pwrite_all(fd, sym[l], SECTOR, ((uint64_t)l * spl + s) * SECTOR)) { io_error(err, errlen, "cannot write", path); goto done; }
                if (l < ndata - 1) r->repaired_data++;
                else if (l == ndata - 1) r->repaired_crc++;
                else r->repaired_ecc++;
            }
        }
        memcpy(prev_crc, layer[ndata - 1] + (m - 1) * SECTOR, SECTOR);
    }
    rc = 0;
done:
    if (close(fd) && !rc) rc = io_error(err, errlen, "cannot write", path);
    free(gpoly); free(lut); free(buf); free(parity); free(last_crc); free(prev_crc); free(mul);
    return rc;
}
