/* Disc plans: discs composed by hand from files and folders anywhere (the mastering workspace).
 *
 *   arv plan new NAME [--medium M] [--set CODE] [--title TEXT] [--description TEXT] [--access LEVEL] [--discs N]
 *                    [--collection CODE]   (--collection: its discs are that collection's next edition)
 *   arv plan list [--all]           open plans (--all: made ones too)
 *   arv plan show NAME [--json] [--archived] [--duplicates]   (--duplicates: byte-identical files on a
 *                                  disc; --archived: hashes each item, through the hash
 *                                  cache, and says what of it is on discs already)
 *   arv plan add NAME SOURCE [--of OBJECT]   SOURCE continues that data object (a copy of it you made); files
 *                                  restored by arv restore continue theirs on their own (its .arv marker)
 *   arv plan add NAME SOURCE... [--disc N|new|auto] [--as PATH] [--each]   (--each: a folder's every
 *                                  file an item, a data object, of its own)
 *   arv plan add NAME --list FILE|- [--disc N|new|auto]   SOURCE<TAB>PATH[<TAB>DISC] lines, all or none
 *   arv plan rename NAME PATH NEWPATH [--from N]   a new place on its disc (inside a planned folder:
 *                                  split off, as plan add of that file or folder --as NEWPATH)
 *   arv plan tag NAME SOURCE TAGS   tags for a file or folder in the plan ("" for none)
 *   arv plan tag NAME --list FILE|-   SOURCE<TAB>TAGS lines, all or none
 *   arv plan layout NAME LAYOUT [--disc N]   a disc's tree made from its items' tags, each item placed
 *                                  whole: '/' nests, ',' falls back ('year/event, year'); '' by hand
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
 * takes, and Layout: a disc's tree from its items' tags, "N LAYOUT" for disc N), then an Item
 * record for each thing on a disc (Disc, Source, Path), then a Tags record for each source given
 * tags. Sizes are measured
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

static const char *get_or(const rec_record *r, const char *name, const char *dflt)
{
    const char *v = rec_get(r, name);
    return v ? v : dflt;
}

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
        if (!r->descriptor && r->type && !strcmp(r->type, "Tags") && rec_get(r, "Source") && *rec_get(r, "Source")) {
            char *src = path_abs(out->root, rec_get(r, "Source"));
            strlist_add(&out->tag_src, src);
            strlist_add(&out->tag_val, rec_get(r, "Tags") ? rec_get(r, "Tags") : "");
            free(src);
            continue;
        }
        if (r->descriptor || !r->type || strcmp(r->type, "Item")) continue;
        plan_item *it = &out->v[out->n++];
        memset(it, 0, sizeof *it);
        it->disc = rec_get(r, "Disc") ? atoi(rec_get(r, "Disc")) : 0;
        it->source = rec_get(r, "Source") && *rec_get(r, "Source") ? path_abs(out->root, rec_get(r, "Source")) : xstrdup("");
        it->path = xstrdup(rec_get(r, "Path") ? rec_get(r, "Path") : "");
        it->seen = rec_get(r, "Seen") ? xstrdup(rec_get(r, "Seen")) : NULL;
        it->of = rec_get(r, "Of") ? xstrdup(rec_get(r, "Of")) : NULL;
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
        if (p->v[i].seen) sb_printf(&b, "Seen: %s\n", p->v[i].seen);
        if (p->v[i].of) sb_printf(&b, "Of: %s\n", p->v[i].of);
    }
    for (size_t i = 0; i < p->tag_src.n; i++) {
        if (!i) sb_puts(&b, "\n%rec: Tags\n");
        char *src = path_rel(p->root, p->tag_src.v[i]);
        sb_printf(&b, "\nSource: %s\nTags: %s\n", src, p->tag_val.v[i]);
        free(src);
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

/* ------------------------------------------------------------------ tags and layouts */

/* a tag's namespace length ("event:trip" -> 5), 0 for a plain tag */
static size_t ns_of(const char *tag)
{
    const char *c = strchr(tag, ':');
    if (!c || c == tag) return 0;
    for (const char *t = tag; t < c; t++)
        if (*t == ' ') return 0;
    return (size_t)(c - tag);
}

/* the tags the plan gives a file or folder (absolute): those of each folder above it, then its own; of
 * a one-valued namespace, the nearest value replaces the ones above it */
static void plan_tags_of(const disc_plan *p, const char *abs, const strlist *one, strlist *out)
{
    memset(out, 0, sizeof *out);
    size_t *at = xmalloc((p->tag_src.n + 1) * sizeof *at), n = 0;
    for (size_t i = 0; i < p->tag_src.n; i++) {
        const char *src = p->tag_src.v[i];
        size_t l = strlen(src);
        if (!strncmp(abs, src, l) && (!abs[l] || abs[l] == '/')) at[n++] = i;
    }
    for (size_t a = 1; a < n; a++)          /* nearest last */
        for (size_t b = a; b > 0 && strlen(p->tag_src.v[at[b - 1]]) > strlen(p->tag_src.v[at[b]]); b--) {
            size_t t = at[b];
            at[b] = at[b - 1];
            at[b - 1] = t;
        }
    for (size_t a = 0; a < n; a++) {
        char *list = xstrdup(p->tag_val.v[at[a]]);
        for (char *t = strtok(list, ","); t; t = strtok(NULL, ",")) {
            while (*t == ' ') t++;
            char *e = t + strlen(t);
            while (e > t && e[-1] == ' ') *--e = 0;
            if (!*t) continue;
            size_t l = ns_of(t);
            if (l) {
                char *ns = xprintf("%.*s", (int)l, t);
                for (size_t k = 0; strlist_has(one, ns) && k < out->n;) {
                    if (ns_of(out->v[k]) == l && !strncmp(out->v[k], t, l)) {
                        free(out->v[k]);
                        out->v[k] = out->v[--out->n];
                    } else {
                        k++;
                    }
                }
                free(ns);
            }
            if (!strlist_has(out, t)) strlist_add(out, t);
        }
        free(list);
    }
    free(at);
}

/* the same, ", "-joined ("" for none) */
static char *plan_tags_text(const disc_plan *p, const char *abs, const strlist *one)
{
    strlist tags;
    plan_tags_of(p, abs, one, &tags);
    sbuf b = { 0 };
    sb_puts(&b, "");
    for (size_t t = 0; t < tags.n; t++) sb_printf(&b, "%s%s", t ? ", " : "", tags.v[t]);
    strlist_free(&tags);
    return b.s;
}

/* a layout checked against the vocabulary: alternatives split by ",", levels by "/"; each level a
 * one-valued namespace, or year or decade (made from date). NULL, or what is wrong */
static char *layout_problem(const char *layout, const strlist *one)
{
    char *copy = xstrdup(layout), *problem = NULL;
    int alts = 0;
    for (char *alt = copy, *next; alt && !problem; alt = next) {
        next = strchr(alt, ',');
        if (next) *next++ = 0;
        int levels = 0;
        for (char *lv = alt, *nl; lv && !problem; lv = nl) {
            nl = strchr(lv, '/');
            if (nl) *nl++ = 0;
            while (*lv == ' ') lv++;
            char *e = lv + strlen(lv);
            while (e > lv && e[-1] == ' ') *--e = 0;
            if (!*lv) problem = xstrdup("an empty level (levels are split by / and alternatives by ,)");
            else if (!strcmp(lv, "year") || !strcmp(lv, "decade")) {
                if (!strlist_has(one, "date")) problem = xprintf("%s needs a one-valued date namespace in the tag vocabulary", lv);
            } else if (!strlist_has(one, lv)) {
                problem = xprintf("%s is not a one-valued namespace (a file with two values would need two folders; "
                                  "arv tags lists the namespaces)", lv);
            }
            levels++;
        }
        alts += levels > 0;
    }
    if (!problem && !alts) problem = xstrdup("it is empty");
    free(copy);
    return problem;
}

/* where a layout puts a file with these tags: the first alternative it has a value for at every level,
 * as namespace/value/.../slug (year/2019/event/kyoto-trip/IMG_0001.JPG); NULL for none. A date is
 * filed by its start; without one, by the file's own time when the vocabulary allows it. */
