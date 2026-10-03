/*
 * arvc: arv's reader in C (research/plan.md, the C port; it becomes `arv` when the port is done).
 *
 *   arvc info DISC                 the disc's record, binding and appraisals
 *   arvc verify [-v] DISC          every file against the BagIt manifests; extra files in data/
 *   arvc ls DISC                   the listing: files, executables and links
 *   arvc restore [--no-links] DISC DEST
 *                                  copy data/ to DEST, checking every file on the way; restore
 *                                  modification times, execute bits and the source's links
 *
 * DISC is the root of a mounted disc or an extracted image (the folder with catalog.rec). Needs
 * only C99 and POSIX: the same reading can be done by hand with sha256sum, cat and ln.
 */
#define _POSIX_C_SOURCE 200809L
#include "rec.h"
#include "sha256.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define VERSION "arvc 0.1"

static void die(const char *fmt, const char *arg)
{
    fprintf(stderr, "arvc: ");
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
    exit(2);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("%s", "out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) die("%s", "out of memory");
    return p;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    return memcpy(xmalloc(n), s, n);
}

static char *join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    char *p = xmalloc(la + lb + 2);
    memcpy(p, a, la);
    p[la] = '/';
    memcpy(p + la + 1, b, lb + 1);
    return p;
}

/* ------------------------------------------------------------------ manifests */

typedef struct {
    char *path;     /* as in the manifest: data/... or a tag file */
    char hex[65];
    int seen;
} mentry;

typedef struct {
    mentry *e;
    size_t n;
} manifest;

static int by_path(const void *a, const void *b)
{
    return strcmp(((const mentry *)a)->path, ((const mentry *)b)->path);
}

/* "<sha256>  <path>" per line (BagIt; paths written raw, see docs/smart-archive-format.md) */
static int read_manifest(const char *file, manifest *m)
{
    FILE *fp = fopen(file, "rb");
    char *line = NULL;
    size_t cap = 0, mcap = 0;
    ssize_t len;
    m->e = NULL;
    m->n = 0;
    if (!fp) return -1;
    while ((len = getline(&line, &cap, fp)) >= 0) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (len < 66 || line[64] != ' ') continue;
        char *path = line + 65;
        while (*path == ' ' || *path == '*') path++;
        if (m->n == mcap) {
            mcap = mcap ? mcap * 2 : 256;
            m->e = xrealloc(m->e, mcap * sizeof *m->e);
        }
        memcpy(m->e[m->n].hex, line, 64);
        m->e[m->n].hex[64] = 0;
        for (int i = 0; i < 64; i++)
            if (m->e[m->n].hex[i] >= 'A' && m->e[m->n].hex[i] <= 'F') m->e[m->n].hex[i] += 'a' - 'A';
        m->e[m->n].path = xstrdup(path);
        m->e[m->n].seen = 0;
        m->n++;
    }
    free(line);
    fclose(fp);
    qsort(m->e, m->n, sizeof *m->e, by_path);
    return 0;
}

static mentry *find_entry(const manifest *m, const char *path)
{
    mentry key = { (char *)path, { 0 }, 0 };
    return m->n ? bsearch(&key, m->e, m->n, sizeof *m->e, by_path) : NULL;
}

static void free_manifest(manifest *m)
{
    for (size_t i = 0; i < m->n; i++) free(m->e[i].path);
    free(m->e);
}

/* Hashes `path`, also writing its bytes to `out_fd` when it is not -1. Returns 0, -1 when the
 * file cannot be read (errno set), -2 when it cannot be written. */
static int hash_file(const char *path, char hex[65], int out_fd, uint64_t *bytes)
{
    static unsigned char buf[1 << 20];
    sha256_ctx c;
    unsigned char digest[32];
    ssize_t n;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    sha256_init(&c);
    if (bytes) *bytes = 0;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        sha256_update(&c, buf, (size_t)n);
        if (bytes) *bytes += (uint64_t)n;
        for (ssize_t done = 0; out_fd >= 0 && done < n;) {
            ssize_t w = write(out_fd, buf + done, (size_t)(n - done));
            if (w < 0) { close(fd); return -2; }
            done += w;
        }
    }
    int e = errno;
    close(fd);
    if (n < 0) { errno = e; return -1; }
    sha256_final(&c, digest);
    sha256_hex(digest, hex);
    return 0;
}

/* ------------------------------------------------------------------ the disc */

