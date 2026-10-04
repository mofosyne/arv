/*
 * udfwrite: closed, read-only UDF 2.50 images for archives. See udfwrite.h and
 * docs/spec/archival-udf.md. References: ECMA-167 3rd edition, OSTA UDF 2.50.
 *
 * Image layout (sectors of 2048 bytes; P = partition start, blocks are partition blocks):
 *
 *   16-18    volume recognition sequence (BEA01, NSR03, TEA01)
 *   256      anchor
 *   288      main volume descriptor sequence (16 sectors)
 *   320      logical volume integrity sequence (64 sectors)
 *   P=384    partition: block 0 metadata file entry, blocks 32.. the metadata (file set,
 *            directories, file entries), then file data, then the metadata mirror (a copy) and
 *            the mirror file entry in the last block
 *   after    reserve volume descriptor sequence (16 sectors), anchors at N-256 and N
 */
#include "udfwrite.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SECTOR UDFW_SECTOR
#define VRS_SECTOR 16
#define ANCHOR_SECTOR 256
#define MVDS_SECTOR 288
#define VDS_SECTORS 16
#define LVIS_SECTOR 320
#define LVIS_SECTORS 64
#define PART_SECTOR 384
#define ALIGN 32                       /* metadata allocation and alignment unit, in blocks */
#define MAX_EXTENT 0x3FFFF800u         /* longest extent: 2^30 bytes less one block */
#define EFE_BASE 216                   /* size of an Extended File Entry before its EAs and ADs */
#define FID_BASE 38
#define UDF_REV 0x0250

enum { TAG_PVD = 1, TAG_AVDP = 2, TAG_IUVD = 4, TAG_PD = 5, TAG_LVD = 6, TAG_USD = 7, TAG_TD = 8,
       TAG_LVID = 9, TAG_FSD = 256, TAG_FID = 257, TAG_EFE = 266 };
enum { FT_DIR = 4, FT_FILE = 5, FT_METADATA = 250, FT_MIRROR = 251 };

typedef struct node node;
struct node {
    char *name;                 /* this component, UTF-8; "" for the root */
    char *path;                 /* full path, for reporting */
    node *parent;
    node **kids;
    size_t nkids, capkids;
    int is_dir;
    int executable;             /* the source had an execute bit */
    uint64_t size;              /* file bytes, or directory FID bytes */
    int64_t mtime;
    udfw_read_fn read;
    void *file_ctx;
    uint8_t ident[256];         /* encoded name (OSTA CS0, compression id first) */
    int ident_len;
    uint64_t unique_id;
    uint32_t efe_lbn;           /* metadata partition block of its entry */
    uint32_t data_lbn;          /* directories: metadata block of the FIDs; files: physical block */
    uint32_t subdirs;
};

struct udfw {
    udfw_options opt;
    char *volume_id, *label, *volume_set;
    node *root;
    node **order;               /* depth-first, by path */
    size_t norder, nfiles, ndirs;
    int laid_out;
    uint32_t meta_blocks;       /* M: metadata extent length, a multiple of ALIGN */
    uint32_t data_start, data_end, mirror_start, mirror_fe, part_len;
    uint64_t rvds_sector, last_sector;
    uint8_t *meta;              /* the metadata extent, built in memory */
    char err[256];
};

/* ------------------------------------------------------------------ errors and helpers */

static int fail(udfw *w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(w->err, sizeof w->err, fmt, ap);
    va_end(ap);
    return -1;
}

const char *udfw_error(const udfw *w) { return w->err; }

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static uint32_t blocks_for(uint64_t bytes) { return (uint32_t)((bytes + SECTOR - 1) / SECTOR); }
static uint32_t round_up(uint32_t v, uint32_t unit) { return (v + unit - 1) / unit * unit; }