static char *laid_out(const char *layout, const strlist *tags, time_t mtime, int date_from_file, const char *slug)
{
    char *copy = xstrdup(layout), *out = NULL;
    for (char *alt = copy, *next; alt && !out; alt = next) {
        next = strchr(alt, ',');
        if (next) *next++ = 0;
        sbuf b = { 0 };
        int ok = 1;
        for (char *lv = alt, *nl; lv && ok; lv = nl) {
            nl = strchr(lv, '/');
            if (nl) *nl++ = 0;
            while (*lv == ' ') lv++;
            char *e = lv + strlen(lv);
            while (e > lv && e[-1] == ' ') *--e = 0;
            int year = !strcmp(lv, "year"), decade = !strcmp(lv, "decade");
            const char *ns = year || decade ? "date" : lv, *value = NULL;
            size_t l = strlen(ns);
            for (size_t t = 0; t < tags->n && !value; t++)
                if (ns_of(tags->v[t]) == l && !strncmp(tags->v[t], ns, l)) {
                    value = tags->v[t] + l + 1;
                    while (*value == ' ') value++;
                }
            if (year || decade) {
                long first = 0, last = 0, y = 0;
                if (value && edtf_span(value, &first, &last) == 1 && first > EDTF_MIN) y = first / 10000;
                else if (!value && date_from_file) {
                    struct tm tm;
                    localtime_r(&mtime, &tm);
                    y = tm.tm_year + 1900;
                }
                if (!y) ok = 0;
                else if (year) sb_printf(&b, "year/%ld/", y);
                else sb_printf(&b, "decade/%lds/", y / 10 * 10);
            } else if (!value || !*value) {
                ok = 0;
            } else {
                sb_printf(&b, "%s/", lv);
                for (const char *c = value; *c; c++) sb_add(&b, *c == '/' ? "-" : c, 1);
                sb_puts(&b, "/");
            }
        }
        if (ok && b.s) {
            sb_puts(&b, slug);
            out = b.s;
        } else {
            free(b.s);
        }
    }
    free(copy);
    return out;
}

/* the layout of a disc of the plan: its own (Layout: N LAYOUT), else the plan's (Layout: LAYOUT); NULL
 * when its items are placed by hand */
const char *plan_layout(const disc_plan *p, int disc)
{
    const char *all = NULL, *mine = NULL;
    for (size_t i = 0; i < p->rec.nrecords; i++) {
        const rec_record *r = &p->rec.records[i];
        if (r->descriptor || !r->type || strcmp(r->type, "Plan")) continue;
        for (size_t f = 0; f < r->nfields; f++) {
            if (strcmp(r->fields[f].name, "Layout")) continue;
            const char *v = r->fields[f].value;
            char *end;
            long n = strtol(v, &end, 10);
            if (end != v && *end == ' ') {
                if (n == disc) mine = end + 1;
            } else if (*v) {
                all = v;
            }
        }
    }
    return mine ? mine : all;
}

static int plan_uses_tags(const disc_plan *p)
{
    int uses = p->tag_src.n > 0;
    for (int d = 1; d <= p->discs && !uses; d++) uses = plan_layout(p, d) != NULL;
    return uses;
}

/* where a layout puts an item, whole: folders from the item's tags (its source's and its folders'),
 * then its name; first is its earliest file's time, for a date it has no tag for. NULL when it has
 * a value for no alternative: it keeps its place by hand. */
static const rec_record *lineage_of(const disc_plan *p, const plan_item *it, const archive *cat);

static char *item_place(const arv_home *h, const disc_plan *p, const plan_item *it, const char *layout, const strlist *one,
                        int date_from_file, time_t first, const archive *cat)
{
    strlist tags;
    plan_tags_of(p, it->source, one, &tags);
    const char *name = !strcmp(it->path, ".") ? strrchr(it->source, '/') + 1
                       : strrchr(it->path, '/') ? strrchr(it->path, '/') + 1 : it->path;
    const rec_record *was = cat ? lineage_of(p, it, cat) : NULL;   /* archived before: its tags and name now */
    if (was && rec_get(was, "Uuid")) {
        const char *base = strrchr(it->source, '/') + 1;
        char *copy = object_tags_now(h, cat, was);
        for (char *t = copy ? strtok(copy, ",") : NULL; t; t = strtok(NULL, ",")) {
            while (*t == ' ') t++;
            size_t l = ns_of(t);
            int have = 0;                   /* the plan's own value of a namespace wins */
            for (size_t k = 0; k < tags.n && !have; k++)
                have = l ? ns_of(tags.v[k]) == l && !strncmp(tags.v[k], t, l) : !strcmp(tags.v[k], t);
            if (*t && !have) strlist_add(&tags, t);
        }
        free(copy);
        if (!strcmp(name, base)) name = object_slug(cat, was);   /* not named in the plan: its name now */
    }
    char *at = laid_out(layout, &tags, first, date_from_file, name);
    strlist_free(&tags);
    return at;
}

/* the data object a restored file or folder is: arv restore records where it put a disc's files (a
 * dissemination event, with Path), and the object at the same path on that disc is this; NULL for
 * none. Its provenance, not its bytes. The nearest restore above it decides. */
static const rec_record *restored_object(const archive *cat, const char *source, const char **disc)
{
    const rec_record *best = NULL;
    size_t best_n = 0;
    for (size_t i = 0; i < cat->events.n; i++) {
        const rec_record *e = cat->events.v[i];
        const char *t = rec_get(e, "Type"), *at = rec_get(e, "Path");
        if (!t || strcmp(t, "dissemination") || !at || !rec_get(e, "Disc")) continue;
        size_t n = strlen(at);
        if (strncmp(source, at, n) || (source[n] && source[n] != '/') || n < best_n) continue;
        best = e;
        best_n = n;
    }
    if (!best) return NULL;
    const char *rel = source[best_n] == '/' ? source + best_n + 1 : ".", *id = rec_get(best, "Disc");
    for (size_t i = 0; i < cat->objects.n; i++)
        if (!strcmp(get_or(cat->objects.v[i], "Disc", ""), id) && !strcmp(get_or(cat->objects.v[i], "Path", ""), rel)) {
            if (disc) *disc = id;
            return cat->objects.v[i];
        }
    return NULL;
}

/* the object this item continues: the one it was said to be (--of), else the newest archived from its
 * source, else the one it was restored from; NULL for none */
static const rec_record *lineage_of(const disc_plan *p, const plan_item *it, const archive *cat)
{
    const rec_record *was = NULL;
    if (it->of) {
        for (size_t i = 0; i < cat->objects.n; i++)
            if (rec_get(cat->objects.v[i], "Uuid") && !strcmp(rec_get(cat->objects.v[i], "Uuid"), it->of)) was = cat->objects.v[i];
        return was;
    }
    char *stored = path_rel(p->root, it->source);
    for (size_t i = 0; i < cat->objects.n; i++)
        if (rec_get(cat->objects.v[i], "Source") && !strcmp(rec_get(cat->objects.v[i], "Source"), stored)) was = cat->objects.v[i];
    free(stored);
    return was ? was : restored_object(cat, it->source, NULL);
}

/* the tags now of the object this item continues, or NULL */
static char *lineage_tags(const arv_home *h, const disc_plan *p, const plan_item *it, const archive *cat)
{
    const rec_record *was = lineage_of(p, it, cat);
    if (was) {
        char *t = object_tags_now(h, cat, was);
        if (*t) return t;
        free(t);
    }
    return NULL;
}

/* a (taken) with b's tags it lacks, ", "-joined */
static char *merged_tags(char *a, const char *b)
{
    if (!b || !*b) return a;
    sbuf out = { 0 };
    sb_puts(&out, a);
    char *copy = xstrdup(b);
    for (char *t = strtok(copy, ","); t; t = strtok(NULL, ",")) {
        while (*t == ' ') t++;
        char *probe = xprintf(", %s, ", t), *hay = xprintf(", %s, ", a);
        if (*t && !strstr(hay, probe)) sb_printf(&out, "%s%s", out.len ? ", " : "", t);
        free(probe);
        free(hay);
    }
    free(copy);
    free(a);
    return out.s ? out.s : xstrdup("");
}

/* ------------------------------------------------------------------ the files a plan puts on its discs */

/* A file two items cover goes where the more specific one puts it: an item whose source is inside
 * this item's source, and holds the file (or is it), places it instead; the deepest such item wins.
 * So a folder can be split (the folder as one item, some of its files or folders placed elsewhere)
 * and each file is on the discs once (arv plan add refuses a source twice). NULL when this item
 * places the file itself. */
static const plan_item *placed_by(const disc_plan *p, const plan_item *it, const char *abs)
{
    size_t n = strlen(it->source);
    const plan_item *best = NULL;
    for (size_t i = 0; i < p->n; i++) {
        const plan_item *o = &p->v[i];
        size_t m = strlen(o->source);
        if (o == it || m <= n || strncmp(o->source, it->source, n) || o->source[n] != '/') continue;
        if (strncmp(abs, o->source, m) || (abs[m] && abs[m] != '/')) continue;
        if (!best || m > strlen(best->source)) best = o;
    }
    return best;
}

