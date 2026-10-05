/* What a folder is, compared with the archive (research/plan.md, collections and tracked folders):
 *
 *   arv status [-v] [--deep] [FOLDER]   a collection's workflow folder: what changed since its
 *                                       last revision; any other folder: which of its files are on
 *                                       which discs, and which on none
 *   arv checkpoint [FOLDER] [--message TEXT]
 *                                       record the workflow folder's state as a revision (no discs)
 *   arv log COLLECTION                  its revisions
 *   arv diff REV [REV]                  what changed between two revisions (REV: a Node prefix, or
 *                                       CODE/N for edition N); with one, from it to the folder now
 *   arv link FOLDER COLLECTION|DISC-ID [--past]
 *                                       say what a folder is, without writing in it (logged)
 *
 * Hashing a NAS is slow, so hashes are cached in <home>/cache/hashes.tsv, keyed by path, device,
 * inode, size and modified time: a file whose five are unchanged is not read again (--deep reads
 * everything, and reports a file whose content changed while its time did not). */
#define _XOPEN_SOURCE 700
#include "arv.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void sort_(void *v, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    if (n) qsort(v, n, size, cmp);              /* an empty list may be NULL */
}

static const char *get_or(const rec_record *r, const char *name, const char *dflt)
{
    const char *v = rec_get(r, name);
    return v ? v : dflt;
}

/* ------------------------------------------------------------------ manifests in memory */

typedef struct {
    char *path, *hash;
} mline;

typedef struct {
    mline *v;
    size_t n, cap;
} mlist;

static void ml_add(mlist *m, const char *path, const char *hash)
{
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 256;
        m->v = xrealloc(m->v, m->cap * sizeof *m->v);
    }
    m->v[m->n].path = xstrdup(path);
    m->v[m->n].hash = xstrdup(hash);
    m->n++;
}

static void ml_free(mlist *m)
{
    for (size_t i = 0; i < m->n; i++) {
        free(m->v[i].path);
        free(m->v[i].hash);
    }
    free(m->v);
    memset(m, 0, sizeof *m);
}

static int ml_by_path(const void *a, const void *b)
{
    return strcmp(((const mline *)a)->path, ((const mline *)b)->path);
}

static int ml_by_hash(const void *a, const void *b)
{
    int c = strcmp(((const mline *)a)->hash, ((const mline *)b)->hash);
    return c ? c : strcmp(((const mline *)a)->path, ((const mline *)b)->path);
}

/* "HASH  PATH" lines (a disc's "data/" prefix dropped) */
static void ml_parse(mlist *m, const char *text)
{
    for (const char *l = text; l && *l;) {
        const char *nl = strchr(l, '\n'), *end = nl ? nl : l + strlen(l), *sep = strstr(l, "  ");
        if (sep && sep < end && sep - l == 64) {
            char hash[65], *path = xmalloc((size_t)(end - sep - 2) + 1);
            memcpy(hash, l, 64);
            hash[64] = 0;
            memcpy(path, sep + 2, (size_t)(end - sep - 2));
            path[end - sep - 2] = 0;
            ml_add(m, !strncmp(path, "data/", 5) ? path + 5 : path, hash);
            free(path);
        }
        l = nl ? nl + 1 : end;
    }
}

static char *ml_text(mlist *m)
{
    sort_(m->v, m->n, sizeof *m->v, ml_by_path);
    sbuf b = { 0 };
    sb_puts(&b, "");
    for (size_t i = 0; i < m->n; i++) sb_printf(&b, "%s  %s\n", m->v[i].hash, m->v[i].path);
    return b.s;
}

/* ------------------------------------------------------------------ the hash cache */

typedef struct {
    char *path;
    unsigned long long dev, ino, size;
    long long mtime;
    char hash[65];
    int seen;
} centry;

typedef struct {
    centry *v;
    size_t n, cap;
    char *file;
    size_t read, cached;        /* this run: files hashed, files taken from the cache */
} hcache;

static int ce_by_path(const void *a, const void *b)
{
    return strcmp(((const centry *)a)->path, ((const centry *)b)->path);
}

