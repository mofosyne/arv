/* Disc plans: discs composed by hand from files and folders anywhere (the mastering workspace).
 *
 *   arv plan new NAME [--medium M] [--set CODE] [--title TEXT] [--description TEXT] [--access LEVEL] [--discs N]
 *   arv plan list [--all]           open plans (--all: made ones too)
 *   arv plan show NAME [--json]
 *   arv plan add NAME SOURCE... [--disc N|new|auto] [--as PATH]
 *   arv plan move NAME PATH... --disc N|new [--from N]
 *   arv plan drop NAME PATH... [--from N]
 *   arv plan disc NAME add | drop N
 *   arv plan make NAME [arv make's options]
 *   arv plan again NAME NEW         a new open plan from NAME (made or not): its settings, discs and items
 *   arv plan delete NAME            (always safe for a made plan: its discs and objects keep all it said)
 * --medium, --set, --title, --description and --access given to new or to any other action (but
 * make, where they are arv make's) are kept in the plan as make's defaults.
 *
 * A plan says which file or folder goes on which disc, and where under data/; it copies nothing.
 * The sources are read where they are when the discs are made (arv make --plan). It is kept in the
 * home's drafts/plans/NAME.rec, a recfile: a Plan record (its name, discs and the defaults make
 * takes), then an Item record for each thing on a disc (Disc, Source, Path). Sizes are measured
 * when shown, never stored, so a plan always shows its sources as they are now.
 */
#define _XOPEN_SOURCE 700
#include "arv.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SECTOR 2048
#define TOOLS_RESERVE 65536L        /* sectors kept for tools/ (arv's source and programs): about 128 MiB */

/* ------------------------------------------------------------------ the plan file */

static char *plans_dir(const arv_home *h)
{
    return join(h->drafts_dir, "plans");
}

