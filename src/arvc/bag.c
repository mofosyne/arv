/* BagIt (RFC 8493) for a payload that is not moved (src/arv/bag.py), and the listing
 * (src/arv/listing.py, version 2). The links policy is docs/spec/smart-archive-format.md, "Links". */
#define _XOPEN_SOURCE 700
#include "arvc.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void entries_add(entries *l, entry e)
{
    l->v = xrealloc(l->v, (l->n + 1) * sizeof *l->v);
    l->v[l->n++] = e;
}

static int inside(const char *path, const char *root)
{
    size_t n = strlen(root);
    return !strcmp(path, root) || (!strncmp(path, root, n) && (path[n] == '/' || !strcmp(root, "/")));
}

static int by_entry_path(const void *a, const void *b)
{
    return strcmp(((const entry *)a)->path, ((const entry *)b)->path);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

typedef struct {
    char *root;                 /* realpath of the payload folder */
    const char *policy;
    entries files;              /* path, source (disk path), link (target of a file link, or "") */
    entries noted;
    strlist special, loops, refused, ancestors;
} scan;

static void walk(scan *s, const char *disk, const char *rel, int via_folder_link)
{
    DIR *d = opendir(disk);
    if (!d) {
        fprintf(stderr, "Error: cannot read %s: %s\n", disk, strerror(errno));
        exit(1);
    }
    strlist names = { 0 };
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) strlist_add(&names, e->d_name);
    closedir(d);
    if (names.n) qsort(names.v, names.n, sizeof *names.v, by_name);
    for (size_t i = 0; i < names.n; i++) {
        char *full = join(disk, names.v[i]), *r = *rel ? join(rel, names.v[i]) : xstrdup(names.v[i]);
        struct stat st;
        if (lstat(full, &st)) die("cannot read %s", full);
        if (!S_ISLNK(st.st_mode)) {
            if (S_ISDIR(st.st_mode)) {
                char *real = realpath(full, NULL);
                strlist_add(&s->ancestors, real);
                walk(s, full, r, via_folder_link);
                free(s->ancestors.v[--s->ancestors.n]);
                free(real);
            } else if (S_ISREG(st.st_mode)) {
                entry f = { 0 };
                f.path = xstrdup(r);
                f.source = via_folder_link ? realpath(full, NULL) : xstrdup(full);
                f.link = NULL;
                f.via_folder = via_folder_link;
                entries_add(&s->files, f);
            } else {
                strlist_add(&s->special, full);
            }
            free(full);
            free(r);
            continue;
        }
        char target[4096];
        ssize_t tl = readlink(full, target, sizeof target - 1);
        target[tl > 0 ? tl : 0] = 0;
        char *real = realpath(full, NULL);
        struct stat ts;
        if (!real || stat(full, &ts)) {                       /* broken: noted, with a warning */
            entry n = { 0 };
            n.path = xstrdup(r);
            n.kind = xstrdup("link broken");
            n.link = xstrdup(target);
            n.mtime = st.st_mtime;
            entries_add(&s->noted, n);
            fprintf(stderr, "Warning: broken link %s -> %s (recorded in the listing, not archived)\n", r, target);
        } else {
            int in = inside(real, s->root);
            char *line = xprintf("%s -> %s", r, target);
            if (!in && !strcmp(s->policy, "default")) {
                strlist_add(&s->refused, line);
            } else if (S_ISDIR(ts.st_mode)) {
                int loops = 0;
                for (size_t k = 0; k < s->ancestors.n && !loops; k++) loops = inside(s->ancestors.v[k], real);
                if (!strcmp(s->policy, "copy") && loops) {
                    strlist_add(&s->loops, line);
                } else if (!strcmp(s->policy, "copy")) {
                    entry n = { 0 };
                    n.path = xstrdup(r);
                    n.kind = xstrdup("link copied folder");
                    n.link = xstrdup(target);
                    n.mtime = st.st_mtime;
                    entries_add(&s->noted, n);
                    strlist_add(&s->ancestors, real);
                    walk(s, full, r, 1);
                    free(s->ancestors.v[--s->ancestors.n]);
                } else {
                    entry n = { 0 };
                    n.path = xstrdup(r);
                    n.kind = xstrdup(in ? "link recorded folder" : "link recorded external");
                    n.link = xstrdup(target);
                    n.mtime = st.st_mtime;
                    entries_add(&s->noted, n);
                }
            } else if (S_ISREG(ts.st_mode)) {
                if (!in && !strcmp(s->policy, "record")) {
                    entry n = { 0 };
                    n.path = xstrdup(r);
                    n.kind = xstrdup("link recorded external");
                    n.link = xstrdup(target);
                    n.mtime = st.st_mtime;
                    entries_add(&s->noted, n);
                } else {
                    entry f = { 0 };
                    f.path = xstrdup(r);
                    f.source = xstrdup(real);
                    f.link = xstrdup(target);
                    entries_add(&s->files, f);
                }
            } else {
                strlist_add(&s->special, full);
            }
            free(line);
        }
        free(real);
        free(full);
        free(r);
    }
    strlist_free(&names);
}