static void cache_load(hcache *c, const arv_home *h)
{
    memset(c, 0, sizeof *c);
    c->file = join(h->cache_dir, "hashes.tsv");
    char *text = access(c->file, F_OK) ? NULL : read_text(c->file);
    for (char *l = text; l && *l;) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        centry e;
        memset(&e, 0, sizeof e);
        int at = 0;
        if (sscanf(l, "%llu\t%llu\t%llu\t%lld\t%64[0-9a-f]\t%n", &e.dev, &e.ino, &e.size, &e.mtime, e.hash, &at) == 5 && at) {
            e.path = xstrdup(l + at);
            if (c->n == c->cap) {
                c->cap = c->cap ? c->cap * 2 : 1024;
                c->v = xrealloc(c->v, c->cap * sizeof *c->v);
            }
            c->v[c->n++] = e;
        }
        l = nl ? nl + 1 : l + strlen(l);
    }
    free(text);
    sort_(c->v, c->n, sizeof *c->v, ce_by_path);
}

static void cache_save(hcache *c, const arv_home *h)
{
    if (mkdirs(h->cache_dir)) return;           /* a cache: losing it costs time only */
    sort_(c->v, c->n, sizeof *c->v, ce_by_path);
    sbuf b = { 0 };
    sb_puts(&b, "");
    for (size_t i = 0; i < c->n; i++)
        sb_printf(&b, "%llu\t%llu\t%llu\t%lld\t%s\t%s\n", c->v[i].dev, c->v[i].ino, c->v[i].size, c->v[i].mtime,
                  c->v[i].hash, c->v[i].path);
    char *tmp = xprintf("%s.tmp", c->file);
    write_text(tmp, b.s);
    if (rename(tmp, c->file)) unlink(tmp);
    free(tmp);
    free(b.s);
}

/* the file's SHA-256, from the cache when nothing about it changed; 0, or -1 when unreadable */
static int cached_hash(hcache *c, const char *abs, const struct stat *st, int deep, char hex[65], int *silent_change)
{
    centry key;
    key.path = (char *)abs;
    centry *e = c->n ? bsearch(&key, c->v, c->n, sizeof *c->v, ce_by_path) : NULL;
    int same = e && e->dev == (unsigned long long)st->st_dev && e->ino == (unsigned long long)st->st_ino
               && e->size == (unsigned long long)st->st_size && e->mtime == (long long)st->st_mtime;
    *silent_change = 0;
    if (same && !deep) {
        memcpy(hex, e->hash, 65);
        c->cached++;
        return 0;
    }
    if (hash_file(abs, hex, -1, NULL)) return -1;
    c->read++;
    if (same && strcmp(e->hash, hex)) *silent_change = 1;     /* bit rot, or a tool that kept the time */
    if (!e) {
        if (c->n == c->cap) {
            c->cap = c->cap ? c->cap * 2 : 1024;
            c->v = xrealloc(c->v, c->cap * sizeof *c->v);
        }
        e = &c->v[c->n++];
        memset(e, 0, sizeof *e);
        e->path = xstrdup(abs);
    }
    e->dev = (unsigned long long)st->st_dev;
    e->ino = (unsigned long long)st->st_ino;
    e->size = (unsigned long long)st->st_size;
    e->mtime = (long long)st->st_mtime;
    memcpy(e->hash, hex, 65);
    if (!same) sort_(c->v, c->n, sizeof *c->v, ce_by_path);   /* keep it searchable */
    return 0;
}

/* ------------------------------------------------------------------ a folder's files */

typedef struct {
    mlist files;
    size_t links, unreadable, silent;
    strlist silent_paths;
} scan;

static void walk(const char *root, const char *rel, hcache *c, int deep, scan *s)
{
    char *dir = *rel ? join(root, rel) : xstrdup(root);
    DIR *d = opendir(dir);
    if (!d) {
        s->unreadable++;
        free(dir);
        return;
    }
    strlist names = { 0 };
    for (struct dirent *e; (e = readdir(d));)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) strlist_add(&names, e->d_name);
    closedir(d);
    for (size_t i = 0; i < names.n; i++) {
        if (!*rel && !strcmp(names.v[i], ".arv")) continue;     /* arv's own */
        char *r = *rel ? xprintf("%s/%s", rel, names.v[i]) : xstrdup(names.v[i]), *abs = join(root, r);
        struct stat st;
        if (lstat(abs, &st)) s->unreadable++;
        else if (S_ISLNK(st.st_mode)) s->links++;
        else if (S_ISDIR(st.st_mode)) walk(root, r, c, deep, s);
        else if (S_ISREG(st.st_mode)) {
            char hex[65];
            int silent;
            if (cached_hash(c, abs, &st, deep, hex, &silent)) s->unreadable++;
            else {
                ml_add(&s->files, r, hex);
                if (silent) {
                    s->silent++;
                    strlist_add(&s->silent_paths, r);
                }
            }
        }
        free(r);
        free(abs);
    }
    strlist_free(&names);
    free(dir);
}