static int name_ok(const char *name)
{
    if (!*name || name[0] == '.' || strlen(name) > 64) return 0;
    for (const char *c = name; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_' && *c != '.') return 0;
    return 1;
}

static char *plan_file(const arv_home *h, const char *name)
{
    if (!name_ok(name)) die("%s is not a plan name (letters, digits, - _ and .; not starting with .)", name);
    char *dir = plans_dir(h), *file = xprintf("%s/%s.rec", dir, name);
    free(dir);
    return file;
}

/* what a plan's paths are relative to: the folder holding its home (<home>/drafts/plans/NAME.rec),
 * or the folder the plan file is in when it is kept elsewhere */
static char *plan_root(const char *file)
{
    char *parent = xstrdup(file), *slash = strrchr(parent, '/');
    if (slash) *slash = 0;
    else {
        free(parent);
        parent = xstrdup(".");
    }
    char *dir = realpath(parent, NULL);         /* absolute: the sources are */
    if (!dir) dir = abs_path(parent);
    free(parent);
    size_t n = strlen(dir);
    if (n > 13 && !strcmp(dir + n - 13, "/drafts/plans")) {
        dir[n - 13] = 0;                    /* the home */
        char *up = strrchr(dir, '/');
        if (up && up != dir) *up = 0;
        else if (up) up[1] = 0;
    }
    return dir;
}

static rec_record *head(disc_plan *p)
{
    for (size_t i = 0; i < p->rec.nrecords; i++)
        if (!p->rec.records[i].descriptor && p->rec.records[i].type && !strcmp(p->rec.records[i].type, "Plan"))
            return &p->rec.records[i];
    die("%s has no Plan record", p->file);
    return NULL;
}

void plan_load(const char *file, disc_plan *out)
{
    memset(out, 0, sizeof *out);
    int bad = 0;
    if (rec_read(file, &out->rec, &bad)) {
        if (errno == ENOENT) die("no plan %s (arv plan list)", file);
        char n[16];
        snprintf(n, sizeof n, "%d", bad);
        die2("cannot read the plan %s (line %s)", file, n);
    }
    out->file = xstrdup(file);
    out->root = plan_root(file);
    const rec_record *hd = head(out);
    out->name = xstrdup(rec_get(hd, "Name") ? rec_get(hd, "Name") : "plan");
    out->discs = rec_get(hd, "Discs") ? atoi(rec_get(hd, "Discs")) : 0;
    out->v = xmalloc((out->rec.nrecords + 1) * sizeof *out->v);
    for (size_t i = 0; i < out->rec.nrecords; i++) {
        const rec_record *r = &out->rec.records[i];
        if (r->descriptor || !r->type || strcmp(r->type, "Item")) continue;
        plan_item *it = &out->v[out->n++];
        it->disc = rec_get(r, "Disc") ? atoi(rec_get(r, "Disc")) : 0;
        it->source = rec_get(r, "Source") && *rec_get(r, "Source") ? path_abs(out->root, rec_get(r, "Source")) : xstrdup("");
        it->path = xstrdup(rec_get(r, "Path") ? rec_get(r, "Path") : "");
        it->origin = rec_get(r, "Origin") ? path_abs(out->root, rec_get(r, "Origin")) : NULL;
        it->seen = rec_get(r, "Seen") ? xstrdup(rec_get(r, "Seen")) : NULL;
        if (it->disc < 1 || it->disc > out->discs || !*it->source || !*it->path)
            die("%s: an Item needs a Disc between 1 and the plan's Discs, a Source and a Path", file);
    }
}

static int by_disc_path(const void *a, const void *b)
{
    const plan_item *x = a, *y = b;
    return x->disc != y->disc ? (x->disc < y->disc ? -1 : 1) : strcmp(x->path, y->path);
}

/* writes the plan back: its Plan record (Discs updated), then its items by disc and path */
static void plan_save(disc_plan *p)
{
    rec_record *hd = head(p);
    char n[16];
    snprintf(n, sizeof n, "%d", p->discs);
    rec_set(hd, "Discs", n);
    if (p->n) qsort(p->v, p->n, sizeof *p->v, by_disc_path);
    sbuf b = { 0 };
    char *buf = NULL;
    size_t len = 0;
    rec_format(hd, &buf, &len);
    sb_puts(&b, "%rec: Plan\n\n");
    sb_add(&b, buf, len);
    free(buf);
    sb_puts(&b, "\n");
    for (size_t i = 0; i < p->n; i++) {
        if (!i) sb_puts(&b, "\n%rec: Item\n");
        char *src = path_rel(p->root, p->v[i].source);
        sb_printf(&b, "\nDisc: %d\nSource: %s\nPath: %s\n", p->v[i].disc, src, p->v[i].path);
        free(src);
        if (p->v[i].origin) {
            char *o = path_rel(p->root, p->v[i].origin);
            sb_printf(&b, "Origin: %s\n", o);
            free(o);
        }
        if (p->v[i].seen) sb_printf(&b, "Seen: %s\n", p->v[i].seen);
    }
    char *tmp = xprintf("%s.tmp", p->file);
    write_text(tmp, b.s);
    if (rename(tmp, p->file)) die("cannot write %s", p->file);
    free(tmp);
    free(b.s);
}

void plan_made(const char *file, const strlist *disc_ids, const char *date)
{
    disc_plan p;
    plan_load(file, &p);
    rec_record *hd = head(&p);
    rec_add(hd, "Made", date);
    for (size_t i = 0; i < disc_ids->n; i++) rec_add(hd, "Volume", disc_ids->v[i]);
    plan_save(&p);
}

/* ------------------------------------------------------------------ the files a plan puts on its discs */

void plan_scan(const disc_plan *p, const char *links, entries *files, entries *noted, size_t *left_out)
{
    memset(files, 0, sizeof *files);
    memset(noted, 0, sizeof *noted);
    size_t fcap = 0, ncap = 0;
    for (size_t i = 0; i < p->n; i++) {
        const plan_item *it = &p->v[i];
        struct stat st;
        if (stat(it->source, &st)) die("%s is gone (it is in the plan: arv plan drop, or put it back)", it->source);
        entries f = { 0 }, n = { 0 };
        if (S_ISDIR(st.st_mode)) {
            scan_payload(it->source, links, &f, &n);
        } else {
            f.v = xmalloc(sizeof *f.v);
            scan_file(it->source, "", &f.v[0]);
            f.n = 1;
        }
        const entries *both[2] = { &f, &n };
        for (int k = 0; k < 2; k++)
            for (size_t j = 0; j < both[k]->n; j++) {
                entry e = both[k]->v[j];
                if (S_ISDIR(st.st_mode) && (!strcmp(e.path, ".arv") || !strncmp(e.path, ".arv/", 5))) {
                    if (!k) (*left_out)++;          /* a folder's own .arv: arv's, never content */
                    continue;
                }
                char *dest = !*e.path ? xstrdup(it->path) : !strcmp(it->path, ".") ? xstrdup(e.path) : join(it->path, e.path);
                free(e.path);
                e.path = dest;
                e.bin = it->disc - 1;
                entries *to = k ? noted : files;
                size_t *cap = k ? &ncap : &fcap;
                if (to->n == *cap) {
                    *cap = *cap ? 2 * *cap : 64;
                    to->v = xrealloc(to->v, (*cap + 1) * sizeof *to->v);
                }
                to->v[to->n++] = e;
            }
        free(f.v);
        free(n.v);
    }
    if (!files->v) files->v = xmalloc(sizeof *files->v);
    if (!noted->v) noted->v = xmalloc(sizeof *noted->v);
}

/* ------------------------------------------------------------------ sizes */

typedef struct {
    uint64_t bytes;
    long files, sectors;
    int missing, folder;
} measure;

static void walk_size(const char *disk, const char *rel, int top, measure *m)
{
    struct stat st;
    if (lstat(disk, &st)) return;
    if (S_ISREG(st.st_mode)) {
        m->bytes += (uint64_t)st.st_size;
        m->files++;
        m->sectors += file_sectors((uint64_t)st.st_size, rel);
        return;
    }
    if (!S_ISDIR(st.st_mode)) return;           /* links: arv make's --links decides; not counted */
    DIR *d = opendir(disk);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (top && !strcmp(e->d_name, ".arv"))) continue;
        char *p = join(disk, e->d_name), *r = *rel ? join(rel, e->d_name) : xstrdup(e->d_name);
        walk_size(p, r, 0, m);
        free(p);
        free(r);
    }
    if (d) closedir(d);
    m->sectors += 1;
}

static measure item_size(const plan_item *it)
{
    measure m = { 0 };
    struct stat st;
    if (stat(it->source, &st)) {
        m.missing = 1;
        return m;
    }
    m.folder = S_ISDIR(st.st_mode);
    if (m.folder) walk_size(it->source, strcmp(it->path, ".") ? it->path : "", 1, &m);
    else {
        m.bytes = (uint64_t)st.st_size;
        m.files = 1;
        m.sectors = file_sectors((uint64_t)st.st_size, it->path);
    }
    return m;
}

static long tree_sectors(const char *path)
{
    measure m = { 0 };
    walk_size(path, "", 0, &m);
    return m.sectors;
}

/* the sectors a disc has for the plan's files: the medium at 20% RS03, less about what tools/, the
 * catalogue snapshot and the tag files take (arv make measures exactly; this is for planning) */
static long room(const arv_home *h, const rec_record *hd, const char **label)
{
    long budget = medium_budget(rec_get(hd, "Medium") ? rec_get(hd, "Medium") : "bd25", label);
    if (!budget) return 0;
    long reserve = TOOLS_RESERVE + 2 * tree_sectors(h->catalog_dir) + budget / 200 + (1024 < budget / 20 ? 1024 : budget / 20);
    return budget - reserve;
}

/* ------------------------------------------------------------------ what a source looked like when planned */