static int by_bin_path(const void *a, const void *b)
{
    const entry *x = *(entry *const *)a, *y = *(entry *const *)b;
    return x->bin != y->bin ? (x->bin < y->bin ? -1 : 1) : strcmp(x->path, y->path);
}

void plan_scan(const arv_home *h, disc_plan *p, const char *links, entries *files, entries *noted, size_t *left_out,
               strlist *tagged, strlist *clashes)
{
    memset(files, 0, sizeof *files);
    memset(noted, 0, sizeof *noted);
    size_t fcap = 0, ncap = 0;
    strlist one = { 0 };
    int date_from_file = 0;
    archive cat;                            /* the record: objects archived from these sources before */
    if (h) archive_load(&cat, h->rec_path);
    if (h && plan_uses_tags(p)) tag_namespaces(h, &one, NULL, &date_from_file);   /* the vocabulary only when needed */
    for (size_t i = 0; i < p->n; i++) {
        plan_item *it = &p->v[i];
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
        const char *layout = plan_layout(p, it->disc);
        free(it->placed);
        it->placed = NULL;
        if (layout) {                       /* placed whole, by its tags; dated by its earliest file */
            time_t first = 0;
            for (size_t j = 0; j < f.n; j++) {
                char *abs = *f.v[j].path ? join(it->source, f.v[j].path) : xstrdup(it->source);
                int mine = !S_ISDIR(st.st_mode) || (strcmp(f.v[j].path, ".arv") && strncmp(f.v[j].path, ".arv/", 5)
                                                     && !placed_by(p, it, abs));
                if (mine && (!first || f.v[j].mtime < first)) first = f.v[j].mtime;
                free(abs);
            }
            it->placed = item_place(h, p, it, layout, &one, date_from_file, first, h ? &cat : NULL);
        }
        char *was_tags = h ? lineage_tags(h, p, it, &cat) : NULL;   /* its tags now, if archived from here before */
        char *folder_tags = NULL;           /* a folder item's tags: once, on its folder */
        if (S_ISDIR(st.st_mode) && tagged && (p->tag_src.n || was_tags)) {
            folder_tags = merged_tags(p->tag_src.n ? plan_tags_text(p, it->source, &one) : xstrdup(""), was_tags);
            if (*folder_tags) {
                char *line = xprintf("%s\t%s", it->placed ? it->placed : it->path, folder_tags);
                strlist_add(tagged, line);
                free(line);
            }
        }
        const entries *both[2] = { &f, &n };
        for (int k = 0; k < 2; k++)
            for (size_t j = 0; j < both[k]->n; j++) {
                entry e = both[k]->v[j];
                if (S_ISDIR(st.st_mode) && (!strcmp(e.path, ".arv") || !strncmp(e.path, ".arv/", 5))) {
                    if (!k) (*left_out)++;          /* a folder's own .arv: arv's, never content */
                    continue;
                }
                if (S_ISDIR(st.st_mode)) {          /* placed by a more specific item instead */
                    char *abs = join(it->source, e.path);
                    const plan_item *other = placed_by(p, it, abs);
                    free(abs);
                    if (other) {
                        free(e.path);
                        continue;
                    }
                }
                char *dest = !*e.path ? xstrdup(it->path) : !strcmp(it->path, ".") ? xstrdup(e.path) : join(it->path, e.path);
                if (it->placed) {                   /* where the layout put the item; dest: where it was arranged */
                    e.origin = dest;
                    dest = *e.path ? join(it->placed, e.path) : xstrdup(it->placed);
                }
                if (!k && tagged && (p->tag_src.n || (was_tags && !*e.path))) {   /* the disc's tags.tsv: a file's */
                    char *abs = *e.path ? join(it->source, e.path) : xstrdup(it->source);   /* when not its folder's */
                    char *mine = p->tag_src.n ? plan_tags_text(p, abs, &one) : xstrdup("");
                    if (!*e.path) mine = merged_tags(mine, was_tags);   /* a file item: its tags now as well */
                    if (*mine && (!folder_tags || strcmp(mine, folder_tags))) {
                        char *line = xprintf("%s\t%s", dest, mine);
                        strlist_add(tagged, line);
                        free(line);
                    }
                    free(mine);
                    free(abs);
                }
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
        free(folder_tags);
        free(was_tags);
    }
    if (clashes && files->n > 1) {          /* two files one path: only a layout can do that */
        entry **by = xmalloc(files->n * sizeof *by);
        for (size_t i = 0; i < files->n; i++) by[i] = &files->v[i];
        qsort(by, files->n, sizeof *by, by_bin_path);
        for (size_t i = 1; i < files->n; i++)
            if (by[i]->bin == by[i - 1]->bin && !strcmp(by[i]->path, by[i - 1]->path)) {
                char *line = xprintf("data/%s on disc %d: %s and %s", by[i]->path, by[i]->bin + 1, by[i - 1]->source,
                                     by[i]->source);
                strlist_add(clashes, line);
                free(line);
            }
        free(by);
    }
    strlist_free(&one);
    if (!files->v) files->v = xmalloc(sizeof *files->v);
    if (!noted->v) noted->v = xmalloc(sizeof *noted->v);
}

/* the item whose source most closely holds this item's source (the folder it was split from), or NULL */
static const plan_item *holder(const disc_plan *p, const plan_item *in)
{
    const plan_item *near = NULL;
    for (size_t k = 0; k < p->n; k++) {
        size_t n = strlen(p->v[k].source);
        if (&p->v[k] != in && strlen(in->source) > n && !strncmp(in->source, p->v[k].source, n)
            && in->source[n] == '/' && (!near || n > strlen(near->source)))
            near = &p->v[k];
    }
    return near;
}

/* ------------------------------------------------------------------ sizes */

typedef struct {
    uint64_t bytes;
    long files, sectors;
    int missing, folder;
} measure;

static void walk_size(const disc_plan *pl, const plan_item *it, const char *disk, const char *rel, int top, measure *m)
{
    struct stat st;
    if (lstat(disk, &st)) return;
    if (pl && !top && placed_by(pl, it, disk)) return;  /* counted with the item that places it */
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
        walk_size(pl, it, p, r, 0, m);
        free(p);
        free(r);
    }
    if (d) closedir(d);
    m->sectors += 1;
}

static measure item_size(const disc_plan *p, const plan_item *it)
{
    measure m = { 0 };
    struct stat st;
    if (stat(it->source, &st)) {
        m.missing = 1;
        return m;
    }
    m.folder = S_ISDIR(st.st_mode);
    if (m.folder) walk_size(p, it, it->source, strcmp(it->path, ".") ? it->path : "", 1, &m);
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
    walk_size(NULL, NULL, path, "", 0, &m);
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

/* what of a planned item is on discs already: its files there (any disc that is not retired), and
 * the data object version it is, when its content is one archived before */
typedef struct {
    size_t files, on;
    const rec_record *object;     /* an archived version with the same Tree, or NULL */
    strlist discs;                /* the discs holding that version (not retired) */
} archived;

static void item_archived(const arv_home *h, const archive *cat, const strlist *have, const plan_item *it, archived *a)
{
    memset(a, 0, sizeof *a);
    struct stat st;
    char *text = path_manifest(h, it->source, &a->files);
    if (!text || !a->files) {
        free(text);
        return;
    }
    char tree[65];
    if (!stat(it->source, &st) && S_ISREG(st.st_mode)) {        /* a file's Tree is its own SHA-256 */
        memcpy(tree, text, 64);
        tree[64] = 0;
    } else {
        text_sha256(text, tree);
    }
    for (size_t i = 0; i < cat->objects.n; i++) {
        const rec_record *o = cat->objects.v[i], *d = archive_disc(cat, rec_get(o, "Disc") ? rec_get(o, "Disc") : "");
        if (!rec_get(o, "Tree") || strcmp(rec_get(o, "Tree"), tree) || !d || rec_get(d, "Retired")) continue;
        a->object = o;
        if (!strlist_has(&a->discs, rec_get(o, "Disc"))) strlist_add(&a->discs, rec_get(o, "Disc"));
    }
    for (char *l = text; *l;) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        if (strlen(l) > 66) {
            l[64] = 0;
            char *key = l;
            a->on += have->n && bsearch(&key, have->v, have->n, sizeof *have->v, by_str) != NULL;
        }
        l = nl ? nl + 1 : l + strlen(l);
    }
    free(text);
}