/* arv make has just hashed these files: remember them, so arv status need not read them again */
void hash_cache_note(const arv_home *h, const entries *files)
{
    hcache c;
    cache_load(&c, h);
    for (size_t i = 0; i < files->n; i++) {
        const entry *f = &files->v[i];
        struct stat st;
        char *abs = f->source ? realpath(f->source, NULL) : NULL;
        if (!abs || *f->link || lstat(abs, &st) || !S_ISREG(st.st_mode)) {
            free(abs);
            continue;
        }
        centry key;
        key.path = abs;
        centry *e = c.n ? bsearch(&key, c.v, c.n, sizeof *c.v, ce_by_path) : NULL;
        if (!e) {
            if (c.n == c.cap) {
                c.cap = c.cap ? c.cap * 2 : 1024;
                c.v = xrealloc(c.v, c.cap * sizeof *c.v);
            }
            e = &c.v[c.n++];
            memset(e, 0, sizeof *e);
            e->path = xstrdup(abs);
            sort_(c.v, c.n, sizeof *c.v, ce_by_path);
            e = bsearch(&key, c.v, c.n, sizeof *c.v, ce_by_path);
        }
        e->dev = (unsigned long long)st.st_dev;
        e->ino = (unsigned long long)st.st_ino;
        e->size = (unsigned long long)st.st_size;
        e->mtime = (long long)st.st_mtime;
        memcpy(e->hash, f->sha256, 65);
        free(abs);
    }
    cache_save(&c, h);
    for (size_t i = 0; i < c.n; i++) free(c.v[i].path);
    free(c.v);
    free(c.file);
}

static void scan_folder(const char *root, const arv_home *h, int deep, scan *s)
{
    memset(s, 0, sizeof *s);
    hcache c;
    cache_load(&c, h);
    walk(root, "", &c, deep, s);
    sort_(s->files.v, s->files.n, sizeof *s->files.v, ml_by_path);
    cache_save(&c, h);
    fprintf(stderr, "%zu files: %zu read, %zu unchanged since last read (hash cache)%s\n", s->files.n, c.read, c.cached,
            deep ? "; --deep: all read" : "");
    for (size_t i = 0; i < c.n; i++) free(c.v[i].path);
    free(c.v);
    free(c.file);
}

/* ------------------------------------------------------------------ comparing two manifests */

static int by_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

typedef struct {
    strlist added, changed, removed, moved;     /* moved: "OLD -> NEW" */
} mdiff;

static void diff_manifests(mlist *before, mlist *after, mdiff *d)
{
    memset(d, 0, sizeof *d);
    sort_(before->v, before->n, sizeof *before->v, ml_by_path);
    sort_(after->v, after->n, sizeof *after->v, ml_by_path);
    mlist gone = { 0 }, came = { 0 };
    size_t i = 0, k = 0;
    while (i < before->n || k < after->n) {
        int c = i == before->n ? 1 : k == after->n ? -1 : strcmp(before->v[i].path, after->v[k].path);
        if (c < 0) { ml_add(&gone, before->v[i].path, before->v[i].hash); i++; }
        else if (c > 0) { ml_add(&came, after->v[k].path, after->v[k].hash); k++; }
        else {
            if (strcmp(before->v[i].hash, after->v[k].hash)) strlist_add(&d->changed, after->v[k].path);
            i++;
            k++;
        }
    }
    /* a removed path whose content came back under a new path: moved */
    sort_(came.v, came.n, sizeof *came.v, ml_by_hash);
    char *used = calloc(came.n + 1, 1);
    for (size_t g = 0; g < gone.n; g++) {
        size_t lo = 0, hi = came.n, hit = (size_t)-1;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            int c = strcmp(came.v[mid].hash, gone.v[g].hash);
            if (c < 0) lo = mid + 1;
            else hi = mid;
        }
        for (size_t j = lo; j < came.n && !strcmp(came.v[j].hash, gone.v[g].hash); j++)
            if (!used[j]) { hit = j; break; }
        if (hit != (size_t)-1) {
            used[hit] = 1;
            char *m = xprintf("%s -> %s", gone.v[g].path, came.v[hit].path);
            strlist_add(&d->moved, m);
            free(m);
        } else {
            strlist_add(&d->removed, gone.v[g].path);
        }
    }
    for (size_t j = 0; j < came.n; j++)
        if (!used[j]) strlist_add(&d->added, came.v[j].path);
    free(used);
    ml_free(&gone);
    ml_free(&came);
    sort_(d->added.v, d->added.n, sizeof *d->added.v, by_str);
}