typedef struct {
    char *root;
    rec_file cat;           /* catalog.rec */
    const rec_record *disc;
    const char *id;
} disc;

static void open_disc(const char *root, disc *d)
{
    int bad = 0;
    char *path = join(root, "catalog.rec");
    memset(d, 0, sizeof *d);
    d->root = xstrdup(root);
    if (rec_read(path, &d->cat, &bad)) {
        if (errno == ENOENT)
            die("%s: no catalog.rec here (give the root of a mounted disc or an extracted image)", root);
        if (errno == EINVAL) {
            fprintf(stderr, "arvc: %s: line %d is not a field\n", path, bad);
            exit(2);
        }
        die("cannot read %s", path);
    }
    free(path);
    d->disc = rec_first(&d->cat, "Disc");
    if (!d->disc || !(d->id = rec_get(d->disc, "Id"))) die("%s/catalog.rec has no Disc record with an Id", root);
}

static char *volume_file(const disc *d, const char *name)
{
    char *dir = join(d->root, "catalog/volumes"), *vol = join(dir, d->id), *p = join(vol, name);
    free(dir);
    free(vol);
    return p;
}

/* ------------------------------------------------------------------ listing */

typedef struct {
    long long size;         /* -1: only noted (not in data/) */
    char *modified, *kind, *link, *path;
} lrow;

typedef struct {
    lrow *r;
    size_t n;
} listing;

static int split_tabs(char *line, char **cols, int want)
{
    int n = 0;
    cols[n++] = line;
    while (n < want) {
        char *t = strchr(cols[n - 1], '\t');
        if (!t) break;
        *t = 0;
        cols[n++] = t + 1;
    }
    return n;
}

/* Listing version 2 (size, modified, kind, link target, path) or 1 (size, modified, path). */
static int read_listing(const char *file, listing *l)
{
    FILE *fp = fopen(file, "rb");
    char *line = NULL;
    size_t cap = 0, lcap = 0;
    ssize_t len;
    int version = 1;
    l->r = NULL;
    l->n = 0;
    if (!fp) return -1;
    while ((len = getline(&line, &cap, fp)) >= 0) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (line[0] == '#') {
            char word[32];
            int v;
            if (sscanf(line + 1, " %31s listing %d", word, &v) == 2) version = v;
            continue;
        }
        if (!len) continue;
        char *c[5];
        int want = version >= 2 ? 5 : 3;
        if (split_tabs(line, c, want) != want) continue;
        if (l->n == lcap) {
            lcap = lcap ? lcap * 2 : 256;
            l->r = xrealloc(l->r, lcap * sizeof *l->r);
        }
        lrow *r = &l->r[l->n++];
        r->size = strcmp(c[0], "-") ? atoll(c[0]) : -1;
        r->modified = xstrdup(c[1]);
        r->kind = xstrdup(version >= 2 ? c[2] : "file");
        r->link = xstrdup(version >= 2 && strcmp(c[3], "-") ? c[3] : "");
        r->path = xstrdup(c[want - 1]);
    }
    free(line);
    fclose(fp);
    return 0;
}

static void free_listing(listing *l)
{
    for (size_t i = 0; i < l->n; i++) {
        free(l->r[i].modified);
        free(l->r[i].kind);
        free(l->r[i].link);
        free(l->r[i].path);
    }
    free(l->r);
}