static void stamp_walk(const char *disk, const char *rel, int top, strlist *lines, long *files, uint64_t *bytes)
{
    struct stat st;
    if (lstat(disk, &st)) return;
    if (S_ISREG(st.st_mode)) {
        char *line = xprintf("%s\t%llu\t%lld", rel, (unsigned long long)st.st_size, (long long)st.st_mtime);
        strlist_add(lines, line);
        free(line);
        (*files)++;
        *bytes += (uint64_t)st.st_size;
        return;
    }
    if (!S_ISDIR(st.st_mode)) return;
    DIR *d = opendir(disk);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (top && !strcmp(e->d_name, ".arv"))) continue;
        char *p = join(disk, e->d_name), *r = *rel ? join(rel, e->d_name) : xstrdup(e->d_name);
        stamp_walk(p, r, 0, lines, files, bytes);
        free(p);
        free(r);
    }
    if (d) closedir(d);
}

static int by_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* "FILES BYTES STAMP": every file's name, size and modified time, hashed (contents are not read,
 * so it is quick); NULL when the source is gone */
static char *stamp_of(const char *source)
{
    struct stat st;
    if (stat(source, &st)) return NULL;
    strlist lines = { 0 };
    long files = 0;
    uint64_t bytes = 0;
    stamp_walk(source, "", 1, &lines, &files, &bytes);
    if (lines.n) qsort(lines.v, lines.n, sizeof *lines.v, by_str);
    sbuf b = { 0 };
    sb_puts(&b, "");
    for (size_t i = 0; i < lines.n; i++) sb_printf(&b, "%s\n", lines.v[i]);
    char hex[65];
    text_sha256(b.s, hex);
    free(b.s);
    strlist_free(&lines);
    return xprintf("%ld %llu %.16s", files, (unsigned long long)bytes, hex);
}

/* changed since it was planned (or copied): its names, sizes or dates differ */
static int changed_since(const plan_item *it)
{
    if (!it->seen) return 0;
    char *now = stamp_of(it->source);
    int changed = now && strcmp(now, it->seen);
    free(now);
    return changed;
}

/* a copy that keeps what arv records: modified times and permissions; links stay links */
static void copy_keep(const char *from, const char *to)
{
    struct stat st;
    if (lstat(from, &st)) die("cannot read %s", from);
    if (S_ISLNK(st.st_mode)) {
        char target[4096];
        ssize_t n = readlink(from, target, sizeof target - 1);
        if (n < 0) die("cannot read %s", from);
        target[n] = 0;
        if (symlink(target, to)) die("cannot write %s", to);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(to, 0755) && errno != EEXIST) die("cannot create %s", to);
        DIR *d = opendir(from);
        struct dirent *e;
        if (!d) die("cannot read %s", from);
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char *a = join(from, e->d_name), *b = join(to, e->d_name);
            copy_keep(a, b);
            free(a);
            free(b);
        }
        closedir(d);
    } else if (S_ISREG(st.st_mode)) {
        copy_file(from, to);
    } else {
        return;                 /* devices and sockets: never content */
    }
    chmod(to, st.st_mode & 07777);
    struct timespec t[2] = { st.st_atim, st.st_mtim };
    utimensat(AT_FDCWD, to, t, 0);
}

/* where a plan keeps its copies (--copy): a folder beside the plan file, named after it */
static char *copies_dir(const disc_plan *p)
{
    char *f = xstrdup(p->file), *slash = strrchr(f, '/'), *name = slash ? slash + 1 : f;
    if (slash) *slash = 0;
    char *dir = realpath(slash ? f : ".", NULL);
    if (!dir) dir = abs_path(slash ? f : ".");
    size_t n = strlen(name);
    if (n > 4 && !strcmp(name + n - 4, ".rec")) name[n - 4] = 0;
    char *d = join(dir, name);
    free(dir);
    free(f);
    return d;
}

/* ------------------------------------------------------------------ showing it */

void json_str(sbuf *b, const char *s)
{
    sb_puts(b, "\"");
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c == '"' || *c == '\\') sb_printf(b, "\\%c", *c);
        else if (*c < 0x20) sb_printf(b, "\\u%04x", *c);
        else sb_add(b, (const char *)c, 1);
    }
    sb_puts(b, "\"");
}

static void bar(char out[23], long used, long cap)
{
    int filled = cap ? (int)((used * 20 + cap - 1) / cap) : 0;
    out[0] = '[';
    for (int i = 0; i < 20; i++) out[i + 1] = i < filled ? '#' : '-';
    out[21] = ']';
    out[22] = 0;
}