static void diff_free(mdiff *d);

/* "+NEW ~CHANGED -REMOVED >MOVED files" between two manifests' texts (a Revision's Changes) */
char *manifest_changes(const char *before, const char *after)
{
    mlist b = { 0 }, a = { 0 };
    ml_parse(&b, before);
    ml_parse(&a, after);
    mdiff d;
    diff_manifests(&b, &a, &d);
    char *out = xprintf("+%zu ~%zu -%zu >%zu files", d.added.n, d.changed.n, d.removed.n, d.moved.n);
    diff_free(&d);
    ml_free(&b);
    ml_free(&a);
    return out;
}

static void diff_print(const mdiff *d, int verbose)
{
    printf("  +%zu new, ~%zu changed, -%zu removed, >%zu moved\n", d->added.n, d->changed.n, d->removed.n, d->moved.n);
    const strlist *l[] = { &d->added, &d->changed, &d->removed, &d->moved };
    const char *mark[] = { "+", "~", "-", ">" };
    size_t shown = 0, limit = verbose ? (size_t)-1 : 20, total = 0;
    for (int k = 0; k < 4; k++) total += l[k]->n;
    for (int k = 0; k < 4; k++)
        for (size_t i = 0; i < l[k]->n && shown < limit; i++, shown++) printf("  %s %s\n", mark[k], l[k]->v[i]);
    if (shown < total) printf("  ... %zu more (-v for all)\n", total - shown);
}

static void diff_free(mdiff *d)
{
    strlist_free(&d->added);
    strlist_free(&d->changed);
    strlist_free(&d->removed);
    strlist_free(&d->moved);
}

/* ------------------------------------------------------------------ what a folder is */

/* the newest link event for this folder (arv link), or NULL */
static const rec_record *folder_link(const archive *cat, const char *abs)
{
    const rec_record *found = NULL;
    for (size_t i = 0; i < cat->events.n; i++) {
        const rec_record *e = cat->events.v[i];
        if (rec_get(e, "Folder") && !strcmp(rec_get(e, "Folder"), abs) && rec_get(e, "State")) found = e;
    }
    return found;
}

/* the collection a folder is the workflow folder of: its marker, else a declaration (arv link) */
rec_record *folder_collection(const archive *cat, const char *abs, const char **how)
{
    char *uuid = marker_collection(abs);
    if (uuid) {
        rec_record *c = archive_collection(cat, uuid);
        free(uuid);
        if (how) *how = "marker";
        return c;
    }
    const rec_record *l = folder_link(cat, abs);
    const char *obj = l ? rec_get(l, "Object") : NULL;
    if (obj && !strncmp(obj, "collection:", 11) && !strcmp(get_or(l, "State", ""), "present")) {
        if (how) *how = "declared";
        return archive_collection(cat, obj + 11);
    }
    return NULL;
}

/* the collection's workflow folder now: the newest folder linked to it as present, or NULL */
static const char *collection_folder(const archive *cat, const rec_record *c)
{
    char *want = xprintf("collection:%s", get_or(c, "Code", ""));
    const char *found = NULL;
    for (size_t i = 0; i < cat->events.n; i++) {
        const rec_record *e = cat->events.v[i];
        const char *f = rec_get(e, "Folder");
        if (f && !strcmp(get_or(e, "Object", ""), want) && folder_link(cat, f) == e
            && !strcmp(get_or(e, "State", ""), "present"))
            found = f;
    }
    free(want);
    return found;
}