static void fail_list(const char *intro, const strlist *l)
{
    fprintf(stderr, "Error: %s", intro);
    for (size_t i = 0; i < l->n && i < 20; i++) fprintf(stderr, "\n  %s", l->v[i]);
    fputc('\n', stderr);
    exit(1);
}

static int ambiguous(const char *p)
{
    for (const char *s = p; *s; s++) {
        if (*s == '\r' || *s == '\n') return 1;
        if (*s == '%' && s[1] && s[2]) {
            char h[3] = { (char)toupper((unsigned char)s[1]), (char)toupper((unsigned char)s[2]), 0 };
            if (!strcmp(h, "0A") || !strcmp(h, "0D") || !strcmp(h, "25")) return 1;
        }
    }
    return 0;
}

/* hashes a file with both algorithms */
void hash_both(const char *path, char sha256[65], char sha512[129])
{
    static unsigned char buf[1 << 20];
    sha256_ctx a;
    sha512_ctx b;
    unsigned char da[32], db[64];
    ssize_t n;
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot read %s", path);
    sha256_init(&a);
    sha512_init(&b);
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        sha256_update(&a, buf, (size_t)n);
        sha512_update(&b, buf, (size_t)n);
    }
    if (n < 0) die("cannot read %s", path);
    close(fd);
    sha256_final(&a, da);
    sha512_final(&b, db);
    sha256_hex(da, sha256);
    sha512_hex(db, sha512);
}

void scan_payload(const char *src, const char *policy, entries *files, entries *noted)
{
    scan s;
    memset(&s, 0, sizeof s);
    s.root = realpath(src, NULL);
    if (!s.root) die("cannot read %s", src);
    s.policy = policy;
    strlist_add(&s.ancestors, s.root);
    walk(&s, s.root, "", 0);

    strlist amb = { 0 };
    for (size_t i = 0; i < s.files.n; i++) if (ambiguous(s.files.v[i].path)) strlist_add(&amb, s.files.v[i].path);
    for (size_t i = 0; i < s.noted.n; i++) if (ambiguous(s.noted.v[i].path)) strlist_add(&amb, s.noted.v[i].path);
    if (amb.n)
        fail_list("file names containing line breaks or %0A / %0D / %25 cannot be listed\n"
                  "unambiguously in BagIt manifests; please rename:", &amb);
    if (s.special.n) fail_list("only files, folders and links can be archived; please remove:", &s.special);
    if (s.loops.n) fail_list("links that loop back to a folder containing them:", &s.loops);
    if (s.refused.n)
        fail_list("links pointing outside the folder (use --links record to note them in the listing,\n"
                  "or --links copy to archive what they point to):", &s.refused);

    uint64_t total = 0, done = 0;
    int progress = isatty(2);            /* "Hashing n/N files (p%)" on a terminal, as bag.scan_payload */
    for (size_t i = 0; i < s.files.n; i++) {
        struct stat st;
        if (!stat(s.files.v[i].source, &st)) total += (uint64_t)st.st_size;
    }
    for (size_t i = 0; i < s.files.n; i++) {
        entry *e = &s.files.v[i];
        struct stat st;
        if (stat(e->source, &st)) die("cannot read %s", e->source);
        e->size = (uint64_t)st.st_size;
        e->mtime = st.st_mtime;
        int exec = (st.st_mode & 0111) != 0;
        e->kind = xprintf("%s%s", e->link ? "link copied" : "file", exec ? " executable" : "");
        if (!e->link) e->link = xstrdup("");
        hash_both(e->source, e->sha256, e->sha512);
        done += e->size;
        if (progress) fprintf(stderr, "\rHashing %zu/%zu files (%.0f%%)", i + 1, s.files.n,
                              total ? 100.0 * (double)done / (double)total : 100.0);
    }
    if (progress) fputc('\n', stderr);
    if (s.files.n) qsort(s.files.v, s.files.n, sizeof *s.files.v, by_entry_path);
    if (s.noted.n) qsort(s.noted.v, s.noted.n, sizeof *s.noted.v, by_entry_path);
    *files = s.files;
    *noted = s.noted;
}

/* "links: 2 copied, 1 recorded, 1 broken (policy: default)", or NULL without links */
char *link_summary(const entries *files, const entries *noted, const char *policy)
{
    size_t copied = 0, recorded = 0, broken = 0;
    const entries *both[2] = { files, noted };
    for (int k = 0; k < 2; k++)
        for (size_t i = 0; i < both[k]->n; i++) {
            const char *kind = both[k]->v[i].kind;
            if (strncmp(kind, "link ", 5)) continue;
            if (!strncmp(kind + 5, "copied", 6)) copied++;
            else if (!strncmp(kind + 5, "recorded", 8)) recorded++;
            else if (!strncmp(kind + 5, "broken", 6)) broken++;
        }
    if (!copied && !recorded && !broken) return NULL;
    sbuf b = { 0 };
    sb_puts(&b, "links: ");
    const char *sep = "";
    if (copied) { sb_printf(&b, "%s%zu copied", sep, copied); sep = ", "; }
    if (recorded) { sb_printf(&b, "%s%zu recorded", sep, recorded); sep = ", "; }
    if (broken) sb_printf(&b, "%s%zu broken", sep, broken);
    sb_printf(&b, " (policy: %s)", policy);
    return b.s;
}