/* the SHA-256 of every file on a disc that is not retired, sorted */
static void archived_hashes(const arv_home *h, const archive *cat, strlist *out)
{
    for (size_t i = 0; i < cat->discs.n; i++) {
        const rec_record *d = cat->discs.v[i];
        if (rec_get(d, "Retired") || !rec_get(d, "Id")) continue;
        char *m = home_volume_file(h, rec_get(d, "Id"), "manifest.sha256"), *t = access(m, F_OK) ? NULL : read_text(m);
        for (char *l = t; l && *l;) {
            char *nl = strchr(l, '\n');
            if (nl) *nl = 0;
            if (strlen(l) > 66) {
                l[64] = 0;
                strlist_add(out, l);
            }
            l = nl ? nl + 1 : l + strlen(l);
        }
        free(t);
        free(m);
    }
    if (out->n) qsort(out->v, out->n, sizeof *out->v, by_str);
}

/* ------------------------------------------------------------------ byte-identical files on a disc */

static int by_bin_sha(const void *a, const void *b)
{
    const entry *x = *(entry *const *)a, *y = *(entry *const *)b;
    int c = x->bin != y->bin ? (x->bin < y->bin ? -1 : 1) : strcmp(x->sha256, y->sha256);
    return c ? c : strcmp(x->path, y->path);
}

static int by_sha(const void *a, const void *b)
{
    const entry *x = *(entry *const *)a, *y = *(entry *const *)b;
    int c = strcmp(x->sha256, y->sha256);
    return c ? c : strcmp(x->path, y->path);
}

/* files whose bytes another file on the same disc has (bin; all one disc unless by_bin), each
 * written in full: a line for each set ("A = B"), at most limit (0: all); how many files beyond the
 * first of each set, and their bytes */
size_t duplicates_report(const entries *files, int by_bin, size_t limit, FILE *out, uint64_t *bytes)
{
    *bytes = 0;
    if (files->n < 2) return 0;
    entry **by = xmalloc(files->n * sizeof *by);
    for (size_t i = 0; i < files->n; i++) by[i] = &files->v[i];
    qsort(by, files->n, sizeof *by, by_bin ? by_bin_sha : by_sha);
    size_t extra = 0, sets = 0;
    for (size_t i = 0; i < files->n;) {
        size_t j = i + 1;
        while (j < files->n && (!by_bin || by[j]->bin == by[i]->bin) && !strcmp(by[j]->sha256, by[i]->sha256) && by[i]->size)
            j++;
        if (j - i > 1) {
            extra += j - i - 1;
            *bytes += (uint64_t)(j - i - 1) * by[i]->size;
            if (out && (!limit || sets < limit)) {
                fputs("  ", out);
                if (by_bin) fprintf(out, "disc %d: ", by[i]->bin + 1);
                for (size_t k = i; k < j; k++) fprintf(out, "%s%s", k > i ? " = " : "", by[k]->path);
                fputc('\n', out);
            }
            sets++;
        }
        i = j;
    }
    if (out && limit && sets > limit) fprintf(out, "  ... %zu more\n", sets - limit);
    free(by);
    return extra;
}

/* plan show --duplicates: byte-identical files on each disc of the plan, as arranged (hashed
 * through the hash cache) */
static void show_duplicates(const arv_home *h, const disc_plan *p)
{
    entries all = { 0 };
    size_t cap = 0;
    for (size_t i = 0; i < p->n; i++) {
        const plan_item *it = &p->v[i];
        struct stat st;
        size_t n = 0;
        char *text = stat(it->source, &st) ? NULL : path_manifest(h, it->source, &n);
        int dir = text && S_ISDIR(st.st_mode);
        for (char *l = text; l && *l;) {
            char *nl = strchr(l, '\n');
            if (nl) *nl = 0;
            if (strlen(l) > 66) {
                char *abs = dir ? join(it->source, l + 66) : xstrdup(it->source);
                struct stat fs;
                if ((!dir || !placed_by(p, it, abs)) && !stat(abs, &fs)) {
                    if (all.n == cap) {
                        cap = cap ? 2 * cap : 64;
                        all.v = xrealloc(all.v, cap * sizeof *all.v);
                    }
                    entry *e = &all.v[all.n++];
                    memset(e, 0, sizeof *e);
                    memcpy(e->sha256, l, 64);
                    e->path = !dir ? xstrdup(it->path) : !strcmp(it->path, ".") ? xstrdup(l + 66) : join(it->path, l + 66);
                    e->size = (uint64_t)fs.st_size;
                    e->bin = it->disc - 1;
                }
                free(abs);
            }
            l = nl ? nl + 1 : l + strlen(l);
        }
        free(text);
    }
    uint64_t bytes;
    size_t extra = duplicates_report(&all, 1, 0, NULL, &bytes);
    if (!extra) {
        printf("No byte-identical files on any disc of plan %s\n", p->name);
    } else {
        char sz[32];
        human_size(bytes, sz);
        printf("%zu file%s (%s) %s byte-identical to others on the same disc, and written in full:\n", extra,
               extra == 1 ? "" : "s", sz, extra == 1 ? "is" : "are");
        duplicates_report(&all, 1, 0, stdout, &bytes);
        puts("Drop the copies you do not want (arv plan drop), or keep them: the plan is yours.");
    }
    for (size_t i = 0; i < all.n; i++) free(all.v[i].path);
    free(all.v);
}

/* the earliest time of the files an item places (no hashing), 0 for none */
static void first_walk(const disc_plan *p, const plan_item *it, const char *disk, int top, time_t *first)
{
    struct stat st;
    if (lstat(disk, &st) || (!top && placed_by(p, it, disk))) return;
    if (S_ISREG(st.st_mode)) {
        if (!*first || st.st_mtime < *first) *first = st.st_mtime;
        return;
    }
    if (!S_ISDIR(st.st_mode)) return;
    DIR *d = opendir(disk);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (top && !strcmp(e->d_name, ".arv"))) continue;
        char *q = join(disk, e->d_name);
        first_walk(p, it, q, 0, first);
        free(q);
    }
    if (d) closedir(d);
}

/* where the disc's layout puts each item (NULL: by hand), for plan show; the vocabulary is read
 * only when a disc has a layout */
static char **show_places(const arv_home *h, const disc_plan *p)
{
    char **at = xmalloc((p->n + 1) * sizeof *at);
    strlist one = { 0 };
    int date_from_file = 0, read = 0;
    archive cat;
    for (size_t i = 0; i < p->n; i++) {
        const char *layout = plan_layout(p, p->v[i].disc);
        at[i] = NULL;
        if (!layout) continue;
        if (!read++) {
            tag_namespaces(h, &one, NULL, &date_from_file);
            archive_load(&cat, h->rec_path);
        }
        time_t first = 0;
        first_walk(p, &p->v[i], p->v[i].source, 1, &first);
        at[i] = item_place(h, p, &p->v[i], layout, &one, date_from_file, first, &cat);
    }
    strlist_free(&one);
    return at;
}