static char *read_revision(const arv_home *h, const rec_record *r, mlist *out)
{
    memset(out, 0, sizeof *out);
    char *path = revision_manifest_path(h, rec_get(r, "Node"));
    char *text = access(path, F_OK) ? NULL : read_text(path);
    free(path);
    if (!text) return NULL;
    ml_parse(out, text);
    return text;
}

/* a revision by Node prefix (at least 6 hex digits) or CODE/N (edition N) */
static const rec_record *find_revision(const archive *cat, const char *spec)
{
    const char *slash = strchr(spec, '/');
    if (slash) {
        char *code = xstrdup(spec);
        code[slash - spec] = 0;
        const rec_record *c = archive_collection(cat, code);
        free(code);
        if (!c) return NULL;
        for (size_t i = 0; i < cat->revisions.n; i++) {
            const rec_record *r = cat->revisions.v[i];
            if (!strcmp(get_or(r, "Collection", ""), get_or(c, "Uuid", "")) && !strcmp(get_or(r, "Edition", ""), slash + 1))
                return r;
        }
        return NULL;
    }
    if (strlen(spec) < 6) return NULL;
    const rec_record *hit = NULL;
    for (size_t i = 0; i < cat->revisions.n; i++)
        if (!strncmp(get_or(cat->revisions.v[i], "Node", ""), spec, strlen(spec))) {
            if (hit) die("revision %s is ambiguous: give more digits", spec);
            hit = cat->revisions.v[i];
        }
    return hit;
}

static void describe_revision(const rec_record *r, sbuf *out)
{
    if (rec_get(r, "Edition")) sb_printf(out, "edition %s (%s)", rec_get(r, "Edition"), get_or(r, "Stage", ""));
    else sb_puts(out, "checkpoint");
    sb_printf(out, ", %s, revision %.12s", get_or(r, "Date", ""), get_or(r, "Node", ""));
}

static char *folder_arg(const char *given)
{
    char *abs = realpath(given ? given : ".", NULL);
    if (!abs) die("%s: no such folder", given ? given : ".");
    struct stat st;
    if (stat(abs, &st) || !S_ISDIR(st.st_mode)) die("%s is not a folder", abs);
    return abs;
}

/* records the folder's state as a checkpoint revision (arv checkpoint, arv status --record) */
static void record_checkpoint(const arv_home *h, archive *cat, const rec_record *coll, mlist *now, const char *message)
{
    char today[11], tree[65], node[65];
    today_iso(today);
    const char *uuid = rec_get(coll, "Uuid");
    const rec_record *head = collection_head(cat, uuid);
    const char *parent = head ? rec_get(head, "Node") : NULL;
    char *text = ml_text(now);
    revision_hashes(text, parent, today, message, tree, node);
    if (head && !strcmp(get_or(head, "Tree", ""), tree)) {
        printf("%s: nothing changed since %.12s; no checkpoint recorded\n", rec_get(coll, "Code"), parent);
        free(text);
        return;
    }
    char *before_text = NULL;
    if (head) {
        char *p = revision_manifest_path(h, parent);
        before_text = access(p, F_OK) ? NULL : read_text(p);
        free(p);
    }
    char *changes = manifest_changes(before_text ? before_text : "", text);
    rec_record *r = rec_alloc("Revision");
    rec_add(r, "Node", node);
    rec_add(r, "Collection", uuid);
    rec_add(r, "Tree", tree);
    if (parent) rec_add(r, "Parent", parent);
    rec_add(r, "Date", today);
    rec_add(r, "Stage", "checkpoint");
    rec_add(r, "Changes", changes);
    if (message && *message) rec_add(r, "Message", message);
    recs_add(&cat->revisions, r);
    char *path = revision_manifest_path(h, node), *dir = xstrdup(path);
    *strrchr(dir, '/') = 0;
    if (mkdirs(dir)) die("cannot create %s", dir);
    write_text(path, text);
    archive_save(cat, h->rec_path);
    printf("%s: checkpoint %.12s, %s\n", rec_get(coll, "Code"), node, changes);
    free(path); free(dir); free(text); free(changes); free(before_text);
}

