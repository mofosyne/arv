/* Disc plans: discs composed by hand from files and folders anywhere (the mastering workspace).
 *
 *   arv plan new NAME [--medium M] [--set CODE] [--title TEXT] [--description TEXT] [--access LEVEL] [--discs N]
 *   arv plan list
 *   arv plan show NAME [--json]
 *   arv plan add NAME SOURCE... [--disc N|new|auto] [--as PATH]
 *   arv plan move NAME PATH... --disc N|new [--from N]
 *   arv plan drop NAME PATH... [--from N]
 *   arv plan disc NAME add | drop N
 *   arv plan make NAME [arv make's options]
 *   arv plan delete NAME
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
#include <sys/stat.h>
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
    const rec_record *hd = head(out);
    out->name = xstrdup(rec_get(hd, "Name") ? rec_get(hd, "Name") : "plan");
    out->discs = rec_get(hd, "Discs") ? atoi(rec_get(hd, "Discs")) : 0;
    out->v = xmalloc((out->rec.nrecords + 1) * sizeof *out->v);
    for (size_t i = 0; i < out->rec.nrecords; i++) {
        const rec_record *r = &out->rec.records[i];
        if (r->descriptor || !r->type || strcmp(r->type, "Item")) continue;
        plan_item *it = &out->v[out->n++];
        it->disc = rec_get(r, "Disc") ? atoi(rec_get(r, "Disc")) : 0;
        it->source = xstrdup(rec_get(r, "Source") ? rec_get(r, "Source") : "");
        it->path = xstrdup(rec_get(r, "Path") ? rec_get(r, "Path") : "");
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
        sb_printf(&b, "\nDisc: %d\nSource: %s\nPath: %s\n", p->v[i].disc, p->v[i].source, p->v[i].path);
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

/* ------------------------------------------------------------------ showing it */

static void json_str(sbuf *b, const char *s)
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
    for (size_t i = 0; i < p->n; i++) {
        m[i] = item_size(&p->v[i]);
        used[p->v[i].disc] += m[i].sectors;
        bytes[p->v[i].disc] += m[i].bytes;
    }
    const char *fields[] = { "Title", "Set", "Medium", "Description", "Access", NULL };
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
                sb_printf(&b, ", \"kind\": \"%s\", \"bytes\": %llu, \"files\": %ld}",
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
                    printf("  %s/  %s, %ld file%s  %s\n", p->v[i].path, sz, m[i].files,
                           m[i].files == 1 ? "" : "s", p->v[i].source);
                } else {
                    printf("  %s  %s  %s\n", p->v[i].path, sz, p->v[i].source);
                }
            }
        }
        if (rec_get(hd, "Made")) {
            printf("Made %s:", rec_get(hd, "Made"));
            for (size_t f = 0; f < hd->nfields; f++)
                if (!strcmp(hd->fields[f].name, "Volume")) printf(" %s", hd->fields[f].value);
            putchar('\n');
        } else if (missing) {
            printf("%zu item%s missing: put %s back, or arv plan drop %s PATH\n", missing, missing == 1 ? " is" : "s are",
                   missing == 1 ? "it" : "them", p->name);
        } else if (p->n) {
            printf("Next: arv plan make %s (one image a disc; arv make's options apply)\n", p->name);
        }
    }
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
    int as_json = 0;
    strlist args = { 0 }, defaults = { 0 }, rest = { 0 };
    static const char *const SETTINGS[][2] = { { "--medium", "Medium" }, { "--set", "Set" }, { "--title", "Title" },
                                               { "--description", "Description" }, { "--access", "Access" } };
    int i = 0;
    for (; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
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
        else if (!strcmp(argv[i], "--json")) as_json = 1;
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
            printf("%s  %d disc%s, %zu item%s%s%s%s\n", p.name, p.discs, p.discs == 1 ? "" : "s", p.n, p.n == 1 ? "" : "s",
                   rec_get(hd, "Title") ? "  \"" : "", rec_get(hd, "Title") ? rec_get(hd, "Title") : "",
                   rec_get(hd, "Title") ? "\"" : "");
            if (rec_get(hd, "Made")) printf("    made %s\n", rec_get(hd, "Made"));
            free(f);
        }
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
        if (unlink(file)) die("cannot delete %s", file);
        printf("Plan %s deleted (its sources are untouched)\n", name);
        return 0;
    }

    disc_plan p;
    plan_load(file, &p);
    rec_record *hd = head(&p);
    if (defaults.n) {                         /* settings given to any action: changed in the plan */
        if (strcmp(action, "new")) {
            for (size_t k = 0; k + 1 < defaults.n; k += 2) rec_set(hd, defaults.v[k], defaults.v[k + 1]);
            plan_save(&p);
        }
    }
    int made = rec_get(hd, "Made") != NULL;
    if (made && strcmp(action, "show"))
        die("plan %s was made into discs already (arv plan show); start another with arv plan new", name);

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
                if (!strcmp(p.v[j].source, src)) {
                    char n[16];
                    snprintf(n, sizeof n, "%d", p.v[j].disc);
                    die2("%s is in the plan already, on disc %s", src, n);
                }
            char *base = strrchr(src, '/') ? strrchr(src, '/') + 1 : src;
            const char *path = as ? as : *base ? base : NULL;
            if (!path || !path_ok(path, S_ISDIR(st.st_mode)))
                die("%s cannot be a path under data/ (relative, no . or .. parts; . only for a folder's contents)", path ? path : "/");
            plan_item it = { 0, src, xstrdup(path) };
            int d = pick_disc(&h, &p, disc, item_size(&it).sectors);
            check_free(&p, path, d);
            if (d > p.discs) p.discs = d;
            it.disc = d;
            p.v = xrealloc(p.v, (p.n + 1) * sizeof *p.v);
            p.v[p.n++] = it;
            char *what = xprintf("%s%s", path, S_ISDIR(st.st_mode) && strcmp(path, ".") ? "/" : "");
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
            printf("%s: off disc %d (the source is untouched)\n", it->path, it->disc);
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
    if (!strcmp(action, "make")) {
        if (!p.n) die("plan %s has nothing on it yet (arv plan add)", name);
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