/* CRC-ITU-T (polynomial 0x1021, initial value 0), as ECMA-167 7.2.6 asks. */
static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* Fills in a descriptor tag (ECMA-167 3/7.2) once the descriptor body is complete. */
static void tag(uint8_t *d, uint16_t id, uint32_t location, size_t total_len)
{
    put16(d, id);
    put16(d + 2, 3);                    /* descriptor version 3: NSR03 */
    d[5] = 0;
    put16(d + 6, 1);                    /* tag serial number */
    put16(d + 8, crc16(d + 16, total_len - 16));
    put16(d + 10, (uint32_t)(total_len - 16));
    put32(d + 12, location);
    uint8_t sum = 0;
    for (int i = 0; i < 16; i++)
        if (i != 4) sum += d[i];
    d[4] = sum;
}

/* regid (ECMA-167 1/7.4): flags, identifier, suffix. */
static void regid(uint8_t *p, const char *id, int udf_suffix)
{
    memset(p, 0, 32);
    memcpy(p + 1, id, strlen(id));
    if (udf_suffix) put16(p + 24, UDF_REV);   /* UDF revision; OS class and identifier 0 */
}

static void charspec(uint8_t *p)
{
    memset(p, 0, 64);
    memcpy(p + 1, "OSTA Compressed Unicode", 23);   /* type 0: CS0 */
}

static void timestamp(uint8_t *p, int64_t t)
{
    time_t tt = (time_t)t;
    struct tm tm;
    memset(p, 0, 12);
    if (!gmtime_r(&tt, &tm)) return;
    put16(p, 1 << 12);                  /* type 1 (local time), offset 0: UTC */
    put16(p + 2, (uint32_t)(tm.tm_year + 1900));
    p[4] = tm.tm_mon + 1; p[5] = tm.tm_mday; p[6] = tm.tm_hour; p[7] = tm.tm_min; p[8] = tm.tm_sec;
}

static void long_ad(uint8_t *p, uint32_t len, uint32_t lbn, uint16_t part, uint32_t unique)
{
    memset(p, 0, 16);
    put32(p, len);
    put32(p + 4, lbn);
    put16(p + 8, part);
    put32(p + 12, unique);              /* ADImpUse: flags (2 bytes, 0), then the UDF unique id */
}

/* ------------------------------------------------------------------ names: OSTA CS0 */

/* Encodes UTF-8 as OSTA compressed Unicode: compression id 8 when every character is at most
 * U+00FF, otherwise 16 (UTF-16 big-endian). Returns the encoded length, or -1 if the text is
 * not valid UTF-8 or the result is longer than `max`. */
static int cs0(const char *s, uint8_t *out, int max)
{
    uint32_t cps[512];
    int n = 0, wide = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        uint32_t c;
        int more;
        if (*p < 0x80) { c = *p; more = 0; }
        else if ((*p & 0xE0) == 0xC0) { c = *p & 0x1F; more = 1; }
        else if ((*p & 0xF0) == 0xE0) { c = *p & 0x0F; more = 2; }
        else if ((*p & 0xF8) == 0xF0) { c = *p & 0x07; more = 3; }
        else return -1;
        p++;
        while (more--) {
            if ((*p & 0xC0) != 0x80) return -1;
            c = (c << 6) | (*p++ & 0x3F);
        }
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF) || n >= 512) return -1;
        if (c > 0xFF) wide = 1;
        cps[n++] = c;
    }
    int len = 1;
    out[0] = wide ? 16 : 8;
    for (int i = 0; i < n; i++) {
        uint32_t c = cps[i];
        if (!wide) {
            if (len + 1 > max) return -1;
            out[len++] = (uint8_t)c;
        } else if (c < 0x10000) {
            if (len + 2 > max) return -1;
            out[len++] = c >> 8; out[len++] = c & 0xFF;
        } else {
            if (len + 4 > max) return -1;
            c -= 0x10000;
            uint32_t hi = 0xD800 + (c >> 10), lo = 0xDC00 + (c & 0x3FF);
            out[len++] = hi >> 8; out[len++] = hi & 0xFF;
            out[len++] = lo >> 8; out[len++] = lo & 0xFF;
        }
    }
    return len;
}