/* a folder no collection claims: which of its files are on discs, and which discs it resembles */
static void tracked_status(const arv_home *h, const archive *cat, const scan *s, int verbose, int hint)
{
    mlist all = { 0 };          /* every disc's manifest: hash, then the disc id in path */
    size_t *disc_total = xmalloc((cat->discs.n + 1) * sizeof *disc_total), *disc_here = xmalloc((cat->discs.n + 1) * sizeof *disc_here);
    for (size_t i = 0; i < cat->discs.n; i++) {
        const char *id = get_or(cat->discs.v[i], "Id", "");
        char *p = home_volume_file(h, id, "manifest.sha256");
        char *text = access(p, F_OK) ? NULL : read_text(p);
        mlist one = { 0 };
        ml_parse(&one, text);
        disc_total[i] = one.n;
        disc_here[i] = 0;
        for (size_t k = 0; k < one.n; k++) ml_add(&all, id, one.v[k].hash);
        ml_free(&one);
        free(text);
        free(p);
    }
    sort_(all.v, all.n, sizeof *all.v, ml_by_hash);
    size_t archived = 0;
    strlist missing = { 0 };
    for (size_t f = 0; f < s->files.n; f++) {
        size_t lo = 0, hi = all.n;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (strcmp(all.v[mid].hash, s->files.v[f].hash) < 0) lo = mid + 1;
            else hi = mid;
        }
        int any = 0;
        for (size_t j = lo; j < all.n && !strcmp(all.v[j].hash, s->files.v[f].hash); j++) {
            if (j > lo && !strcmp(all.v[j].path, all.v[j - 1].path)) continue;   /* one count per disc */
            any = 1;
            for (size_t i = 0; i < cat->discs.n; i++)
                if (!strcmp(get_or(cat->discs.v[i], "Id", ""), all.v[j].path)) disc_here[i]++;
        }
        if (any) archived++;
        else strlist_add(&missing, s->files.v[f].path);
    }
    printf("  %zu of its %zu files are on discs; %zu on none\n", archived, s->files.n, missing.n);
    for (int shown = 0; shown < 3; shown++) {          /* the discs it most resembles */
        size_t best = (size_t)-1;
        for (size_t i = 0; i < cat->discs.n; i++)
            if (disc_here[i] && (best == (size_t)-1 || disc_here[i] > disc_here[best])) best = i;
        if (best == (size_t)-1) break;
        size_t pct = disc_total[best] ? disc_here[best] * 100 / disc_total[best] : 0;
        printf("  %s %s: %zu%% of its files are here (%zu of %zu)\n", shown ? "also" : "most like", get_or(cat->discs.v[best], "Id", ""),
               pct, disc_here[best], disc_total[best]);
        disc_here[best] = 0;
    }
    size_t limit = verbose ? missing.n : missing.n < 20 ? missing.n : 20;
    for (size_t i = 0; i < limit; i++) printf("  + %s\n", missing.v[i]);
    if (limit < missing.n) printf("  ... %zu more on no disc (-v for all)\n", missing.n - limit);
    if (missing.n && hint) puts("  (arv link FOLDER COLLECTION|DISC-ID says what it is; arv collection init makes it a collection)");
    strlist_free(&missing);
    ml_free(&all);
    free(disc_total);
    free(disc_here);
}