static int has_word(const char *kind, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = kind; (p = strstr(p, word)); p += n)
        if ((p == kind || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return 1;
    return 0;
}

/* "2019-07-14T09:12:03Z" -> seconds since 1970 (UTC), or -1 */
static long long parse_utc(const char *s)
{
    int y, mo, d, h, mi, se;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2dZ", &y, &mo, &d, &h, &mi, &se) != 6) return -1;
    /* days from civil (Howard Hinnant's algorithm): no timegm, which is not POSIX */
    y -= mo <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + se;
}

/* ------------------------------------------------------------------ info */

static void print_record(const rec_record *r)
{
    for (size_t i = 0; i < r->nfields; i++) {
        const char *v = r->fields[i].value;
        printf("%s: ", r->fields[i].name);
        for (; *v; v++) {
            putchar(*v);
            if (*v == '\n') fputs("  ", stdout);
        }
        putchar('\n');
    }
}

static int cmd_info(int argc, char **argv)
{
    disc d;
    if (argc != 1) return 2;
    open_disc(argv[0], &d);
    print_record(d.disc);
    for (size_t i = 0; i < d.cat.nrecords; i++) {
        const rec_record *r = &d.cat.records[i];
        if (r->descriptor || !r->type) continue;
        if (!strcmp(r->type, "Binding") || !strcmp(r->type, "Appraisal")) {
            printf("\n%s\n", r->type);
            print_record(r);
        }
    }
    size_t events = 0;
    for (size_t i = 0; i < d.cat.nrecords; i++)
        if (!d.cat.records[i].descriptor && d.cat.records[i].type && !strcmp(d.cat.records[i].type, "Event")) events++;
    printf("\n%zu events in catalog.rec; every disc this one knows: catalog/archive.rec\n", events);
    return 0;
}

/* ------------------------------------------------------------------ verify */

typedef struct {
    size_t ok, failed, missing, extra;
    int verbose;
} tally;

static void check_manifest(const char *root, manifest *m, tally *t)
{
    for (size_t i = 0; i < m->n; i++) {
        char hex[65];
        char *path = join(root, m->e[i].path);
        m->e[i].seen = 1;
        if (hash_file(path, hex, -1, NULL)) {
            printf("MISSING  %s (%s)\n", m->e[i].path, strerror(errno));
            t->missing++;
        } else if (strcmp(hex, m->e[i].hex)) {
            printf("FAILED   %s\n", m->e[i].path);
            t->failed++;
        } else {
            if (t->verbose) printf("OK       %s\n", m->e[i].path);
            t->ok++;
        }
        free(path);
    }
}

/* files under data/ that no manifest line names */
static void find_extra(const char *root, const char *rel, const manifest *m, tally *t)
{
    char *dir = join(root, rel);
    DIR *dp = opendir(dir);
    struct dirent *e;
    if (!dp) {
        if (strcmp(rel, "data")) printf("MISSING  %s/ (%s)\n", rel, strerror(errno));
        free(dir);
        return;
    }
    while ((e = readdir(dp))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *r = join(rel, e->d_name), *full = join(root, r);
        struct stat st;
        if (!lstat(full, &st) && S_ISDIR(st.st_mode)) find_extra(root, r, m, t);
        else if (!find_entry(m, r)) {
            printf("EXTRA    %s\n", r);
            t->extra++;
        }
        free(r);
        free(full);
    }
    closedir(dp);
    free(dir);
}

static int cmd_verify(int argc, char **argv)
{
    tally t = { 0 };
    disc d;
    manifest payload, tags;
    if (argc == 2 && !strcmp(argv[0], "-v")) { t.verbose = 1; argc--; argv++; }
    if (argc != 1) return 2;
    open_disc(argv[0], &d);
    char *mpath = join(d.root, "manifest-sha256.txt"), *tpath = join(d.root, "tagmanifest-sha256.txt");
    if (read_manifest(mpath, &payload)) die("%s: no manifest-sha256.txt (not a bag?)", d.root);
    check_manifest(d.root, &payload, &t);
    size_t files = t.ok + t.failed + t.missing;
    if (!read_manifest(tpath, &tags)) {
        check_manifest(d.root, &tags, &t);
        free_manifest(&tags);
    } else {
        printf("MISSING  tagmanifest-sha256.txt\n");
        t.missing++;
    }
    find_extra(d.root, "data", &payload, &t);
    free_manifest(&payload);
    free(mpath);
    free(tpath);
    if (!t.failed && !t.missing && !t.extra) {
        printf("%s: all %zu files and %zu tag files match their checksums\n", d.id, files, t.ok - files);
        return 0;
    }
    printf("%s: %zu OK, %zu FAILED, %zu MISSING, %zu EXTRA\n", d.id, t.ok, t.failed, t.missing, t.extra);
    if (t.failed || t.missing)
        printf("Repair the image with its RS03 error correction first (dvdisaster -f), then verify again.\n");
    return 1;
}

/* ------------------------------------------------------------------ ls */

static int cmd_ls(int argc, char **argv)
{
    disc d;
    listing l;
    if (argc != 1) return 2;
    open_disc(argv[0], &d);
    char *path = volume_file(&d, "listing.tsv");
    if (read_listing(path, &l)) die("no listing at %s", path);
    for (size_t i = 0; i < l.n; i++) {
        lrow *r = &l.r[i];
        if (r->size < 0) printf("%-26s %12s  %s", r->kind, "-", r->path);
        else printf("%-26s %12lld  %s", r->kind, r->size, r->path);
        if (*r->link) printf(" -> %s", r->link);
        putchar('\n');
    }
    free_listing(&l);
    free(path);
    return 0;
}

/* ------------------------------------------------------------------ restore */

/* A path from the disc must stay inside DEST: relative, no "..", no empty parts. */
static int safe_rel(const char *p)
{
    if (!*p || *p == '/') return 0;
    for (const char *s = p; *s;) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
        s += n + (e != NULL);
    }
    return 1;
}