/* A dstring of `size` bytes: the encoded text, then its length in the last byte. */
static int dstring(uint8_t *p, size_t size, const char *s)
{
    memset(p, 0, size);
    if (!s || !*s) return 0;
    int n = cs0(s, p, (int)size - 1);
    if (n < 0) return -1;
    p[size - 1] = (uint8_t)n;
    return 0;
}

/* ------------------------------------------------------------------ the tree */

static node *new_node(node *parent, const char *name, size_t len, int is_dir)
{
    node *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->name = malloc(len + 1);
    size_t plen = parent && parent->path[0] ? strlen(parent->path) + 1 : 0;
    n->path = malloc(plen + len + 1);
    if (!n->name || !n->path) { free(n->name); free(n->path); free(n); return NULL; }
    memcpy(n->name, name, len);
    n->name[len] = 0;
    if (plen) { memcpy(n->path, parent->path, plen - 1); n->path[plen - 1] = '/'; }
    memcpy(n->path + plen, name, len);
    n->path[plen + len] = 0;
    n->parent = parent;
    n->is_dir = is_dir;
    return n;
}

static void free_node(node *n)
{
    if (!n) return;
    for (size_t i = 0; i < n->nkids; i++) free_node(n->kids[i]);
    free(n->kids); free(n->name); free(n->path); free(n);
}

static node *find_kid(node *dir, const char *name, size_t len)
{
    for (size_t i = 0; i < dir->nkids; i++)
        if (strlen(dir->kids[i]->name) == len && !memcmp(dir->kids[i]->name, name, len))
            return dir->kids[i];
    return NULL;
}

static int add_kid(udfw *w, node *dir, node *kid)
{
    if (dir->nkids == dir->capkids) {
        size_t cap = dir->capkids ? dir->capkids * 2 : 8;
        node **k = realloc(dir->kids, cap * sizeof *k);
        if (!k) return fail(w, "out of memory");
        dir->kids = k;
        dir->capkids = cap;
    }
    dir->kids[dir->nkids++] = kid;
    return 0;
}

/* Walks `path`, creating missing folders; returns the parent folder of the last component and
 * that component's name in *leaf / *leaf_len. */
static node *walk(udfw *w, const char *path, const char **leaf, size_t *leaf_len)
{
    node *dir = w->root;
    const char *p = path;
    if (w->laid_out) { fail(w, "the image is already laid out"); return NULL; }
    for (;;) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0 || (len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.')) {
            fail(w, "%s: empty, \".\" or \"..\" path component", path);
            return NULL;
        }
        if (!slash) { *leaf = p; *leaf_len = len; return dir; }
        node *next = find_kid(dir, p, len);
        if (!next) {
            next = new_node(dir, p, len, 1);
            if (!next) { fail(w, "out of memory"); return NULL; }
            next->ident_len = cs0(next->name, next->ident, 255);    /* a folder made as a parent needs its name too */
            if (next->ident_len < 0) {
                free_node(next);
                fail(w, "%s: name is not valid UTF-8 or longer than UDF allows (255 bytes)", path);
                return NULL;
            }
            if (add_kid(w, dir, next)) { free_node(next); fail(w, "out of memory"); return NULL; }
            next->mtime = w->opt.time;
        } else if (!next->is_dir) {
            fail(w, "%s: %s is a file, not a folder", path, next->path);
            return NULL;
        }
        dir = next;
        p = slash + 1;
    }
}