int cmd_status(int argc, char **argv)
{
    const char *given = NULL, *folder = NULL, *message = NULL;
    int verbose = 0, deep = 0, record = 0;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--deep")) deep = 1;
        else if (!strcmp(argv[i], "--record")) record = 1;
        else if (i + 1 < argc && !strcmp(argv[i], "--message")) message = argv[++i];
        else if (argv[i][0] != '-' && !folder) folder = argv[i];
        else return 2;
    }
    char *abs = folder_arg(folder);
    arv_home h;
    home_find(&h, given, abs);
    archive cat;
    archive_load(&cat, h.rec_path);
    const char *how = NULL;
    rec_record *coll = folder_collection(&cat, abs, &how);
    const rec_record *link = folder_link(&cat, abs);
    if (!coll && marker_collection(abs)) die("%s", "this folder's .arv marker names a collection that is not in the home catalogue");
    scan s;
    scan_folder(abs, &h, deep, &s);
    if (coll) {
        printf("%s (%s): workflow folder %s (%s)\n", rec_get(coll, "Code"), get_or(coll, "Title", ""), abs, how);
        const rec_record *head = collection_head(&cat, rec_get(coll, "Uuid"));
        if (!head) {
            printf("  no revision yet: %zu files (arv make for its first edition, arv checkpoint to record it)\n", s.files.n);
        } else {
            sbuf what = { 0 };
            describe_revision(head, &what);
            printf("  since %s:\n", what.s);
            free(what.s);
            mlist before;
            char *text = read_revision(&h, head, &before);
            if (!text) printf("  (its manifest is not in %s/revisions: nothing to compare with)\n", h.catalog_dir);
            else {
                mdiff d;
                diff_manifests(&before, &s.files, &d);
                if (!d.added.n && !d.changed.n && !d.removed.n && !d.moved.n) puts("  nothing changed");
                else diff_print(&d, verbose);
                diff_free(&d);
            }
            ml_free(&before);
            free(text);
        }
    } else if (link && !strcmp(get_or(link, "State", ""), "past")) {
        printf("%s: kept for reference, an older state of %s (linked %s)\n", abs,
               get_or(link, "Object", get_or(link, "Disc", "?")), get_or(link, "Date", ""));
        tracked_status(&h, &cat, &s, verbose, 0);
    } else if (link && rec_get(link, "Disc")) {
        printf("%s: the source of disc %s (declared %s)\n", abs, rec_get(link, "Disc"), get_or(link, "Date", ""));
        tracked_status(&h, &cat, &s, verbose, 0);
    } else {
        printf("%s: not a collection's workflow folder\n", abs);
        tracked_status(&h, &cat, &s, verbose, 1);
    }
    if (s.links || s.unreadable) printf("  (%zu symbolic links not compared, %zu unreadable)\n", s.links, s.unreadable);
    for (size_t i = 0; i < s.silent_paths.n; i++)
        printf("  ! %s: content changed but its modified time did not (bit rot, or a tool that keeps times)\n",
               s.silent_paths.v[i]);
    if (record) {
        if (!coll) die("%s", "--record: only a collection's workflow folder has revisions");
        record_checkpoint(&h, &cat, coll, &s.files, message);
    }
    ml_free(&s.files);
    strlist_free(&s.silent_paths);
    free(abs);
    return 0;
}

int cmd_checkpoint(int argc, char **argv)
{
    char **args = xmalloc(((size_t)argc + 2) * sizeof *args);
    for (int i = 0; i < argc; i++) args[i] = argv[i];
    args[argc] = "--record";
    args[argc + 1] = NULL;
    return cmd_status(argc + 1, args);
}

int cmd_log(int argc, char **argv)
{
    const char *given = NULL, *what = NULL;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!what) what = argv[i];
        else return 2;
    }
    char *abs = !what ? folder_arg(NULL) : NULL;
    arv_home h;
    home_find(&h, given, abs);
    archive cat;
    archive_load(&cat, h.rec_path);
    const rec_record *c = what ? archive_collection(&cat, what) : folder_collection(&cat, abs, NULL);
    if (!c) die("no collection %s (give a collection code, or run it in a workflow folder)", what ? what : abs);
    for (size_t i = 0; i < cat.revisions.n; i++) {
        const rec_record *r = cat.revisions.v[i];
        if (strcmp(get_or(r, "Collection", ""), get_or(c, "Uuid", ""))) continue;
        sbuf line = { 0 };
        describe_revision(r, &line);
        printf("%s/%s  %s\n", get_or(c, "Code", ""), rec_get(r, "Edition") ? rec_get(r, "Edition") : "-", line.s);
        printf("    %s", get_or(r, "Changes", ""));
        for (size_t f = 0; f < r->nfields; f++)
            if (!strcmp(r->fields[f].name, "Volume")) printf("  %s", r->fields[f].value);
        putchar('\n');
        if (rec_get(r, "Message")) printf("    %s\n", rec_get(r, "Message"));
        free(line.s);
    }
    free(abs);
    return 0;
}