static void show(const arv_home *h, disc_plan *p, int as_json, int want_archived)
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
    archived *ar = want_archived ? xmalloc((p->n + 1) * sizeof *ar) : NULL;
    char **at = show_places(h, p);          /* where a disc's layout puts each item */
    archive scat;                           /* what each item continues, when said or restored */
    archive_load(&scat, h->rec_path);
    if (want_archived) {
        archive cat;
        strlist have = { 0 };
        archive_load(&cat, h->rec_path);
        archived_hashes(h, &cat, &have);
        for (size_t i = 0; i < p->n; i++) item_archived(h, &cat, &have, &p->v[i], &ar[i]);
        strlist_free(&have);
    }
    for (size_t i = 0; i < p->n; i++) {
        changed += (chg[i] = !made && changed_since(&p->v[i]));
        m[i] = item_size(p, &p->v[i]);
        used[p->v[i].disc] += m[i].sectors;
        bytes[p->v[i].disc] += m[i].bytes;
    }
    const char *fields[] = { "Title", "Set", "Medium", "Description", "Access", "From", "Collection", NULL };
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
            sb_printf(&b, "%s{\"disc\": %d, \"bytes\": %llu, \"used\": %lld, \"layout\": ", d > 1 ? ", " : "", d,
                      (unsigned long long)bytes[d], (long long)used[d] * SECTOR);
            if (plan_layout(p, d)) json_str(&b, plan_layout(p, d));
            else sb_puts(&b, "null");
            sb_puts(&b, ", \"items\": [");
            int any = 0;
            for (size_t i = 0; i < p->n; i++) {
                if (p->v[i].disc != d) continue;
                sb_puts(&b, any ? ", {\"path\": " : "{\"path\": ");
                json_str(&b, p->v[i].path);
                sb_puts(&b, ", \"source\": ");
                json_str(&b, p->v[i].source);
                sb_puts(&b, ", \"placed\": ");     /* where the disc's layout puts it; null: at path */
                if (at[i]) json_str(&b, at[i]);
                else sb_puts(&b, "null");

                sb_printf(&b, ", \"changed\": %s, \"kind\": \"%s\", \"bytes\": %llu, \"files\": %ld, \"archived\": ", chg[i] ? "true" : "false",
                          m[i].missing ? "missing" : m[i].folder ? "folder" : "file", (unsigned long long)m[i].bytes, m[i].files);
                if (!ar || m[i].missing) sb_puts(&b, "null}");
                else {
                    sb_printf(&b, "{\"files\": %zu, \"onDiscs\": %zu, \"object\": ", ar[i].files, ar[i].on);
                    if (ar[i].object) json_str(&b, (rec_get(ar[i].object, "Name") ? rec_get(ar[i].object, "Name") : "?"));
                    else sb_puts(&b, "null");
                    sb_printf(&b, ", \"version\": %ld, \"discs\": [", ar[i].object ? atol((rec_get(ar[i].object, "Version") ? rec_get(ar[i].object, "Version") : "0")) : 0L);
                    for (size_t k = 0; k < ar[i].discs.n; k++) {
                        sb_puts(&b, k ? ", " : "");
                        json_str(&b, ar[i].discs.v[k]);
                    }
                    sb_puts(&b, "]}}");
                }
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
        if (rec_get(hd, "Collection")) printf("  the next edition of %s", rec_get(hd, "Collection"));
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
            if (plan_layout(p, d)) printf("  laid out by %s\n", plan_layout(p, d));
            for (size_t i = 0; i < p->n; i++) {
                if (p->v[i].disc != d) continue;
                char sz[32];
                human_size(m[i].bytes, sz);
                if (m[i].missing) {
                    printf("  %s  MISSING  %s\n", p->v[i].path, p->v[i].source);
                    missing++;
                } else if (m[i].folder) {
                    printf("  %s/  %s, %ld file%s  %s", p->v[i].path, sz, m[i].files,
                           m[i].files == 1 ? "" : "s", p->v[i].source);
                } else {
                    printf("  %s  %s  %s", p->v[i].path, sz, p->v[i].source);
                }
                if (!m[i].missing) {
                    if (chg[i]) printf("  CHANGED since planned");
                    putchar('\n');
                    for (size_t j = 0; m[i].folder && j < p->n; j++) {   /* what of it another item places */
                        const plan_item *in = &p->v[j];
                        if (holder(p, in) != &p->v[i]) continue;
                        printf("      not %s: placed as %s on disc %d\n", in->source + strlen(p->v[i].source) + 1, in->path, in->disc);
                    }
                    {
                        const char *rdisc = NULL;
                        const rec_record *was = p->v[i].of ? object_at_uuid(&scat, p->v[i].of)
                                                           : restored_object(&scat, p->v[i].source, &rdisc);
                        if (was)
                            printf("      continues data object %s version %s (%s)\n", object_slug(&scat, was),
                                   get_or(was, "Version", "?"), p->v[i].of ? "said with --of" : "restored from that disc");
                    }
                    if (at[i]) printf("      -> data/%s%s\n", at[i], m[i].folder ? "/" : "");
                    else if (plan_layout(p, d)) printf("      by hand: it has a value for no alternative of the layout\n");
                    for (size_t j = 0; j < i; j++)  /* two items at one place: only a layout can do that */
                        if (p->v[j].disc == d && !strcmp(at[j] ? at[j] : p->v[j].path, at[i] ? at[i] : p->v[i].path))
                            printf("      TWO ITEMS at data/%s (with %s): give one another name: arv plan rename %s %s NEWPATH\n",
                                   at[i] ? at[i] : p->v[i].path, p->v[j].path, p->name, p->v[i].path);
                    if (ar && ar[i].object) {
                        printf("      archived already: %s version %s, on", (rec_get(ar[i].object, "Name") ? rec_get(ar[i].object, "Name") : "?"),
                               (rec_get(ar[i].object, "Version") ? rec_get(ar[i].object, "Version") : "0"));
                        for (size_t k = 0; k < ar[i].discs.n; k++) printf("%s %s", k ? "," : "", ar[i].discs.v[k]);
                        putchar('\n');
                    } else if (ar && ar[i].files) {
                        if (ar[i].on == ar[i].files) printf("      every file on discs already (not as one data object)\n");
                        else if (ar[i].on) printf("      %zu of %zu files on discs already\n", ar[i].on, ar[i].files);
                        else printf("      on no disc yet\n");
                    }
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
                printf("%zu item%s changed since planned: the discs get %s as %s now\n",
                       changed, changed == 1 ? "" : "s", changed == 1 ? "it" : "them", changed == 1 ? "it is" : "they are");
            printf("Next: arv plan make %s (one image a disc; arv make's options apply)\n", p->name);
        }
        if (!rec_get(hd, "Made") && p->n)
            puts("Planned, not archived: keep the originals until the discs are burned and read back (arv todo says when).");
    }
    for (size_t i = 0; i < p->n; i++) free(at[i]);
    free(at);
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
                if (p->v[i].disc == d) used += item_size(p, &p->v[i]).sectors;
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
        if (p->v[i].disc == disc) used += item_size(p, &p->v[i]).sectors;
    if (cap) printf("%s: disc %d, now %ld%% full%s\n", what, disc, (used * 100 + cap - 1) / cap, used > cap ? " (OVER)" : "");
    else printf("%s: disc %d\n", what, disc);
}

/* ------------------------------------------------------------------ arv plan */

/* puts one source on a disc of the plan, under path (default: its own name); dies on anything
 * wrong, before the plan is saved, so a list of them goes in whole or not at all */
static void add_item(const arv_home *h, disc_plan *p, const char *given, const char *as, const char *disc, int quiet)
{
    char *src = realpath(given, NULL);
    struct stat st;
    if (!src || stat(src, &st) || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) die("%s is not a file or folder", given);
    if (strpbrk(src, "\r\n") || (as && strpbrk(as, "\r\n"))) die("%s: a name with a line break cannot be planned", src);
    for (size_t j = 0; j < p->n; j++)
        if (!strcmp(p->v[j].source, src)) {
            char n[16];
            snprintf(n, sizeof n, "%d", p->v[j].disc);
            die2("%s is in the plan already, on disc %s (arv plan rename gives it another place)", src, n);
        }
    char *base = strrchr(src, '/') ? strrchr(src, '/') + 1 : src;
    const char *path = as && *as ? as : *base ? base : NULL;
    if (!path || !path_ok(path, S_ISDIR(st.st_mode)))
        die("%s cannot be a path under data/ (relative, no . or .. parts; . only for a folder's contents)", path ? path : "/");
    plan_item it = { 0, src, xstrdup(path), NULL, NULL, NULL };
    int d = pick_disc(h, p, disc, item_size(p, &it).sectors);
    check_free(p, path, d);
    if (d > p->discs) p->discs = d;
    it.disc = d;
    it.seen = stamp_of(it.source);
    p->v = xrealloc(p->v, (p->n + 1) * sizeof *p->v);
    p->v[p->n++] = it;
    if (quiet) return;
    char *what = xprintf("%s%s", path, S_ISDIR(st.st_mode) && strcmp(path, ".") ? "/" : "");
    say_where(h, p, d, what);
    free(what);
}