static void show(const arv_home *h, disc_plan *p, int as_json)
{
    rec_record *hd = head(p);
    const char *label;
    long cap = room(h, hd, &label);
    measure *m = xmalloc((p->n + 1) * sizeof *m);
    long *used = xmalloc(((size_t)p->discs + 1) * sizeof *used);
    uint64_t *bytes = xmalloc(((size_t)p->discs + 1) * sizeof *bytes);
    for (int d = 0; d <= p->discs; d++) used[d] = 0, bytes[d] = 0;
    int *chg = xmalloc((p->n + 1) * sizeof *chg), made = rec_get(hd, "Made") != NULL;
    size_t changed = 0;
    for (size_t i = 0; i < p->n; i++) {
        changed += (chg[i] = !made && changed_since(&p->v[i]));
        m[i] = item_size(&p->v[i]);
        used[p->v[i].disc] += m[i].sectors;
        bytes[p->v[i].disc] += m[i].bytes;
    }
    const char *fields[] = { "Title", "Set", "Medium", "Description", "Access", "From", NULL };
    if (as_json) {
        sbuf b = { 0 };
        sb_puts(&b, "{\"name\": ");
        json_str(&b, p->name);
        for (int k = 0; fields[k]; k++) {
            sb_printf(&b, ", \"%c%s\": ", tolower((unsigned char)fields[k][0]), fields[k] + 1);
            if (rec_get(hd, fields[k])) json_str(&b, rec_get(hd, fields[k]));
            else sb_puts(&b, "null");
        }
        sb_puts(&b, ", \"mediumLabel\": ");
        json_str(&b, label);
        sb_printf(&b, ", \"room\": %lld, \"made\": ", (long long)cap * SECTOR);
        if (rec_get(hd, "Made")) json_str(&b, rec_get(hd, "Made"));
        else sb_puts(&b, "null");
        sb_puts(&b, ", \"volumes\": [");
        int first = 1;
        for (size_t f = 0; f < hd->nfields; f++)
            if (!strcmp(hd->fields[f].name, "Volume")) {
                sb_puts(&b, first ? "" : ", ");
                json_str(&b, hd->fields[f].value);
                first = 0;
            }
        sb_puts(&b, "], \"discs\": [");
        for (int d = 1; d <= p->discs; d++) {
            sb_printf(&b, "%s{\"disc\": %d, \"bytes\": %llu, \"used\": %lld, \"items\": [", d > 1 ? ", " : "", d,
                      (unsigned long long)bytes[d], (long long)used[d] * SECTOR);
            int any = 0;
            for (size_t i = 0; i < p->n; i++) {
                if (p->v[i].disc != d) continue;
                sb_puts(&b, any ? ", {\"path\": " : "{\"path\": ");
                json_str(&b, p->v[i].path);
                sb_puts(&b, ", \"source\": ");
                json_str(&b, p->v[i].source);
                sb_puts(&b, ", \"origin\": ");
                if (p->v[i].origin) json_str(&b, p->v[i].origin);
                else sb_puts(&b, "null");
                sb_printf(&b, ", \"changed\": %s, \"kind\": \"%s\", \"bytes\": %llu, \"files\": %ld}", chg[i] ? "true" : "false",
                          m[i].missing ? "missing" : m[i].folder ? "folder" : "file", (unsigned long long)m[i].bytes, m[i].files);
                any = 1;
            }
            sb_puts(&b, "]}");
        }
        sb_puts(&b, "]}\n");
        fputs(b.s, stdout);
        free(b.s);
    } else {
        char rs[32];
        human_size((uint64_t)cap * SECTOR, rs);
        printf("Plan %s", p->name);
        if (rec_get(hd, "Title")) printf("  \"%s\"", rec_get(hd, "Title"));
        if (rec_get(hd, "Set")) printf("  set %s", rec_get(hd, "Set"));
        if (rec_get(hd, "From")) printf("  (from plan %s)", rec_get(hd, "From"));
        if (cap) printf("  %s: about %s a disc for files\n", label, rs);
        else printf("  medium auto: arv make chooses each disc's\n");
        size_t missing = 0;
        for (int d = 1; d <= p->discs; d++) {
            char size[32], b[23];
            human_size(bytes[d], size);
            bar(b, used[d], cap);
            size_t items = 0;
            for (size_t i = 0; i < p->n; i++) items += p->v[i].disc == d;
            if (!items) printf("Disc %d  %s    0%%  empty\n", d, cap ? b : "");
            else if (cap) printf("Disc %d  %s  %3ld%%  %s in %zu item%s%s\n", d, b, (used[d] * 100 + cap - 1) / cap, size, items,
                                 items == 1 ? "" : "s", used[d] > cap ? "  OVER: move something (arv plan move)" : "");
            else printf("Disc %d  %s in %zu item%s\n", d, size, items, items == 1 ? "" : "s");
            for (size_t i = 0; i < p->n; i++) {
                if (p->v[i].disc != d) continue;
                char sz[32];
                human_size(m[i].bytes, sz);
                if (m[i].missing) {
                    printf("  %s  MISSING  %s\n", p->v[i].path, p->v[i].source);
                    missing++;
                } else if (m[i].folder) {
                    printf("  %s/  %s, %ld file%s  %s", p->v[i].path, sz, m[i].files,
                           m[i].files == 1 ? "" : "s", p->v[i].origin ? "(the plan's copy)" : p->v[i].source);
                } else {
                    printf("  %s  %s  %s", p->v[i].path, sz, p->v[i].origin ? "(the plan's copy)" : p->v[i].source);
                }
                if (!m[i].missing) {
                    if (p->v[i].origin) printf("  of %s", p->v[i].origin);
                    if (chg[i]) printf("  CHANGED since planned");
                    putchar('\n');
                }
            }
        }
        if (rec_get(hd, "Made")) {
            printf("Made %s:", rec_get(hd, "Made"));
            for (size_t f = 0; f < hd->nfields; f++)
                if (!strcmp(hd->fields[f].name, "Volume")) printf(" %s", hd->fields[f].value);
            putchar('\n');
            printf("The same again, as a new plan: arv plan again %s NEW (this one is kept as it was; deleting it is safe)\n",
                   p->name);
        } else if (missing) {
            printf("%zu item%s missing: put %s back, or arv plan drop %s PATH\n", missing, missing == 1 ? " is" : "s are",
                   missing == 1 ? "it" : "them", p->name);
        } else if (p->n) {
            if (changed)
                printf("%zu item%s changed since planned: the discs get %s as %s now (arv plan refresh %s: that is fine)\n",
                       changed, changed == 1 ? "" : "s", changed == 1 ? "it" : "them", changed == 1 ? "it is" : "they are", p->name);
            printf("Next: arv plan make %s (one image a disc; arv make's options apply)\n", p->name);
        }
        if (!rec_get(hd, "Made") && p->n)
            puts("Planned, not archived: keep the originals until the discs are burned and read back (arv todo says when).");
    }
    free(chg);
    free(m);
    free(used);
    free(bytes);
}

/* ------------------------------------------------------------------ changing it */

static int path_ok(const char *path, int folder)
{
    if (!strcmp(path, ".")) return folder;
    if (!*path || path[0] == '/' || path[strlen(path) - 1] == '/') return 0;
    char *copy = xstrdup(path);
    int ok = 1;
    for (char *part = strtok(copy, "/"); part && ok; part = strtok(NULL, "/"))
        ok = strcmp(part, ".") && strcmp(part, "..") && strcmp(part, ".arv");
    ok &= !strstr(path, "//");
    free(copy);
    return ok;
}

static plan_item *find_item(disc_plan *p, const char *path, int from)
{
    plan_item *found = NULL;
    int count = 0;
    for (size_t i = 0; i < p->n; i++)
        if (!strcmp(p->v[i].path, path) && (!from || p->v[i].disc == from)) {
            found = &p->v[i];
            count++;
        }
    if (!found) die("nothing at %s in the plan (arv plan show)", path);
    if (count > 1) die("%s is on more than one disc: say which with --from N", path);
    return found;
}