int cmd_diff(int argc, char **argv)
{
    const char *given = NULL, *a = NULL, *b = NULL;
    int verbose = 0;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!a) a = argv[i];
        else if (!b) b = argv[i];
        else return 2;
    }
    if (!a) return 2;
    struct stat st;
    char *abs = b && !stat(b, &st) && S_ISDIR(st.st_mode) ? folder_arg(b) : NULL;
    if (abs) b = NULL;
    arv_home h;
    home_find(&h, given, abs);
    archive cat;
    archive_load(&cat, h.rec_path);
    const rec_record *ra = find_revision(&cat, a), *rb = b ? find_revision(&cat, b) : NULL;
    if (!ra) die("no revision %s (a Node prefix of 6+ digits, or CODE/N for edition N)", a);
    if (b && !rb) die("no revision %s (a Node prefix of 6+ digits, or CODE/N for edition N)", b);
    if (!b && !abs) {                    /* to the collection's workflow folder as it is now */
        const rec_record *c = archive_collection(&cat, get_or(ra, "Collection", ""));
        const char *f = c ? collection_folder(&cat, c) : NULL;
        if (!f) die("%s", "which folder? arv diff REV FOLDER (the collection has no workflow folder on record)");
        abs = folder_arg(f);
    }
    mlist before, after = { 0 };
    char *ta = read_revision(&h, ra, &before), *tb = NULL;
    if (!ta) die("the manifest of revision %s is not in the home catalogue", get_or(ra, "Node", ""));
    sbuf head = { 0 };
    describe_revision(ra, &head);
    if (rb) {
        tb = read_revision(&h, rb, &after);
        if (!tb) die("the manifest of revision %s is not in the home catalogue", get_or(rb, "Node", ""));
        sb_puts(&head, " -> ");
        describe_revision(rb, &head);
    } else {
        scan s;
        scan_folder(abs, &h, 0, &s);
        after = s.files;
        sb_printf(&head, " -> %s now", abs);
        strlist_free(&s.silent_paths);
    }
    puts(head.s);
    mdiff d;
    diff_manifests(&before, &after, &d);
    diff_print(&d, verbose);
    diff_free(&d);
    ml_free(&before);
    ml_free(&after);
    free(ta); free(tb); free(head.s); free(abs);
    return 0;
}

int cmd_link(int argc, char **argv)
{
    const char *given = NULL, *folder = NULL, *target = NULL;
    int past = 0;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!strcmp(argv[i], "--past")) past = 1;
        else if (argv[i][0] != '-' && !folder) folder = argv[i];
        else if (argv[i][0] != '-' && !target) target = argv[i];
        else return 2;
    }
    if (!folder || !target) return 2;
    char *abs = folder_arg(folder);
    arv_home h;
    home_find(&h, given, abs);
    archive cat;
    archive_load(&cat, h.rec_path);
    const rec_record *c = archive_collection(&cat, target);
    const rec_record *d = c ? NULL : archive_disc(&cat, target);
    if (!c && !d) die("no collection or disc %s in the catalogue", target);
    char *mine = marker_collection(abs);
    if (mine && c && !strcmp(mine, get_or(c, "Uuid", "")) && !past)
        die("%s", "this folder's .arv marker already makes it that collection's workflow folder");
    free(mine);
    if (!past && c) {                                /* one present workflow folder per collection */
        char *want = xprintf("collection:%s", rec_get(c, "Code"));
        for (size_t i = 0; i < cat.events.n; i++) {
            const rec_record *e = cat.events.v[i];
            const char *f = rec_get(e, "Folder");
            if (!f || !strcmp(f, abs) || strcmp(get_or(e, "Object", ""), want)) continue;
            if (folder_link(&cat, f) == e && !strcmp(get_or(e, "State", ""), "present"))
                fprintf(stderr, "Note: %s is %s's workflow folder too (%s); two folders for one collection branch its "
                                "history: link the older one --past\n", f, rec_get(c, "Code"), get_or(e, "How", "declared"));
        }
        free(want);
    }
    char *who = person(), *obj = c ? xprintf("collection:%s", rec_get(c, "Code")) : xstrdup(rec_get(d, "Id"));
    char *note = past ? xprintf("%s declared an older state of %s, kept for reference", abs, obj)
                      : xprintf("%s declared %s", abs, c ? "the collection's workflow folder" : "the source of this disc");
    rec_record *e = new_event(obj, "accession", "success", who, "human", note);
    if (c) {
        free(e->fields[0].name);
        e->fields[0].name = xstrdup("Object");
    }
    rec_add(e, "Folder", abs);
    rec_add(e, "How", "declared");
    rec_add(e, "State", past ? "past" : "present");
    recs_add(&cat.events, e);
    archive_save(&cat, h.rec_path);
    printf("%s: %s %s (logged)\n", abs, past ? "an older state of" : "linked to", obj);
    free(who); free(obj); free(note); free(abs);
    return 0;
}