/* Creates the folders above dest/rel, refusing to pass through a symbolic link. */
static int make_parents(const char *dest, const char *rel)
{
    char *p = join(dest, rel);
    size_t base = strlen(dest) + 1;
    for (char *s = p + base; (s = strchr(s, '/')); s++) {
        struct stat st;
        *s = 0;
        if (lstat(p, &st)) {
            if (mkdir(p, 0755) && errno != EEXIST) { free(p); return -1; }
        } else if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            free(p);
            return -1;
        }
        *s = '/';
    }
    free(p);
    return 0;
}

/* "dir/sub" + "../x" -> "dir/x"; NULL when it leaves the top or is absolute */
static char *resolve_link(const char *link_path, const char *target)
{
    if (target[0] == '/') return NULL;
    char *dir = xstrdup(link_path), *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else dir[0] = 0;
    char *all = *dir ? join(dir, target) : xstrdup(target);
    free(dir);
    char **parts = xmalloc((strlen(all) / 2 + 2) * sizeof *parts);
    size_t n = 0;
    for (char *s = strtok(all, "/"); s; s = strtok(NULL, "/")) {
        if (!strcmp(s, ".")) continue;
        if (!strcmp(s, "..")) {
            if (!n) { free(parts); free(all); return NULL; }
            n--;
        } else parts[n++] = s;
    }
    size_t len = 1;
    for (size_t i = 0; i < n; i++) len += strlen(parts[i]) + 1;
    char *out = xmalloc(len);
    out[0] = 0;
    for (size_t i = 0; i < n; i++) {
        if (i) strcat(out, "/");
        strcat(out, parts[i]);
    }
    free(parts);
    free(all);
    return out;
}

static void set_time(const char *path, const char *modified)
{
    long long t = parse_utc(modified);
    if (t < 0) return;
    struct timespec ts[2] = { { (time_t)t, 0 }, { (time_t)t, 0 } };
    utimensat(AT_FDCWD, path, ts, AT_SYMLINK_NOFOLLOW);
}