static void check_free(const disc_plan *p, const char *path, int disc)
{
    for (size_t i = 0; i < p->n; i++)
        if (p->v[i].disc == disc && !strcmp(p->v[i].path, path)) {
            char n[16];
            snprintf(n, sizeof n, "%d", disc);
            die2("disc %s already has something at %s (--as another name)", n, path);
        }
}

/* --disc N|new|auto: the disc number (one past the last for new) */
static int pick_disc(const arv_home *h, disc_plan *p, const char *how, long need)
{
    if (!how || !strcmp(how, "auto")) {
        long cap = room(h, head(p), NULL);
        for (int d = 1; d <= p->discs; d++) {
            long used = 0;
            for (size_t i = 0; i < p->n; i++)
                if (p->v[i].disc == d) used += item_size(&p->v[i]).sectors;
            if (!cap || used + need <= cap) return d;
        }
        return p->discs + 1;
    }
    if (!strcmp(how, "new")) return p->discs + 1;
    char *end;
    long d = strtol(how, &end, 10);
    if (*end || d < 1 || d > p->discs + 1) die("--disc %s: a disc of the plan, new or auto (arv plan show)", how);
    return (int)d;
}

static void say_where(const arv_home *h, disc_plan *p, int disc, const char *what)
{
    long cap = room(h, head(p), NULL), used = 0;
    for (size_t i = 0; i < p->n; i++)
        if (p->v[i].disc == disc) used += item_size(&p->v[i]).sectors;
    if (cap) printf("%s: disc %d, now %ld%% full%s\n", what, disc, (used * 100 + cap - 1) / cap, used > cap ? " (OVER)" : "");
    else printf("%s: disc %d\n", what, disc);
}

/* ------------------------------------------------------------------ arv plan */