static void utc(time_t t, char out[21])
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 21, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

void write_listing(const char *path, const entries *files, const entries *noted)
{
    size_t n = files->n + noted->n, a = 0, b = 0;
    sbuf out = { 0 };
    sb_puts(&out, "# arv listing 2\tsize (bytes)\tmodified (UTC, ISO 8601)\tkind\tlink target\tpath (relative to data/)\n");
    for (size_t i = 0; i < n; i++) {               /* merge the two sorted lists by path */
        int take_noted = a >= files->n || (b < noted->n && strcmp(noted->v[b].path, files->v[a].path) < 0);
        const entry *e = take_noted ? &noted->v[b++] : &files->v[a++];
        char when[21];
        utc(e->mtime, when);
        if (take_noted) sb_printf(&out, "-\t%s\t%s\t%s\t%s\n", when, e->kind, *e->link ? e->link : "-", e->path);
        else sb_printf(&out, "%llu\t%s\t%s\t%s\t%s\n", (unsigned long long)e->size, when, e->kind,
                       e->link && *e->link ? e->link : "-", e->path);
    }
    write_text(path, out.s);
    free(out.s);
}

void write_manifest(const char *path, const entries *files, int sha512)
{
    sbuf out = { 0 };
    for (size_t i = 0; i < files->n; i++)
        sb_printf(&out, "%s  data/%s\n", sha512 ? files->v[i].sha512 : files->v[i].sha256, files->v[i].path);
    write_text(path, out.s ? out.s : "");
    free(out.s);
}

void write_bag_tags(const char *stage, const entries *files, const strlist *info)
{
    char *p = join(stage, "bagit.txt");
    write_text(p, "BagIt-Version: 1.0\nTag-File-Character-Encoding: UTF-8\n");
    free(p);
    sbuf b = { 0 };
    for (size_t i = 0; i + 1 < info->n; i += 2) {        /* label, value pairs */
        sb_printf(&b, "%s: ", info->v[i]);
        for (const char *v = info->v[i + 1]; *v; v++) {
            if (*v == '\n') sb_puts(&b, "\n  ");
            else sb_add(&b, v, 1);
        }
        sb_puts(&b, "\n");
    }
    p = join(stage, "bag-info.txt");
    write_text(p, b.s);
    free(p);
    free(b.s);
    p = join(stage, "manifest-sha256.txt");
    write_manifest(p, files, 0);
    free(p);
    p = join(stage, "manifest-sha512.txt");
    write_manifest(p, files, 1);
    free(p);
}

/* every file under dir in os.walk order: a folder's files (sorted), then its folders (sorted) */
static void walk_files(const char *dir, const char *rel, strlist *out)
{
    DIR *d = opendir(dir);
    strlist files = { 0 }, dirs = { 0 };
    struct dirent *e;
    if (!d) die("cannot read %s", dir);
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *full = join(dir, e->d_name);
        struct stat st;
        if (!lstat(full, &st)) strlist_add(S_ISDIR(st.st_mode) ? &dirs : &files, e->d_name);
        free(full);
    }
    closedir(d);
    if (files.n) qsort(files.v, files.n, sizeof *files.v, by_name);
    if (dirs.n) qsort(dirs.v, dirs.n, sizeof *dirs.v, by_name);
    for (size_t i = 0; i < files.n; i++) {
        char *r = *rel ? join(rel, files.v[i]) : xstrdup(files.v[i]);
        strlist_add(out, r);
        free(r);
    }
    for (size_t i = 0; i < dirs.n; i++) {
        char *full = join(dir, dirs.v[i]), *r = *rel ? join(rel, dirs.v[i]) : xstrdup(dirs.v[i]);
        walk_files(full, r, out);
        free(full);
        free(r);
    }
    strlist_free(&files);
    strlist_free(&dirs);
}

/* tagmanifest-sha256/512.txt over every file in the stage but themselves (call last) */
void write_tagmanifests(const char *stage)
{
    strlist all = { 0 };
    walk_files(stage, "", &all);
    sbuf m256 = { 0 }, m512 = { 0 };
    for (size_t i = 0; i < all.n; i++) {
        if (!strncmp(all.v[i], "tagmanifest-", 12)) continue;
        char *full = join(stage, all.v[i]), a[65], b[129];
        hash_both(full, a, b);
        sb_printf(&m256, "%s  %s\n", a, all.v[i]);
        sb_printf(&m512, "%s  %s\n", b, all.v[i]);
        free(full);
    }
    char *p = join(stage, "tagmanifest-sha256.txt");
    write_text(p, m256.s ? m256.s : "");
    free(p);
    p = join(stage, "tagmanifest-sha512.txt");
    write_text(p, m512.s ? m512.s : "");
    free(p);
    free(m256.s);
    free(m512.s);
    strlist_free(&all);
}