static int cmd_restore(int argc, char **argv)
{
    int links = 1;
    if (argc >= 1 && !strcmp(argv[0], "--no-links")) { links = 0; argc--; argv++; }
    if (argc != 2) return 2;
    disc d;
    manifest m;
    listing l = { 0 };
    const char *dest = argv[1];
    open_disc(argv[0], &d);
    char *mpath = join(d.root, "manifest-sha256.txt");
    if (read_manifest(mpath, &m)) die("%s: no manifest-sha256.txt (not a bag?)", d.root);
    free(mpath);

    DIR *dp = opendir(dest);
    if (dp) {
        struct dirent *e;
        while ((e = readdir(dp)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) die("%s is not empty", dest);
        closedir(dp);
    } else if (mkdir(dest, 0755)) {
        die("cannot create %s", dest);
    }

    size_t restored = 0, damaged = 0, missing = 0, made_links = 0, kept = 0, skipped = 0;
    uint64_t total = 0;
    for (size_t i = 0; i < m.n; i++) {
        const char *rel = m.e[i].path;
        if (strncmp(rel, "data/", 5) || !safe_rel(rel + 5)) {
            printf("SKIPPED  %s (not a safe path under data/)\n", rel);
            skipped++;
            continue;
        }
        rel += 5;
        char *src = join(d.root, m.e[i].path), *dst = join(dest, rel), hex[65];
        uint64_t bytes = 0;
        int fd = -1;
        if (make_parents(dest, rel) || (fd = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644)) < 0) {
            printf("FAILED   %s (cannot write: %s)\n", rel, strerror(errno));
            missing++;
        } else {
            int rc = hash_file(src, hex, fd, &bytes);
            if (close(fd) && !rc) rc = -2;
            if (rc == -1) {
                printf("MISSING  %s (cannot read: %s)\n", rel, strerror(errno));
                unlink(dst);
                missing++;
            } else if (rc == -2) {
                printf("FAILED   %s (cannot write: %s)\n", rel, strerror(errno));
                unlink(dst);
                missing++;
            } else if (strcmp(hex, m.e[i].hex)) {
                printf("DAMAGED  %s (restored, but its checksum differs)\n", rel);
                damaged++;
            } else {
                m.e[i].seen = 1;                /* restored intact */
                restored++;
                total += bytes;
            }
        }
        free(src);
        free(dst);
    }

    char *lpath = volume_file(&d, "listing.tsv");
    if (read_listing(lpath, &l)) printf("Note: no listing (%s): times, execute bits and links not restored\n", lpath);
    free(lpath);
    for (size_t i = 0; i < l.n; i++) {
        lrow *r = &l.r[i];
        if (!safe_rel(r->path)) continue;
        char *dst = join(dest, r->path);
        int is_link = has_word(r->kind, "link");
        if (r->size >= 0) {                     /* a file in data/: its mode and time */
            struct stat st;
            if (!lstat(dst, &st) && S_ISREG(st.st_mode))
                chmod(dst, has_word(r->kind, "executable") ? 0755 : 0644);
            if (is_link && has_word(r->kind, "copied") && links) {
                /* a link to a file: a link again when its target came back with the same bytes */
                char *target = resolve_link(r->path, r->link);
                mentry *self = NULL, *other = NULL;
                char *a = join("data", r->path), *b = target ? join("data", target) : NULL;
                self = find_entry(&m, a);
                other = b ? find_entry(&m, b) : NULL;
                char *tmp = xmalloc(strlen(dst) + 16);
                sprintf(tmp, "%s.arvc-link", dst);
                if (self && other && other->seen && !strcmp(self->hex, other->hex) && !symlink(r->link, tmp)) {
                    if (rename(tmp, dst)) {     /* replaces the copy in one step, or not at all */
                        unlink(tmp);
                        printf("KEPT     %s (%s)\n", r->path, strerror(errno));
                        kept++;
                    } else made_links++;
                } else {
                    printf("KEPT     %s (a copy of %s: its target %s)\n", r->path, r->link,
                           target ? "was not restored intact with the same bytes" : "is outside the folder");
                    kept++;
                }
                free(a);
                free(b);
                free(tmp);
                free(target);
            }
            set_time(dst, r->modified);
        } else if (is_link) {                   /* noted only: recreate the link as it was */
            if (!links) {
                skipped++;
            } else if (has_word(r->kind, "copied")) {
                printf("KEPT     %s (a copy of the folder %s)\n", r->path, r->link);
                kept++;
            } else if (make_parents(dest, r->path) || symlink(r->link, dst)) {
                printf("FAILED   %s -> %s (%s)\n", r->path, r->link, strerror(errno));
                missing++;
            } else {
                made_links++;
                set_time(dst, r->modified);
            }
        }
        free(dst);
    }

    printf("%s: restored %zu files (%llu bytes) and %zu links to %s", d.id, restored,
           (unsigned long long)total, made_links, dest);
    if (kept) printf("; %zu links kept as copies", kept);
    if (skipped) printf("; %zu skipped", skipped);
    putchar('\n');
    free_listing(&l);
    free_manifest(&m);
    if (damaged || missing) {
        printf("%zu DAMAGED, %zu MISSING. Repair the image with its RS03 error correction first "
               "(dvdisaster -f), then restore again.\n", damaged, missing);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ main */

static void usage(void)
{
    fputs("usage: arvc info DISC\n"
          "       arvc verify [-v] DISC\n"
          "       arvc ls DISC\n"
          "       arvc restore [--no-links] DISC DEST\n"
          "DISC: the root of a mounted arv disc or an extracted image (the folder with catalog.rec)\n",
          stderr);
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        int (*fn)(int, char **);
    } cmds[] = { { "info", cmd_info }, { "verify", cmd_verify }, { "ls", cmd_ls }, { "restore", cmd_restore } };
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        puts(VERSION);
        return 0;
    }
    for (size_t i = 0; argc >= 2 && i < sizeof cmds / sizeof *cmds; i++)
        if (!strcmp(argv[1], cmds[i].name)) {
            int rc = cmds[i].fn(argc - 2, argv + 2);
            if (rc == 2) usage();
            return rc;
        }
    usage();
    return 2;
}