static int add(udfw *w, const char *path, int is_dir, uint64_t size, int64_t mtime, unsigned mode,
               udfw_read_fn read, void *file_ctx)
{
    const char *leaf;
    size_t len;
    node *dir = walk(w, path, &leaf, &len);
    if (!dir) return -1;
    node *n = find_kid(dir, leaf, len);
    if (n) {
        if (n->is_dir && is_dir) { n->mtime = mtime; return 0; }  /* made earlier as a parent */
        return fail(w, "%s: added twice", path);
    }
    n = new_node(dir, leaf, len, is_dir);
    if (!n) return fail(w, "out of memory");
    n->ident_len = cs0(n->name, n->ident, 255);
    if (n->ident_len < 0) {
        free_node(n);
        return fail(w, "%s: name is not valid UTF-8 or longer than UDF allows (255 bytes)", path);
    }
    n->size = size; n->mtime = mtime; n->read = read; n->file_ctx = file_ctx;
    n->executable = (mode & 0111) != 0;
    if (add_kid(w, dir, n)) { free_node(n); return -1; }
    return 0;
}

int udfw_add_dir(udfw *w, const char *path, int64_t mtime)
{
    return add(w, path, 1, 0, mtime, 0755, NULL, NULL);
}

int udfw_add_file(udfw *w, const char *path, uint64_t size, int64_t mtime, unsigned mode,
                  udfw_read_fn read, void *file_ctx)
{
    if (!read && size) return fail(w, "%s: no read function", path);
    return add(w, path, 0, size, mtime, mode, read, file_ctx);
}

udfw *udfw_new(const udfw_options *opt)
{
    udfw *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->opt = *opt;
    w->volume_id = dupstr(opt->volume_id ? opt->volume_id : "");
    w->label = dupstr(opt->label ? opt->label : w->volume_id ? w->volume_id : "");
    w->volume_set = dupstr(opt->volume_set ? opt->volume_set : "");
    w->root = new_node(NULL, "", 0, 1);
    if (!w->volume_id || !w->label || !w->volume_set || !w->root) { udfw_free(w); return NULL; }
    w->root->mtime = opt->time;
    return w;
}

void udfw_free(udfw *w)
{
    if (!w) return;
    free_node(w->root);
    free(w->order); free(w->meta);
    free(w->volume_id); free(w->label); free(w->volume_set);
    free(w);
}

/* ------------------------------------------------------------------ layout */

static int by_name(const void *a, const void *b)
{
    return strcmp((*(node *const *)a)->name, (*(node *const *)b)->name);   /* UTF-8 bytes */
}

static uint32_t fid_len(int ident_len) { return (FID_BASE + ident_len + 3) & ~3u; }

static int collect(udfw *w, node *n)
{
    w->order[w->norder++] = n;
    if (!n->is_dir) { w->nfiles++; return 0; }
    w->ndirs++;
    qsort(n->kids, n->nkids, sizeof *n->kids, by_name);
    n->size = fid_len(0);                       /* the parent entry */
    for (size_t i = 0; i < n->nkids; i++) {
        n->size += fid_len(n->kids[i]->ident_len);
        if (n->kids[i]->is_dir) n->subdirs++;
        if (collect(w, n->kids[i])) return -1;
    }
    return 0;
}

static size_t count(node *n)
{
    size_t c = 1;
    for (size_t i = 0; i < n->nkids; i++) c += count(n->kids[i]);
    return c;
}

static int layout(udfw *w)
{
    if (w->laid_out) return 0;
    w->order = malloc(count(w->root) * sizeof *w->order);
    if (!w->order) return fail(w, "out of memory");
    if (collect(w, w->root)) return -1;

    /* the metadata partition: file set descriptor, terminator, then each entry (and the FIDs
     * of each folder) in path order */
    uint32_t next = 2;
    for (size_t i = 0; i < w->norder; i++) {
        node *n = w->order[i];
        n->unique_id = i == 0 ? 0 : 16 + i;
        n->efe_lbn = next++;
        if (n->is_dir) { n->data_lbn = next; next += blocks_for(n->size); }
    }
    w->meta_blocks = round_up(next, ALIGN);

    /* the physical partition: metadata file entry, metadata, file data, mirror, mirror entry */
    uint32_t cursor = w->data_start = ALIGN + w->meta_blocks;
    for (size_t i = 0; i < w->norder; i++) {
        node *n = w->order[i];
        if (n->is_dir) continue;
        if ((uint64_t)cursor + blocks_for(n->size) > 0xFFFFFFF0u)
            return fail(w, "the image is too large for one UDF partition");
        n->data_lbn = cursor;
        cursor += blocks_for(n->size);
        if ((n->size + MAX_EXTENT - 1) / MAX_EXTENT > (SECTOR - EFE_BASE) / 16)
            return fail(w, "%s: too large for one file entry", n->path);
    }
    w->data_end = cursor;
    w->mirror_start = round_up(cursor, ALIGN);
    w->mirror_fe = w->mirror_start + w->meta_blocks;
    w->part_len = w->mirror_fe + 1;
    w->rvds_sector = PART_SECTOR + (uint64_t)w->part_len;
    w->last_sector = w->rvds_sector + VDS_SECTORS + 256;   /* anchor N; N-256 follows the RVDS */
    w->laid_out = 1;
    return 0;
}