int cmd_plan(int argc, char **argv)
{
    const char *given = NULL, *action = NULL, *name = NULL, *disc = NULL, *as = NULL, *from_s = NULL;
    int as_json = 0, all = 0, copy = 0, yes = 0;
    strlist args = { 0 }, defaults = { 0 }, rest = { 0 };
    static const char *const SETTINGS[][2] = { { "--medium", "Medium" }, { "--set", "Set" }, { "--title", "Title" },
                                               { "--description", "Description" }, { "--access", "Access" } };
    int i = 0;
    for (; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!strcmp(argv[i], "--json")) as_json = 1;
        else if (!strcmp(argv[i], "--all")) all = 1;
        else if (!strcmp(argv[i], "--copy")) copy = 1;
        else if (!action) action = argv[i];
        else if (!name) {
            name = argv[i];
            if (!strcmp(action, "make")) {          /* the rest is arv make's */
                i++;
                break;
            }
        } else if (i + 1 < argc && !strcmp(argv[i], "--disc")) disc = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--as")) as = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--from")) from_s = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--discs")) disc = argv[++i];
        else if (!strcmp(argv[i], "--yes") || !strcmp(argv[i], "-y")) yes = 1;
        else {
            int setting = 0;
            for (int k = 0; k < 5 && !setting; k++)
                if (i + 1 < argc && !strcmp(argv[i], SETTINGS[k][0])) {
                    strlist_add(&defaults, SETTINGS[k][1]);
                    strlist_add(&defaults, argv[++i]);
                    setting = 1;
                }
            if (setting) continue;
            if (argv[i][0] == '-' && argv[i][1]) return 2;
            strlist_add(&args, argv[i]);
        }
    }
    for (; i < argc; i++) strlist_add(&rest, argv[i]);
    if (!action) return 2;
    arv_home h;
    home_find(&h, given, NULL);

    if (!strcmp(action, "list")) {
        char *dir = plans_dir(&h);
        DIR *d = opendir(dir);
        strlist names = { 0 };
        struct dirent *e;
        while (d && (e = readdir(d))) {
            size_t n = strlen(e->d_name);
            if (n > 4 && !strcmp(e->d_name + n - 4, ".rec") && e->d_name[0] != '.') strlist_add(&names, e->d_name);
        }
        if (d) closedir(d);
        if (!names.n) printf("No plans (arv plan new NAME)\n");
        size_t hidden = 0;
        for (size_t k = 0; k < names.n; k++)          /* in name order */
            for (size_t j = k + 1; j < names.n; j++)
                if (strcmp(names.v[j], names.v[k]) < 0) {
                    char *t = names.v[k];
                    names.v[k] = names.v[j];
                    names.v[j] = t;
                }
        for (size_t k = 0; k < names.n; k++) {
            char *f = join(dir, names.v[k]);
            disc_plan p;
            plan_load(f, &p);
            const rec_record *hd = head(&p);
            if (rec_get(hd, "Made") && !all) {
                hidden++;
                free(f);
                continue;
            }
            printf("%s  %d disc%s, %zu item%s%s%s%s\n", p.name, p.discs, p.discs == 1 ? "" : "s", p.n, p.n == 1 ? "" : "s",
                   rec_get(hd, "Title") ? "  \"" : "", rec_get(hd, "Title") ? rec_get(hd, "Title") : "",
                   rec_get(hd, "Title") ? "\"" : "");
            if (rec_get(hd, "Made")) printf("    made %s (arv plan again %s NEW: the same again, as a new plan)\n", rec_get(hd, "Made"), p.name);
            free(f);
        }
        if (hidden) printf("(%zu made plan%s not shown: arv plan list --all)\n", hidden, hidden == 1 ? "" : "s");
        free(dir);
        return 0;
    }
    if (!name) die("arv plan %s needs a plan name", action);
    char *file = plan_file(&h, name);

    if (!strcmp(action, "new")) {
        if (args.n) return 2;
        if (!access(file, F_OK)) die("there is a plan %s already (arv plan show)", name);
        char *dir = plans_dir(&h);
        if (mkdirs(dir)) die("cannot create %s", dir);
        free(dir);
        disc_plan p;
        memset(&p, 0, sizeof p);
        p.file = file;
        p.root = plan_root(file);
        p.name = xstrdup(name);
        p.discs = disc ? atoi(disc) : 1;
        if (p.discs < 1 || p.discs > 999) die("--discs %s: from 1 to 999", disc);
        rec_record *hd = rec_new(&p.rec, "Plan");
        char today[11];
        today_iso(today);
        rec_add(hd, "Name", name);
        rec_add(hd, "Created", today);
        rec_add(hd, "Medium", "bd25");
        for (size_t k = 0; k + 1 < defaults.n; k += 2) rec_set(hd, defaults.v[k], defaults.v[k + 1]);
        if (!medium_budget(rec_get(hd, "Medium"), NULL) && strcmp(rec_get(hd, "Medium"), "auto"))
            die("--medium %s: bd25, bd50, bd100, bd128 or auto", rec_get(hd, "Medium"));
        plan_save(&p);
        printf("Plan %s: %d empty disc%s (%s). Next: arv plan add %s FILE-OR-FOLDER...\n", name, p.discs, p.discs == 1 ? "" : "s",
               file, name);
        return 0;
    }
    if (!strcmp(action, "delete")) {
        if (args.n) return 2;
        if (access(file, F_OK)) die("no plan %s (arv plan list)", name);
        disc_plan p;
        plan_load(file, &p);
        char *copies = copies_dir(&p);
        if (!access(copies, F_OK) && !yes) {   /* its copies: the only ones until its discs are read back */
            const rec_record *hd = head(&p);
            archive cat;
            archive_load(&cat, h.rec_path);
            int safe = rec_get(hd, "Made") != NULL;
            for (size_t f = 0; f < hd->nfields; f++)
                if (!strcmp(hd->fields[f].name, "Volume") && !disc_read_back(&cat, hd->fields[f].value)) safe = 0;
            if (!safe)
                die2("plan %s holds copies (arv plan add --copy) that may be the only ones until its discs are burned and "
                     "read back (arv burned --device); arv plan delete %s --yes deletes them anyway", name, name);
        }
        int had_copies = !access(copies, F_OK);
        if (had_copies) remove_tree(copies);
        if (unlink(file)) die("cannot delete %s", file);
        printf("Plan %s deleted (%s)\n", name, had_copies ? "with its copies; the originals are untouched" : "its sources are untouched");
        free(copies);
        return 0;
    }

    disc_plan p;
    plan_load(file, &p);
    rec_record *hd = head(&p);
    if (!strcmp(action, "again")) {           /* a made plan as a template: the same selection, open again */
        if (args.n != 1) die("%s", "arv plan again NAME NEW: a new plan with NAME's settings, discs and items");
        char *to = plan_file(&h, args.v[0]);
        if (!access(to, F_OK)) die("there is a plan %s already (arv plan show)", args.v[0]);
        disc_plan q;
        memset(&q, 0, sizeof q);
        q.file = to;
        q.root = plan_root(to);
        q.name = xstrdup(args.v[0]);
        q.discs = p.discs;
        rec_record *qh = rec_new(&q.rec, "Plan");
        char today[11];
        today_iso(today);
        rec_add(qh, "Name", args.v[0]);
        rec_add(qh, "Created", today);
        rec_add(qh, "From", name);
        hd = head(&p);
        for (size_t f = 0; f < hd->nfields; f++) {
            const char *fn = hd->fields[f].name;
            if (strcmp(fn, "Name") && strcmp(fn, "Created") && strcmp(fn, "From") && strcmp(fn, "Made") && strcmp(fn, "Volume")
                && strcmp(fn, "Discs"))
                rec_add(qh, fn, hd->fields[f].value);
        }
        for (size_t k = 0; k + 1 < defaults.n; k += 2) rec_set(qh, defaults.v[k], defaults.v[k + 1]);
        q.v = xmalloc((p.n + 1) * sizeof *q.v);
        size_t relinked = 0;
        for (size_t k = 0; k < p.n; k++) {    /* a copied item points at its original again: the copy is NAME's */
            plan_item it = p.v[k];
            if (it.origin) {
                it.source = it.origin;
                it.origin = NULL;
                relinked++;
            }
            it.seen = stamp_of(it.source);
            q.v[q.n++] = it;
        }
        if (relinked) printf("%zu copied item%s point%s at %s original%s again (arv plan add --copy to copy afresh)\n", relinked,
                             relinked == 1 ? "" : "s", relinked == 1 ? "s" : "", relinked == 1 ? "its" : "their", relinked == 1 ? "" : "s");
        plan_save(&q);
        size_t gone = 0;
        for (size_t k = 0; k < q.n; k++) gone += access(q.v[k].source, F_OK) != 0;
        printf("Plan %s: from %s, %zu item%s on %d disc%s%s. Next: arv plan show %s, then arv plan make %s\n", q.name, name, q.n,
               q.n == 1 ? "" : "s", q.discs, q.discs == 1 ? "" : "s",
               gone ? " (some sources are gone: arv plan show)" : "", q.name, q.name);
        return 0;
    }
    int made = rec_get(hd, "Made") != NULL;
    if (made && (strcmp(action, "show") || defaults.n))
        die2("plan %s was made into discs already (arv plan show); the same again as a new plan: arv plan again %s NEW", name, name);
    if (defaults.n) {                         /* settings given to any other action: changed in the plan */
        for (size_t k = 0; k + 1 < defaults.n; k += 2) rec_set(hd, defaults.v[k], defaults.v[k + 1]);
        plan_save(&p);
    }

    if (!strcmp(action, "show")) {
        if (args.n) return 2;
        show(&h, &p, as_json);
        return 0;
    }
    if (!strcmp(action, "add")) {
        if (!args.n) die("%s", "arv plan add NAME SOURCE...: the files or folders to put on a disc");
        if (as && args.n > 1) die("%s", "--as names one thing: add one SOURCE at a time with it");
        for (size_t k = 0; k < args.n; k++) {
            char *src = realpath(args.v[k], NULL);
            struct stat st;
            if (!src || stat(src, &st) || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)))
                die("%s is not a file or folder", args.v[k]);
            if (strpbrk(src, "\r\n") || (as && strpbrk(as, "\r\n"))) die("%s: a name with a line break cannot be planned", src);
            for (size_t j = 0; j < p.n; j++)
                if (!strcmp(p.v[j].source, src) || (p.v[j].origin && !strcmp(p.v[j].origin, src))) {
                    char n[16];
                    snprintf(n, sizeof n, "%d", p.v[j].disc);
                    die2("%s is in the plan already, on disc %s", src, n);
                }
            char *base = strrchr(src, '/') ? strrchr(src, '/') + 1 : src;
            const char *path = as ? as : *base ? base : NULL;
            if (!path || !path_ok(path, S_ISDIR(st.st_mode)))
                die("%s cannot be a path under data/ (relative, no . or .. parts; . only for a folder's contents)", path ? path : "/");
            plan_item it = { 0, src, xstrdup(path), NULL, NULL };
            int d = pick_disc(&h, &p, disc, item_size(&it).sectors);
            check_free(&p, path, d);
            if (d > p.discs) p.discs = d;
            it.disc = d;
            if (copy) {                     /* the plan's own copy: for a source that will not be there at make */
                char *dir = copies_dir(&p), *slot = NULL;
                for (int n = 1; !slot || !access(slot, F_OK); n++) {
                    free(slot);
                    slot = xprintf("%s/%d", dir, n);
                }
                if (mkdirs(slot)) die("cannot create %s", slot);
                char *to = join(slot, *base ? base : "item");
                fprintf(stderr, "Copying %s into the plan ...\n", src);
                copy_keep(src, to);
                it.origin = src;
                it.source = to;
                free(dir);
                free(slot);
            }
            it.seen = stamp_of(it.source);
            p.v = xrealloc(p.v, (p.n + 1) * sizeof *p.v);
            p.v[p.n++] = it;
            char *what = xprintf("%s%s%s", path, S_ISDIR(st.st_mode) && strcmp(path, ".") ? "/" : "", copy ? " (copied into the plan)" : "");
            say_where(&h, &p, d, what);
            free(what);
        }
        plan_save(&p);
        return 0;
    }
    if (!strcmp(action, "move")) {
        if (!args.n || !disc) die("%s", "arv plan move NAME PATH... --disc N|new [--from N]");
        int from = from_s ? atoi(from_s) : 0;
        int to = pick_disc(&h, &p, disc, 0);
        if (to > p.discs) p.discs = to;
        for (size_t k = 0; k < args.n; k++) {
            plan_item *it = find_item(&p, args.v[k], from);
            if (it->disc == to) continue;
            check_free(&p, it->path, to);
            it->disc = to;
            say_where(&h, &p, to, it->path);
        }
        plan_save(&p);
        return 0;
    }
    if (!strcmp(action, "drop")) {
        if (!args.n) die("%s", "arv plan drop NAME PATH... [--from N]");
        int from = from_s ? atoi(from_s) : 0;
        for (size_t k = 0; k < args.n; k++) {
            plan_item *it = find_item(&p, args.v[k], from);
            if (it->origin) {               /* the plan's own copy goes with it */
                char *slot = xstrdup(it->source), *slash = strrchr(slot, '/');
                if (slash) *slash = 0;
                remove_tree(slot);
                free(slot);
                printf("%s: off disc %d, and the plan's copy deleted (the original is untouched)\n", it->path, it->disc);
            } else {
                printf("%s: off disc %d (the source is untouched)\n", it->path, it->disc);
            }
            *it = p.v[--p.n];
        }
        plan_save(&p);
        return 0;
    }
    if (!strcmp(action, "disc")) {
        if (args.n == 1 && !strcmp(args.v[0], "add")) {
            p.discs++;
            plan_save(&p);
            printf("Disc %d added, empty\n", p.discs);
            return 0;
        }
        if (args.n != 2 || strcmp(args.v[0], "drop")) die("%s", "arv plan disc NAME add | drop N");
        int d = atoi(args.v[1]);
        if (d < 1 || d > p.discs) die("no disc %s in the plan", args.v[1]);
        for (size_t k = 0; k < p.n; k++)
            if (p.v[k].disc == d) die("disc %s is not empty: move or drop what is on it first", args.v[1]);
        if (p.discs == 1) die("%s", "a plan keeps one disc at least (arv plan delete NAME)");
        for (size_t k = 0; k < p.n; k++) p.v[k].disc -= p.v[k].disc > d;
        p.discs--;
        plan_save(&p);
        printf("Disc %d dropped; the discs after it moved up one\n", d);
        return 0;
    }
    if (!strcmp(action, "refresh")) {         /* what the sources are now is what is planned */
        if (args.n) return 2;
        size_t n = 0;
        for (size_t k = 0; k < p.n; k++)
            if (changed_since(&p.v[k]) || !p.v[k].seen) {
                free(p.v[k].seen);
                p.v[k].seen = stamp_of(p.v[k].source);
                printf("%s: as it is now\n", p.v[k].path);
                n++;
            }
        plan_save(&p);
        if (!n) printf("Nothing changed since it was planned\n");
        return 0;
    }
    if (!strcmp(action, "make")) {
        if (!p.n) die("plan %s has nothing on it yet (arv plan add)", name);
        for (size_t k = 0; k < p.n; k++)    /* told, not stopped: what is there now is what goes on the disc */
            if (changed_since(&p.v[k]))
                fprintf(stderr, "Note: %s changed since it was planned (%s); the disc gets it as it is now\n", p.v[k].path,
                        p.v[k].source);
        for (size_t k = 0; k < p.n; k++) {
            struct stat st;
            if (stat(p.v[k].source, &st)) die("%s is gone (arv plan show)", p.v[k].source);
        }
        char **mv = xmalloc((rest.n + 16) * sizeof *mv);   /* the plan's settings first: options given win */
        int n = 0;
        mv[n++] = "-C";
        mv[n++] = h.path;
        for (int k = 0; k < 5; k++)
            if (rec_get(hd, SETTINGS[k][1])) {
                mv[n++] = (char *)SETTINGS[k][0];
                mv[n++] = (char *)rec_get(hd, SETTINGS[k][1]);
            }
        for (size_t k = 0; k < rest.n; k++) mv[n++] = rest.v[k];
        mv[n++] = "--plan";
        mv[n++] = file;
        mv[n] = NULL;
        return cmd_make(n, mv);
    }
    return 2;
}