/* the regular files under a folder, relative to it, sorted; its own .arv left out; links counted */
static void each_walk(const char *disk, const char *rel, strlist *out, size_t *links)
{
    DIR *d = opendir(disk);
    struct dirent *e;
    strlist names = { 0 };
    while (d && (e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && (*rel || strcmp(e->d_name, ".arv"))) strlist_add(&names, e->d_name);
    if (d) closedir(d);
    if (names.n) qsort(names.v, names.n, sizeof *names.v, by_str);
    for (size_t i = 0; i < names.n; i++) {
        char *q = join(disk, names.v[i]), *r = *rel ? join(rel, names.v[i]) : xstrdup(names.v[i]);
        struct stat st;
        if (!lstat(q, &st) && S_ISDIR(st.st_mode)) each_walk(q, r, out, links);
        else if (!lstat(q, &st) && S_ISREG(st.st_mode)) strlist_add(out, r);
        else (*links)++;
        free(q);
        free(r);
    }
    strlist_free(&names);
}

/* arv plan add NAME DIR --each [--as ROOT]: each file under DIR an item of its own (one data object,
 * placed on its own by a layout), arranged as ROOT/its path in DIR (ROOT: DIR's name) */
static void add_each(const arv_home *h, disc_plan *p, const char *dir, const char *as, const char *disc)
{
    char *src = realpath(dir, NULL);
    struct stat st;
    if (!src || stat(src, &st) || !S_ISDIR(st.st_mode)) die("%s is not a folder (--each adds each file in a folder)", dir);
    strlist files = { 0 };
    size_t links = 0, first = p->n;
    each_walk(src, "", &files, &links);
    const char *root = as && *as ? as : strrchr(src, '/') + 1;
    for (size_t i = 0; i < files.n; i++) {
        char *abs = join(src, files.v[i]), *at = !strcmp(root, ".") ? xstrdup(files.v[i]) : join(root, files.v[i]);
        add_item(h, p, abs, at, disc, 1);   /* dies before any is saved */
        free(abs);
        free(at);
    }
    int lo = 0, hi = 0;
    for (size_t i = first; i < p->n; i++) {
        if (!lo || p->v[i].disc < lo) lo = p->v[i].disc;
        if (p->v[i].disc > hi) hi = p->v[i].disc;
    }
    if (lo == hi) printf("%zu item%s from %s, one for each file, on disc %d", files.n, files.n == 1 ? "" : "s", dir, lo);
    else printf("%zu items from %s, one for each file, on discs %d to %d", files.n, dir, lo, hi);
    putchar('\n');
    if (links) printf("  %zu link%s or special file%s left out (add %s itself to keep them, as one item)\n", links,
                      links == 1 ? "" : "s", links == 1 ? "" : "s", dir);
    strlist_free(&files);
    free(src);
}

/* arv plan add NAME --list FILE|-: SOURCE <TAB> PATH [<TAB> DISC] a line, # a comment; what a
 * script, a spreadsheet or exiftool sorted, added as one change */
static void add_list(const arv_home *h, disc_plan *p, const char *file, const char *disc)
{
    FILE *in = strcmp(file, "-") ? fopen(file, "r") : stdin;
    if (!in) die("cannot read %s", file);
    char *line = NULL;
    size_t cap = 0, n = 0, first = p->n;
    ssize_t len;
    strlist src = { 0 }, as = { 0 }, on = { 0 };
    while ((len = getline(&line, &cap, in)) >= 0) {     /* every line read and its source found first */
        n++;
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (!len || line[0] == '#') continue;
        char *cols[3] = { line, NULL, NULL };
        int k = split_tabs(line, cols, 3);
        if (k < 1 || !*cols[0]) continue;
        if (access(cols[0], F_OK)) {
            char at[24];
            snprintf(at, sizeof at, "%zu", n);
            die2("%s, line %s: no such file or folder (nothing was added)", cols[0], at);
        }
        strlist_add(&src, cols[0]);
        strlist_add(&as, k > 1 ? cols[1] : "");
        strlist_add(&on, k > 2 && *cols[2] ? cols[2] : disc ? disc : "auto");
    }
    if (in != stdin) fclose(in);
    free(line);
    for (size_t i = 0; i < src.n; i++) add_item(h, p, src.v[i], as.v[i], on.v[i], 1);   /* dies before any is saved */
    for (size_t i = first; i < p->n; i++) say_where(h, p, p->v[i].disc, p->v[i].path);
    printf("%zu item%s added from %s\n", src.n, src.n == 1 ? "" : "s", strcmp(file, "-") ? file : "standard input");
    strlist_free(&src);
    strlist_free(&as);
    strlist_free(&on);
}

/* the plan's tags for a source (a file or folder in the plan, as given), as typed: each normalised,
 * aliases made names; refused with two values of a one-valued namespace, or a date that is not
 * EDTF. Empty tags take the source's away. Returns 1 when they changed. */
static int set_tags(const arv_home *h, disc_plan *p, const char *given, const char *text, const strlist *one, const char *where)
{
    char *src = realpath(given, NULL);
    if (!src) die2("%s%s: no such file or folder (nothing was changed)", given, where);
    int in = 0;
    for (size_t i = 0; i < p->n && !in; i++) {     /* in an item, or holding items (a folder added --each) */
        size_t l = strlen(p->v[i].source), m = strlen(src);
        in = (!strncmp(src, p->v[i].source, l) && (!src[l] || src[l] == '/'))
             || (!strncmp(p->v[i].source, src, m) && p->v[i].source[m] == '/');
    }
    if (!in) die2("%s%s is not in the plan (arv plan add it first; nothing was changed)", src, where);
    strlist tags = { 0 };
    char *copy = xstrdup(text);
    for (char *t = strtok(copy, ","); t; t = strtok(NULL, ",")) {
        char *n = tag_normalise(t);
        if (*n) strlist_add(&tags, n);
        free(n);
    }
    free(copy);
    tags_canonical(h, &tags);
    for (size_t i = 0; i < tags.n; i++) {
        size_t l = ns_of(tags.v[i]);
        if (!l) continue;
        char *ns = xprintf("%.*s", (int)l, tags.v[i]);
        for (size_t k = i + 1; k < tags.n && strlist_has(one, ns); k++)
            if (ns_of(tags.v[k]) == l && !strncmp(tags.v[k], ns, l))
                die("%s", xprintf("%s%s: %s and %s: %s takes one value (nothing was changed)", given, where, tags.v[i], tags.v[k], ns));
        long a, b;
        if (!strcmp(ns, "date") && edtf_span(tags.v[i] + 5, &a, &b) != 1)
            die("%s", xprintf("%s%s: %s is not an EDTF date or range (2019-07, 1995/2008, 197X, 1995~; nothing was changed)",
                              given, where, tags.v[i]));
        free(ns);
    }
    sbuf b = { 0 };
    sb_puts(&b, "");
    for (size_t i = 0; i < tags.n; i++) sb_printf(&b, "%s%s", i ? ", " : "", tags.v[i]);
    strlist_free(&tags);
    size_t k = 0;
    while (k < p->tag_src.n && strcmp(p->tag_src.v[k], src)) k++;
    int changed = 1;
    if (k < p->tag_src.n && !strcmp(p->tag_val.v[k], b.s)) {
        changed = 0;
    } else if (k < p->tag_src.n && !*b.s) {     /* none now: its record goes */
        free(p->tag_src.v[k]);
        free(p->tag_val.v[k]);
        for (size_t j = k + 1; j < p->tag_src.n; j++) {
            p->tag_src.v[j - 1] = p->tag_src.v[j];
            p->tag_val.v[j - 1] = p->tag_val.v[j];
        }
        p->tag_src.n--;
        p->tag_val.n--;
    } else if (k < p->tag_src.n) {
        free(p->tag_val.v[k]);
        p->tag_val.v[k] = xstrdup(b.s);
    } else if (*b.s) {
        strlist_add(&p->tag_src, src);
        strlist_add(&p->tag_val, b.s);
    } else {
        changed = 0;
    }
    free(b.s);
    free(src);
    return changed;
}

int cmd_plan(int argc, char **argv)
{
    const char *given = NULL, *action = NULL, *name = NULL, *disc = NULL, *as = NULL, *from_s = NULL, *list = NULL, *of = NULL;
    int as_json = 0, all = 0, on_discs = 0, each = 0, dups = 0;
    strlist args = { 0 }, defaults = { 0 }, rest = { 0 };
    static const char *const SETTINGS[][2] = { { "--medium", "Medium" }, { "--set", "Set" }, { "--title", "Title" },
                                               { "--description", "Description" }, { "--access", "Access" },
                                               { "--collection", "Collection" } };   /* the last: plan make reads it */
    int i = 0;
    for (; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!strcmp(argv[i], "--json")) as_json = 1;
        else if (!strcmp(argv[i], "--all")) all = 1;
        else if (!strcmp(argv[i], "--archived")) on_discs = 1;
        else if (!strcmp(argv[i], "--each")) each = 1;
        else if (!strcmp(argv[i], "--duplicates")) dups = 1;
        else if (!action) action = argv[i];
        else if (!name) {
            name = argv[i];
            if (!strcmp(action, "make")) {          /* the rest is arv make's */
                i++;
                break;
            }
        } else if (i + 1 < argc && !strcmp(argv[i], "--disc")) disc = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--as")) as = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--of")) of = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--from")) from_s = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--discs")) disc = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--list")) list = argv[++i];
        else {
            int setting = 0;
            for (int k = 0; k < 6 && !setting; k++)
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
    for (size_t k = 0; k + 1 < defaults.n; k += 2)     /* a collection: one there is, as its code */
        if (!strcmp(defaults.v[k], "Collection") && *defaults.v[k + 1]) {
            archive cat;
            archive_load(&cat, h.rec_path);
            const rec_record *c = archive_collection(&cat, defaults.v[k + 1]);
            if (!c) die("--collection %s: no such collection (arv collection list)", defaults.v[k + 1]);
            free(defaults.v[k + 1]);
            defaults.v[k + 1] = xstrdup(rec_get(c, "Code"));
        }

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
        if (!names.n && !as_json) printf("No plans (arv plan new NAME)\n");
        sbuf json = { 0 };
        int first_json = 1;
        if (as_json) sb_puts(&json, "{\"plans\": [");
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
            if (as_json) {
                sb_puts(&json, first_json ? "" : ", ");
                first_json = 0;
                sb_puts(&json, "{\"name\": "); json_str(&json, p.name);
                sb_printf(&json, ", \"discs\": %d, \"items\": %zu, \"made\": ", p.discs, p.n);
                if (rec_get(hd, "Made")) json_str(&json, rec_get(hd, "Made"));
                else sb_puts(&json, "null");
                sb_puts(&json, "}");
            } else {
                printf("%s  %d disc%s, %zu item%s%s%s%s\n", p.name, p.discs, p.discs == 1 ? "" : "s", p.n, p.n == 1 ? "" : "s",
                       rec_get(hd, "Title") ? "  \"" : "", rec_get(hd, "Title") ? rec_get(hd, "Title") : "",
                       rec_get(hd, "Title") ? "\"" : "");
                if (rec_get(hd, "Made")) printf("    made %s (arv plan again %s NEW: the same again, as a new plan)\n", rec_get(hd, "Made"), p.name);
            }
            free(f);
        }
        if (as_json) { sb_puts(&json, "]}\n"); fputs(json.s, stdout); free(json.s); }
        if (hidden && !as_json) printf("(%zu made plan%s not shown: arv plan list --all)\n", hidden, hidden == 1 ? "" : "s");
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
        if (unlink(file)) die("cannot delete %s", file);
        printf("Plan %s deleted (its sources are untouched)\n", name);
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
        for (size_t k = 0; k < p.n; k++) {
            plan_item it = p.v[k];
            it.seen = stamp_of(it.source);
            it.placed = NULL;
            q.v[q.n++] = it;
        }
        for (size_t k = 0; k < p.tag_src.n; k++) {
            strlist_add(&q.tag_src, p.tag_src.v[k]);
            strlist_add(&q.tag_val, p.tag_val.v[k]);
        }
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
        if (dups) show_duplicates(&h, &p);
        else show(&h, &p, as_json, on_discs);
        return 0;
    }
    if (!strcmp(action, "add")) {
        if (!args.n && !list) die("%s", "arv plan add NAME SOURCE... (or --list FILE): the files or folders to put on a disc");
        if (as && (args.n > 1 || list)) die("%s", "--as names one thing: add one SOURCE at a time with it");
        if (each && list) die("%s", "--each is for folders given as SOURCE, not --list");
        if (of && (args.n != 1 || each || list)) die("%s", "--of says what one SOURCE is: add one at a time with it");
        const char *of_uuid = NULL;         /* the data object it continues: a copy you made of it, or restored */
        archive ocat;
        if (of) {
            archive_load(&ocat, h.rec_path);
            of_uuid = object_by_word(&ocat, of);
        }
        for (size_t k = 0; k < args.n; k++) {
            if (each) add_each(&h, &p, args.v[k], as, disc);
            else add_item(&h, &p, args.v[k], as, disc, 0);
        }
        if (of_uuid) {
            p.v[p.n - 1].of = xstrdup(of_uuid);
            printf("  it continues data object %s (%s): the same bytes are another copy of its version, changed ones its next\n",
                   object_slug(&ocat, object_at_uuid(&ocat, of_uuid)), of_uuid);
        }
        if (list) add_list(&h, &p, list, disc);
        plan_save(&p);
        return 0;
    }
    if (!strcmp(action, "tag")) {           /* tags for files still on a drive: the plan keeps them */
        strlist one = { 0 };
        tag_namespaces(&h, &one, NULL, NULL);
        if (list) {
            if (args.n) die("%s", "arv plan tag NAME --list FILE|-: SOURCE<TAB>TAGS a line (no SOURCE besides)");
            FILE *in = strcmp(list, "-") ? fopen(list, "r") : stdin;
            if (!in) die("cannot read %s", list);
            char *line = NULL;
            size_t cap = 0, n = 0, changed = 0, lines = 0;
            ssize_t len;
            while ((len = getline(&line, &cap, in)) >= 0) {   /* all or none: a refusal dies before the plan is saved */
                n++;
                while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
                if (!len || line[0] == '#') continue;
                char *cols[2] = { line, NULL };
                if (split_tabs(line, cols, 2) < 1 || !*cols[0]) continue;
                char at[32];
                snprintf(at, sizeof at, ", line %zu", n);
                changed += set_tags(&h, &p, cols[0], cols[1] ? cols[1] : "", &one, at);
                lines++;
            }
            if (in != stdin) fclose(in);
            free(line);
            plan_save(&p);
            printf("%zu line%s read from %s: tags of %zu changed\n", lines, lines == 1 ? "" : "s",
                   strcmp(list, "-") ? list : "standard input", changed);
        } else {
            if (args.n != 2) die("%s", "arv plan tag NAME SOURCE TAGS: the tags, comma-separated (\"\" for none)");
            int changed = set_tags(&h, &p, args.v[0], args.v[1], &one, "");
            plan_save(&p);
            char *src = realpath(args.v[0], NULL);
            size_t k = 0;
            while (k < p.tag_src.n && strcmp(p.tag_src.v[k], src)) k++;
            printf("%s: %s%s\n", args.v[0], k < p.tag_src.n ? p.tag_val.v[k] : "no tags", changed ? "" : " (as it was)");
            free(src);
        }
        strlist_free(&one);
        return 0;
    }
    if (!strcmp(action, "layout")) {        /* a disc's tree made from its items' tags */
        if (args.n != 1) die("%s", "arv plan layout NAME LAYOUT [--disc N]: e.g. 'year/event, year'; '' places by hand again");
        const char *layout = args.v[0];
        if (*layout) {
            strlist one = { 0 };
            tag_namespaces(&h, &one, NULL, NULL);
            char *problem = layout_problem(layout, &one);
            if (problem) die2("layout %s: %s", layout, problem);
            strlist_free(&one);
        }
        int on = disc ? atoi(disc) : 0;
        if (disc && (on < 1 || on > p.discs)) die("--disc %s: the plan's discs are 1 to its last (arv plan show)", disc);
        size_t kept = 0;                    /* the Layout fields this does not replace */
        for (size_t f = 0; f < hd->nfields; f++) {
            rec_field *x = &hd->fields[f];
            char *end;
            long n = strtol(x->value, &end, 10);
            int per_disc = end != x->value && *end == ' ';
            if (!strcmp(x->name, "Layout") && (!on || (per_disc && n == on))) {
                free(x->name);
                free(x->value);
                continue;
            }
            hd->fields[kept++] = *x;
        }
        hd->nfields = kept;
        if (*layout) {
            char *v = on ? xprintf("%d %s", on, layout) : xstrdup(layout);
            rec_add(hd, "Layout", v);
            free(v);
        }
        plan_save(&p);
        const char *which = on ? disc : "Every disc";
        if (*layout) printf("%s%s laid out by %s: each item placed whole by its tags. Where: arv plan show %s\n",
                            on ? "Disc " : "", which, layout, name);
        else printf("%s%s placed by hand again%s\n", on ? "Disc " : "", which,
                    on && plan_layout(&p, on) ? " (the plan's layout still applies to it)" : "");
        return 0;
    }
    if (!strcmp(action, "rename")) {        /* a new place on its disc; inside a planned folder: split off */
        if (args.n != 2) die("%s", "arv plan rename NAME PATH NEWPATH [--from N]");
        const char *path = args.v[0], *to = args.v[1];
        int from = from_s ? atoi(from_s) : 0, count = 0;
        plan_item *it = NULL;
        for (size_t k = 0; k < p.n; k++)
            if (!strcmp(p.v[k].path, path) && (!from || p.v[k].disc == from)) it = &p.v[k], count++;
        if (count > 1) die("%s is on more than one disc: say which with --from N", path);
        if (it) {
            struct stat st;
            int folder = !stat(it->source, &st) && S_ISDIR(st.st_mode);
            if (!path_ok(to, folder))
                die("%s cannot be a path under data/ (relative, no . or .. parts; . only for a folder's contents)", to);
            if (!strcmp(it->path, to)) return 0;
            check_free(&p, to, it->disc);
            printf("%s -> %s (disc %d; the source is untouched)\n", it->path, to, it->disc);
            free(it->path);
            it->path = xstrdup(to);
        } else {                            /* a file or folder inside a planned folder */
            const plan_item *in = NULL;
            const char *under = NULL;
            for (size_t k = 0; k < p.n; k++) {
                const char *ip = p.v[k].path;
                size_t n = strlen(ip);
                const char *r = !strcmp(ip, ".") ? path : !strncmp(path, ip, n) && path[n] == '/' ? path + n + 1 : NULL;
                if (r && *r && (!from || p.v[k].disc == from) && (!in || strlen(ip) > strlen(in->path))) in = &p.v[k], under = r;
            }
            if (!in) die("nothing at %s in the plan (arv plan show)", path);
            char *src = join(in->source, under), dn[16];
            snprintf(dn, sizeof dn, "%d", in->disc);
            if (access(src, F_OK)) die("%s: no such file or folder in the planned folder", src);
            printf("%s (in %s/) -> ", path, in->path);
            fflush(stdout);
            add_item(&h, &p, src, to, dn, 0);
            free(src);
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

/* has a person or a model described the object (a metadata modification event naming it)? */
static int object_described(const archive *cat, const char *uuid)
{
    char *key = xprintf("object:%s", uuid);
    int yes = 0;
    for (size_t i = 0; i < cat->events.n && !yes; i++) {
        const char *o = rec_get(cat->events.v[i], "Object"), *t = rec_get(cat->events.v[i], "Type");
        yes = o && t && !strcmp(o, key) && !strcmp(t, "metadata modification");
    }
    free(key);
    return yes;
}

/* an object's description (after Date): a copy of a version has the version's; else what its own
 * files say (dir: NULL for a file), with a person's or a model's Title, Description and Notes
 * carried over from the newest earlier version (uuid: of a new version, its lineage; else NULL) */
static void object_about(rec_record *r, const rec_record *same, const char *uuid, long latest, const archive *cat,
                         const recs *made_now, const char *dir, int git)
{
    if (same) {
        for (size_t f = 0; f < same->nfields; f++) {
            const char *n = same->fields[f].name;
            for (int k = 0; ABOUT_FIELDS[k]; k++)
                if (!strcmp(n, ABOUT_FIELDS[k])) rec_add(r, n, same->fields[f].value);
            if (!strcmp(n, "Note")) rec_add(r, n, same->fields[f].value);
        }
        return;
    }
    about_info a = { 0 };
    if (dir) about_read(dir, git, &a);
    const char *values[5];
    about_fields(&a, values);
    const rec_record *before = NULL;
    if (uuid && object_described(cat, uuid)) {
        const recs *both[2] = { &cat->objects, made_now };
        for (int b = 0; b < 2; b++)
            for (size_t i = 0; i < both[b]->n; i++) {
                const rec_record *o = both[b]->v[i];
                if (rec_get(o, "Uuid") && !strcmp(rec_get(o, "Uuid"), uuid) && rec_get(o, "Version")
                    && atol(rec_get(o, "Version")) == latest)
                    before = o;
            }
    }
    if (before) {
        values[0] = rec_get(before, "Title") ? rec_get(before, "Title") : values[0];
        values[1] = rec_get(before, "Description") ? rec_get(before, "Description") : values[1];
    }
    for (int k = 0; ABOUT_FIELDS[k]; k++)
        if (values[k]) rec_add(r, ABOUT_FIELDS[k], values[k]);
    for (size_t f = 0; before && f < before->nfields; f++)
        if (!strcmp(before->fields[f].name, "Note")) rec_add(r, "Note", before->fields[f].value);
    about_free(&a);
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
            const char *p = files->v[i].origin ? files->v[i].origin : files->v[i].path, *rel;
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
        /* its lineage, from where it was read: the same content as a version of it is that version
         * (a copy); changed, the next version; from nowhere archived before, a new object. Identity
         * is the lineage, never the bytes: the same bytes in another lineage (another source) are
         * another object, noted as SameBytes, for a person to say whether they are one. An empty
         * object is never the same as another. */
        const rec_record *same = NULL, *from = NULL, *twin = NULL;
        long latest = 0;
        char *stored = path_rel(dp->root, it->source);   /* as the home keeps it */
        const recs *both[2] = { &cat->objects, made_now };
        for (int b = 0; b < 2; b++)
            for (size_t i = 0; i < both[b]->n; i++) {
                const rec_record *o = both[b]->v[i];
                if (it->of ? rec_get(o, "Uuid") && !strcmp(rec_get(o, "Uuid"), it->of)   /* said to be it (--of) */
                           : rec_get(o, "Source") && !strcmp(rec_get(o, "Source"), stored))
                    from = o;
            }
        if (!from && !it->of) {             /* restored from a disc (its .arv marker): the object it was there */
            const rec_record *was = restored_object(cat, it->source, NULL);
            for (int b = 0; was && b < 2; b++)    /* the newest of that lineage */
                for (size_t i = 0; i < both[b]->n; i++)
                    if (rec_get(both[b]->v[i], "Uuid") && !strcmp(rec_get(both[b]->v[i], "Uuid"), rec_get(was, "Uuid")))
                        from = both[b]->v[i];
        }
        const recs *any[3] = { &cat->objects, made_now, out };    /* this disc's own so far too */
        for (int b = 0; b < 3; b++)
            for (size_t i = 0; lines.n && i < any[b]->n; i++) {
                const rec_record *o = any[b]->v[i];
                if (!rec_get(o, "Tree") || strcmp(rec_get(o, "Tree"), tree)) continue;
                if (from && rec_get(o, "Uuid") && !strcmp(rec_get(o, "Uuid"), rec_get(from, "Uuid"))) same = o;
                else twin = o;
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
        const char *from_path = it->source;
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
        rec_add(r, "Path", it->placed ? it->placed : it->path);
        {                                   /* its name on the disc, when not the one it was read as */
            const char *at = it->placed ? it->placed : it->path, *base = strrchr(at, '/') ? strrchr(at, '/') + 1 : at;
            if (strcmp(at, ".") && strcmp(base, rec_get(r, "Name"))) rec_add(r, "Slug", base);
        }
        if (it->placed) rec_add(r, "Arranged", it->path);     /* its path as arranged, before the layout placed it */
        rec_add(r, "Files", n_files);
        rec_add(r, "Bytes", n_bytes);
        rec_add(r, "Date", today);
        object_about(r, same, from ? uuid : NULL, latest, cat, made_now, is_file ? NULL : it->source, git);
        if (!same && twin) {                /* the same bytes, another lineage: noted, not merged */
            char *v = xprintf("%s %s", rec_get(twin, "Uuid"), rec_get(twin, "Version") ? rec_get(twin, "Version") : "1");
            rec_add(r, "SameBytes", v);
            free(v);
        }
        rec_add(r, "Source", stored);
        for (size_t j = 0; !is_file && j < dp->n; j++)      /* what of the folder other items placed */
            if (holder(dp, &dp->v[j]) == it) rec_add(r, "Except", dp->v[j].source + strlen(it->source) + 1);
        recs_add(out, r);
        strlist_add(manifests, text.s);
        char *where = it->placed ? xprintf("data/%s, by the layout", it->placed) : xprintf("data/%s", top ? "" : it->path);
        fprintf(stderr, "Object %s%s (%s on %s): %s\n", rec_get(r, "Name"), is_file ? "" : "/", where, disc_id,
                same ? "the same as a version already archived (another copy of it)"
                : from ? "a new version of what was archived from there before" : "new");
        if (!same && twin)
            fprintf(stderr, "  the same bytes as %s%s version %s on %s, archived from elsewhere: recorded as another object "
                            "(SameBytes), since the bytes cannot say whether it is the same thing (a copy you made of it "
                            "is: plan it with plan add --of)\n",
                    get_or(twin, "Name", "?"), strcmp(get_or(twin, "Kind", ""), "file") ? "/" : "", get_or(twin, "Version", "?"),
                    get_or(twin, "Disc", "?"));
        free(where);
        sbuf said = { 0}, facts = { 0 };         /* "Title": description (coverage; rights; origin) */
        const char *t = rec_get(r, "Title"), *d = rec_get(r, "Description");
        if (t) sb_printf(&said, "\"%s\"%s", t, d ? ": " : "");
        if (d) sb_puts(&said, d);
        for (int f = 2; ABOUT_FIELDS[f]; f++)
            if (rec_get(r, ABOUT_FIELDS[f])) sb_printf(&facts, "%s%s", facts.len ? "; " : "", rec_get(r, ABOUT_FIELDS[f]));
        if (said.len || facts.len)
            fprintf(stderr, "  about it: %s%s%s%s\n", said.len ? said.s : "", said.len && facts.len ? " (" : "",
                    facts.len ? facts.s : "", said.len && facts.len ? ")" : "");
        free(said.s);
        free(facts.s);
        free(text.s);
        free(stored);
        strlist_free(&lines);
    }
}