uint64_t udfw_sectors(udfw *w)
{
    return layout(w) ? 0 : w->last_sector + 1;
}

/* ------------------------------------------------------------------ descriptors */

static void efe(udfw *w, uint8_t *d, const node *n, int type, uint32_t location)
{
    uint64_t size = n ? n->size : (uint64_t)w->meta_blocks * SECTOR;
    int64_t t = n ? n->mtime : w->opt.time;
    /* UDF permissions (ECMA-167 4/14.9.5): read for owner, group and other; execute (search) too
     * for folders and for files that were executable. Other's bits matter most: on a mounted
     * disc the owner is unknown, so readers are "other". */
    uint32_t perm = (n && (n->is_dir || n->executable)) ? 0x14A5 : 0x1084;
    uint8_t *ad = d + EFE_BASE;
    uint32_t lad = 0;
    uint16_t ad_type;

    memset(d, 0, SECTOR);
    /* ICB tag (ECMA-167 4/14.6): strategy 4, one entry */
    put16(d + 20, 4);
    put16(d + 24, 1);
    d[27] = (uint8_t)type;
    if (type == FT_FILE) {
        ad_type = 1;                                   /* long_ad: the data is in partition 0 */
        for (uint64_t off = 0; off < size; off += MAX_EXTENT, lad += 16) {
            uint64_t len = size - off < MAX_EXTENT ? size - off : MAX_EXTENT;
            long_ad(ad + lad, (uint32_t)len, n->data_lbn + (uint32_t)(off / SECTOR), 0, 0);
        }
    } else {
        ad_type = 0;                                   /* short_ad, same partition as the entry */
        uint32_t pos = type == FT_DIR ? n->data_lbn : type == FT_METADATA ? ALIGN : w->mirror_start;
        put32(ad, (uint32_t)size);
        put32(ad + 4, pos);
        lad = 8;
    }
    put16(d + 34, ad_type);
    put32(d + 36, 0xFFFFFFFFu);                        /* uid: unknown */
    put32(d + 40, 0xFFFFFFFFu);                        /* gid */
    put32(d + 44, perm);
    put16(d + 48, type == FT_DIR ? 1 + n->subdirs : type == FT_FILE ? 1 : 0);
    put64(d + 56, size);                               /* information length */
    put64(d + 64, size);                               /* object size */
    put64(d + 72, blocks_for(size));                   /* logical blocks recorded */
    timestamp(d + 80, t);                              /* access */
    timestamp(d + 92, t);                              /* modification */
    timestamp(d + 104, t);                             /* creation */
    timestamp(d + 116, t);                             /* attribute */
    put32(d + 128, 1);                                 /* checkpoint */
    regid(d + 168, "*arv udfwrite", 0);
    put64(d + 200, n ? n->unique_id : 0);
    put32(d + 212, lad);                               /* length of allocation descriptors */
    tag(d, TAG_EFE, location, EFE_BASE + lad);
}