/* ------------------------------------------------------------------ data objects (arv make --plan) */

void text_sha256(const char *text, char hex[65])
{
    sha256_ctx c;
    unsigned char d[32];
    sha256_init(&c);
    sha256_update(&c, text, strlen(text));
    sha256_final(&c, d);
    sha256_hex(d, hex);
}

static int by_line_path(const void *a, const void *b)
{
    return strcmp(*(char *const *)a + 66, *(char *const *)b + 66);
}

/* is path inside item path p ("." holds what no other item on its disc holds) */
static int in_item(const char *path, const char *p)
{
    size_t n = strlen(p);
    return !strcmp(path, p) || (!strncmp(path, p, n) && path[n] == '/');
}

void plan_objects(const disc_plan *dp, int disc, const entries *files, const archive *cat, const recs *made_now,
                  const char *disc_id, const char *today, recs *out, strlist *manifests)
{
    for (size_t k = 0; k < dp->n; k++) {
        const plan_item *it = &dp->v[k];
        if (it->disc != disc) continue;
        int top = !strcmp(it->path, ".");
        struct stat st;
        int is_file = !stat(it->source, &st) && S_ISREG(st.st_mode);
        strlist lines = { 0 };
        uint64_t bytes = 0;
        int git = 0;
        for (size_t i = 0; i < files->n; i++) {
            const char *p = files->v[i].path, *rel;
            if (top) {
                int other = 0;
                for (size_t j = 0; j < dp->n && !other; j++)
                    other = dp->v[j].disc == disc && j != k && in_item(p, dp->v[j].path);
                if (other) continue;
                rel = p;
            } else if (in_item(p, it->path)) {
                rel = p[strlen(it->path)] ? p + strlen(it->path) + 1 : p;
            } else {
                continue;
            }
            if (!strcmp(rel, ".git/HEAD")) git = 1;
            if (git_internal(rel)) continue;    /* the history: git.tsv says what it holds */
            char *line = xprintf("%s  %s", files->v[i].sha256, is_file ? (strrchr(it->source, '/') + 1) : rel);
            strlist_add(&lines, line);
            free(line);
            bytes += files->v[i].size;
        }
        if (lines.n) qsort(lines.v, lines.n, sizeof *lines.v, by_line_path);
        sbuf text = { 0 };
        sb_puts(&text, "");
        for (size_t i = 0; i < lines.n; i++) sb_printf(&text, "%s\n", lines.v[i]);
        char tree[65];
        if (is_file && lines.n) {           /* a file's Tree is its own SHA-256: its name is not part of it */
            memcpy(tree, lines.v[0], 64);
            tree[64] = 0;
        } else {
            text_sha256(text.s, tree);
        }
        /* its lineage: the same content is the same version (a copy); from the same source,
         * the next version; else a new object. An empty one is never the same as another. */
        const rec_record *same = NULL, *from = NULL;
        long latest = 0;
        char *stored = path_rel(dp->root, it->origin ? it->origin : it->source);   /* as the home keeps it */
        const recs *both[2] = { &cat->objects, made_now };
        for (int b = 0; b < 2; b++)
            for (size_t i = 0; i < both[b]->n; i++) {
                const rec_record *o = both[b]->v[i];
                if (lines.n && rec_get(o, "Tree") && !strcmp(rec_get(o, "Tree"), tree)) same = o;
                if (rec_get(o, "Source") && !strcmp(rec_get(o, "Source"), stored)) from = o;
            }
        char uuid[37], version[24];
        if (same) {
            snprintf(uuid, sizeof uuid, "%s", rec_get(same, "Uuid"));
            snprintf(version, sizeof version, "%s", rec_get(same, "Version"));
        } else if (from) {
            snprintf(uuid, sizeof uuid, "%s", rec_get(from, "Uuid"));
            for (int b = 0; b < 2; b++)
                for (size_t i = 0; i < both[b]->n; i++)
                    if (rec_get(both[b]->v[i], "Uuid") && !strcmp(rec_get(both[b]->v[i], "Uuid"), uuid)
                        && atol(rec_get(both[b]->v[i], "Version") ? rec_get(both[b]->v[i], "Version") : "0") > latest)
                        latest = atol(rec_get(both[b]->v[i], "Version"));
            snprintf(version, sizeof version, "%ld", latest + 1);
        } else {
            uuid4(uuid);
            snprintf(version, sizeof version, "1");
        }
        rec_record *r = rec_alloc("Object");
        const char *from_path = it->origin ? it->origin : it->source;   /* a copy's object came from its original */
        const char *slash = strrchr(from_path, '/');
        char n_files[24], n_bytes[24];
        snprintf(n_files, sizeof n_files, "%zu", lines.n);
        snprintf(n_bytes, sizeof n_bytes, "%llu", (unsigned long long)bytes);
        rec_add(r, "Uuid", uuid);
        rec_add(r, "Version", version);
        rec_add(r, "Name", slash && slash[1] ? slash + 1 : from_path);
        rec_add(r, "Kind", is_file ? "file" : git ? "git" : "folder");
        rec_add(r, "Tree", tree);
        rec_add(r, "Disc", disc_id);
        rec_add(r, "Path", it->path);
        rec_add(r, "Files", n_files);
        rec_add(r, "Bytes", n_bytes);
        rec_add(r, "Date", today);
        rec_add(r, "Source", stored);
        recs_add(out, r);
        strlist_add(manifests, text.s);
        fprintf(stderr, "Object %s%s (data/%s on %s): %s\n", rec_get(r, "Name"), is_file ? "" : "/", top ? "" : it->path, disc_id,
                same ? "the same as a version already archived (another copy of it)"
                : from ? "a new version of what was archived from there before" : "new");
        free(text.s);
        free(stored);
        strlist_free(&lines);
    }
}