/* The FIDs of folder `n`: its parent, then each entry in name order. */
static void fids(udfw *w, uint8_t *meta, const node *n)
{
    uint64_t off = (uint64_t)n->data_lbn * SECTOR;
    for (size_t i = 0; i <= n->nkids; i++) {
        const node *to = i == 0 ? (n->parent ? n->parent : n) : n->kids[i - 1];
        int il = i == 0 ? 0 : to->ident_len;
        uint32_t len = fid_len(il);
        uint8_t *f = meta + off;
        memset(f, 0, len);
        put16(f + 16, 1);                              /* file version number */
        f[18] = (uint8_t)((i == 0 ? 0x08 : 0) | (to->is_dir ? 0x02 : 0));  /* parent, directory */
        f[19] = (uint8_t)il;
        long_ad(f + 20, SECTOR, to->efe_lbn, 1, (uint32_t)to->unique_id);
        if (il) memcpy(f + FID_BASE, to->ident, (size_t)il);
        tag(f, TAG_FID, (uint32_t)(off / SECTOR), len);   /* the CRC covers the padding too */
        off += len;
    }
    (void)w;
}

static int build_metadata(udfw *w)
{
    size_t bytes = (size_t)w->meta_blocks * SECTOR;
    uint8_t *m = w->meta = calloc(1, bytes);
    if (!m) return fail(w, "out of memory for %zu bytes of metadata", bytes);

    /* file set descriptor (ECMA-167 4/14.1) */
    timestamp(m + 16, w->opt.time);
    put16(m + 28, 3); put16(m + 30, 3);
    put32(m + 32, 1); put32(m + 36, 1);
    charspec(m + 48);
    if (dstring(m + 112, 128, w->label)) return fail(w, "label too long or not UTF-8");
    charspec(m + 240);
    dstring(m + 304, 32, w->volume_id);
    long_ad(m + 400, SECTOR, w->root->efe_lbn, 1, 0);
    regid(m + 416, "*OSTA UDF Compliant", 1);
    tag(m, TAG_FSD, 0, 512);
    tag(m + SECTOR, TAG_TD, 1, 512);                   /* terminating descriptor */

    for (size_t i = 0; i < w->norder; i++) {
        node *n = w->order[i];
        efe(w, m + (size_t)n->efe_lbn * SECTOR, n, n->is_dir ? FT_DIR : FT_FILE, n->efe_lbn);
        if (n->is_dir) fids(w, m, n);
    }
    return 0;
}

static void anchor(udfw *w, uint8_t *d, uint32_t location)
{
    memset(d, 0, SECTOR);
    put32(d + 16, VDS_SECTORS * SECTOR); put32(d + 20, MVDS_SECTOR);
    put32(d + 24, VDS_SECTORS * SECTOR); put32(d + 28, (uint32_t)w->rvds_sector);
    tag(d, TAG_AVDP, location, 512);
}

/* A volume descriptor sequence starting at sector `at`: PVD, IUVD, PD, LVD, USD, TD. */
static int vds(udfw *w, uint8_t *s, uint32_t at)
{
    uint8_t *d;
    memset(s, 0, (size_t)VDS_SECTORS * SECTOR);

    d = s;                                             /* primary volume descriptor */
    put32(d + 16, 0);
    if (dstring(d + 24, 32, w->volume_id)) return fail(w, "volume id too long for UDF (30 Latin-1 characters)");
    put16(d + 56, 1); put16(d + 58, 1); put16(d + 60, 2); put16(d + 62, 3);
    put32(d + 64, 1); put32(d + 68, 1);
    dstring(d + 72, 128, w->volume_set);
    charspec(d + 200); charspec(d + 264);
    regid(d + 344, "*arv udfwrite", 0);
    timestamp(d + 376, w->opt.time);
    regid(d + 388, "*arv udfwrite", 0);
    tag(d, TAG_PVD, at, 512);

    d = s + SECTOR;                                    /* implementation use: LV info */
    put32(d + 16, 1);
    regid(d + 20, "*UDF LV Info", 1);
    charspec(d + 52);
    dstring(d + 116, 128, w->label);
    regid(d + 352, "*arv udfwrite", 0);
    tag(d, TAG_IUVD, at + 1, 512);

    d = s + 2 * SECTOR;                                /* partition descriptor */
    put32(d + 16, 2);
    put16(d + 20, 1);                                  /* allocated */
    regid(d + 24, "+NSR03", 0);
    put32(d + 184, 1);                                 /* access type: read only */
    put32(d + 188, PART_SECTOR);
    put32(d + 192, w->part_len);
    regid(d + 196, "*arv udfwrite", 0);
    tag(d, TAG_PD, at + 2, 512);

    d = s + 3 * SECTOR;                                /* logical volume descriptor */
    put32(d + 16, 3);
    charspec(d + 20);
    if (dstring(d + 84, 128, w->label)) return fail(w, "label too long or not UTF-8");
    put32(d + 212, SECTOR);
    regid(d + 216, "*OSTA UDF Compliant", 1);
    long_ad(d + 248, SECTOR, 0, 1, 0);                 /* the file set descriptor */
    put32(d + 264, 6 + 64);
    put32(d + 268, 2);
    regid(d + 272, "*arv udfwrite", 0);
    put32(d + 432, LVIS_SECTORS * SECTOR); put32(d + 436, LVIS_SECTOR);
    uint8_t *pm = d + 440;
    pm[0] = 1; pm[1] = 6; put16(pm + 2, 1); put16(pm + 4, 0);       /* type 1: partition 0 */
    pm += 6;
    pm[0] = 2; pm[1] = 64;                                         /* type 2: metadata partition */
    regid(pm + 4, "*UDF Metadata Partition", 1);
    put16(pm + 36, 1); put16(pm + 38, 0);
    put32(pm + 40, 0);                                 /* metadata file entry: block 0 */
    put32(pm + 44, w->mirror_fe);                      /* mirror file entry: the last block */
    put32(pm + 48, 0xFFFFFFFFu);                       /* no bitmap: read-only */
    put32(pm + 52, ALIGN);
    put16(pm + 56, ALIGN);
    pm[58] = 1;                                        /* duplicate metadata: a real mirror */
    tag(d, TAG_LVD, at + 3, 440 + 70);

    d = s + 4 * SECTOR;                                /* unallocated space: none */
    put32(d + 16, 4);
    tag(d, TAG_USD, at + 4, 24);

    tag(s + 5 * SECTOR, TAG_TD, at + 5, 512);
    return 0;
}

static void lvid(udfw *w, uint8_t *s)
{
    uint8_t *d = s;
    memset(s, 0, 2 * SECTOR);
    timestamp(d + 16, w->opt.time);
    put32(d + 28, 1);                                  /* closed */
    put64(d + 40, 16 + w->norder);                     /* next unique id */
    put32(d + 72, 2);                                  /* partitions */
    put32(d + 76, 46);                                 /* implementation use length */
    put32(d + 80, 0); put32(d + 84, 0);                /* free space: none */
    put32(d + 88, w->part_len); put32(d + 92, w->meta_blocks);
    regid(d + 96, "*arv udfwrite", 0);
    put32(d + 128, (uint32_t)w->nfiles);
    put32(d + 132, (uint32_t)w->ndirs);
    put16(d + 136, UDF_REV); put16(d + 138, UDF_REV); put16(d + 140, UDF_REV);
    tag(d, TAG_LVID, LVIS_SECTOR, 142);
    tag(s + SECTOR, TAG_TD, LVIS_SECTOR + 1, 512);
}

/* ------------------------------------------------------------------ writing */

typedef struct {
    udfw *w;
    udfw_write_fn write;
    void *ctx;
    uint64_t at;                /* next sector to write */
} out;

static int emit(out *o, const void *buf, uint64_t count)
{
    if (o->write(o->ctx, buf, (size_t)count)) return fail(o->w, "write failed at sector %llu", (unsigned long long)o->at);
    o->at += count;
    return 0;
}

static int zeros_to(out *o, uint64_t sector)
{
    static const uint8_t z[64 * SECTOR];
    while (o->at < sector) {
        uint64_t n = sector - o->at < 64 ? sector - o->at : 64;
        if (emit(o, z, n)) return -1;
    }
    return 0;
}

static int file_data(out *o, node *n, uint8_t *buf, size_t bufsize)
{
    uint64_t off = 0;
    while (off < n->size) {
        size_t want = n->size - off < bufsize ? (size_t)(n->size - off) : bufsize;
        long got = n->read(n->file_ctx, off, buf, want);
        if (got != (long)want) return fail(o->w, "%s: could not read %zu bytes at %llu (changed or unreadable?)",
                                           n->path, want, (unsigned long long)off);
        size_t padded = (want + SECTOR - 1) / SECTOR * SECTOR;
        memset(buf + want, 0, padded - want);
        if (emit(o, buf, padded / SECTOR)) return -1;
        off += want;
    }
    return 0;
}

int udfw_write(udfw *w, udfw_write_fn write, void *ctx, udfw_extent_fn extents, void *extents_ctx)
{
    uint8_t d[VDS_SECTORS * SECTOR];
    size_t bufsize = 512 * SECTOR;
    uint8_t *buf;
    out o = { w, write, ctx, 0 };

    if (layout(w) || (!w->meta && build_metadata(w))) return -1;
    if (!(buf = malloc(bufsize))) return fail(w, "out of memory");

    /* volume recognition sequence (ECMA-167 2/9.1, 3/9.1) */
    if (zeros_to(&o, VRS_SECTOR)) goto err;
    static const char *vsd[] = { "BEA01", "NSR03", "TEA01" };
    for (int i = 0; i < 3; i++) {
        memset(d, 0, SECTOR);
        memcpy(d + 1, vsd[i], 5);
        d[6] = 1;
        if (emit(&o, d, 1)) goto err;
    }
    if (zeros_to(&o, ANCHOR_SECTOR)) goto err;
    anchor(w, d, ANCHOR_SECTOR);
    if (emit(&o, d, 1) || zeros_to(&o, MVDS_SECTOR)) goto err;
    if (vds(w, d, MVDS_SECTOR) || emit(&o, d, VDS_SECTORS) || zeros_to(&o, LVIS_SECTOR)) goto err;
    lvid(w, d);
    if (emit(&o, d, 2) || zeros_to(&o, PART_SECTOR)) goto err;

    /* the partition */
    efe(w, d, NULL, FT_METADATA, 0);
    if (emit(&o, d, 1) || zeros_to(&o, PART_SECTOR + ALIGN)) goto err;
    if (emit(&o, w->meta, w->meta_blocks)) goto err;
    for (size_t i = 0; i < w->norder; i++) {
        node *n = w->order[i];
        if (n->is_dir) continue;
        if (o.at != PART_SECTOR + (uint64_t)n->data_lbn) { fail(w, "internal: layout mismatch"); goto err; }
        if (file_data(&o, n, buf, bufsize)) goto err;
        if (extents) extents(extents_ctx, n->path, PART_SECTOR + (uint64_t)n->data_lbn, n->size);
    }
    if (zeros_to(&o, PART_SECTOR + (uint64_t)w->mirror_start)) goto err;
    if (emit(&o, w->meta, w->meta_blocks)) goto err;             /* the mirror: a real copy */
    efe(w, d, NULL, FT_MIRROR, w->mirror_fe);
    if (emit(&o, d, 1)) goto err;

    /* reserve volume descriptor sequence and the end anchors */
    if (vds(w, d, (uint32_t)w->rvds_sector) || emit(&o, d, VDS_SECTORS)) goto err;
    anchor(w, d, (uint32_t)(w->last_sector - 256));
    if (emit(&o, d, 1) || zeros_to(&o, w->last_sector)) goto err;
    anchor(w, d, (uint32_t)w->last_sector);
    if (emit(&o, d, 1)) goto err;
    free(buf);
    return 0;
err:
    free(buf);
    return -1;
}
