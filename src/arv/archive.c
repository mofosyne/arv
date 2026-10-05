/* The catalogue as arv writes it: Disc, Binding, Location, Selection, Collection, Revision,
 * Event and Appraisal records, written in that order under the shared descriptors
 * (descriptors.rec), and the views of it a disc may carry (access levels, snapshots). */
#define _XOPEN_SOURCE 700
#include "arv.h"
#include "data.h"

#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void recs_add(recs *l, rec_record *r)
{
    l->v = xrealloc(l->v, (l->n + 1) * sizeof *l->v);
    l->v[l->n++] = r;
}

int recs_has(const recs *l, const rec_record *r)
{
    for (size_t i = 0; i < l->n; i++)
        if (l->v[i] == r) return 1;
    return 0;
}

rec_record *rec_alloc(const char *type)
{
    rec_record *r = calloc(1, sizeof *r);
    if (!r) die("%s", "out of memory");
    r->type = type;
    return r;
}

/* the shared descriptor of a record type (descriptors.rec), or NULL */
rec_record *descriptor(const char *type)
{
    static rec_file all;
    static int loaded;
    if (!loaded) {
        int bad = 0;
        if (rec_parse(DATA_DESCRIPTORS, &all, &bad)) die("%s", "the built-in descriptors do not parse");
        loaded = 1;
    }
    for (size_t i = 0; i < all.nrecords; i++)
        if (all.records[i].descriptor && all.records[i].type && !strcmp(all.records[i].type, type))
            return &all.records[i];
    return NULL;
}

static const char *const TYPES[] = { "Disc", "Binding", "Location", "Selection", "Collection", "Revision",
                                     "Event", "Appraisal" };
enum { NTYPES = 8, EVENT_TYPE = 6 };

static recs *group(archive *a, const char *type)
{
    recs *g[] = { &a->discs, &a->bindings, &a->locations, &a->selections, &a->collections, &a->revisions,
                  &a->events, &a->appraisals };
    for (int i = 0; i < NTYPES; i++)
        if (!strcmp(type, TYPES[i])) return g[i];
    return NULL;
}

void archive_load(archive *a, const char *path)
{
    int bad = 0;
    memset(a, 0, sizeof *a);
    if (access(path, F_OK)) return;                 /* a new home: an empty catalogue */
    if (rec_read(path, &a->file, &bad)) {
        if (errno == EINVAL) {
            fprintf(stderr, "Error: %s: line %d is not a field\n", path, bad);
            exit(1);
        }
        die("cannot read %s", path);
    }
    for (size_t i = 0; i < a->file.nrecords; i++) {
        rec_record *r = &a->file.records[i];
        if (!r->descriptor && r->type && !strcmp(r->type, "Collection") && !rec_get(r, "Uuid"))
            r->type = "Selection";          /* format 0.4 and earlier: virtual folders were Collection */
        recs *g = r->descriptor || !r->type ? NULL : group(a, r->type);
        if (g) recs_add(g, r);
    }
}

/* every record in file order: each type's descriptor, then its records (as Catalog.records) */
void archive_records(const archive *a, recs *out)
{
    const recs *g[] = { &a->discs, &a->bindings, &a->locations, &a->selections, &a->collections, &a->revisions,
                        &a->events, &a->appraisals };
    for (int i = 0; i < NTYPES; i++) {
        int always = i == 0 || i == EVENT_TYPE;     /* Disc and Event descriptors are always written */
        if (!always && !g[i]->n) continue;
        recs_add(out, descriptor(TYPES[i]));
        for (size_t k = 0; k < g[i]->n; k++) recs_add(out, g[i]->v[k]);
    }
}

void write_records(const char *path, const recs *r)
{
    char *tmp = xprintf("%s.tmp", path);
    if (rec_write(tmp, r->v, r->n) || rename(tmp, path)) die("cannot write %s", path);
    free(tmp);
}

void archive_save(const archive *a, const char *path)
{
    char *dir = xstrdup(path), *slash = strrchr(dir, '/');   /* a new home: its catalog/ folder first */
    if (slash && slash != dir) {
        *slash = 0;
        if (mkdirs(dir)) die("cannot create %s", dir);
    }
    free(dir);
    recs all = { 0 };
    archive_records(a, &all);
    write_records(path, &all);
    free(all.v);
}

static char *norm_code(const char *code)
{
    while (code && isspace((unsigned char)*code)) code++;
    char *p = xstrdup(code ? code : ""), *e = p + strlen(p);
    while (e > p && isspace((unsigned char)e[-1])) *--e = 0;
    for (char *q = p; *q; q++) *q = (char)toupper((unsigned char)*q);
    return p;
}

static rec_record *by_code(const recs *l, const char *code)
{
    char *want = norm_code(code);
    rec_record *found = NULL;
    for (size_t i = 0; i < l->n && !found; i++)
        if (rec_get(l->v[i], "Code") && !strcmp(rec_get(l->v[i], "Code"), want)) found = l->v[i];
    free(want);
    return found;
}

rec_record *archive_disc(const archive *a, const char *id)
{
    for (size_t i = 0; i < a->discs.n; i++)
        if (rec_get(a->discs.v[i], "Id") && !strcmp(rec_get(a->discs.v[i], "Id"), id)) return a->discs.v[i];
    return NULL;
}

rec_record *archive_location(const archive *a, const char *code) { return by_code(&a->locations, code); }

/* next unused sequence number in a set (numbers are never reused) */
long archive_next_number(const archive *a, const char *set)
{
    long most = 0;
    for (size_t i = 0; i < a->discs.n; i++) {
        const rec_record *d = a->discs.v[i];
        if (!rec_get(d, "Set") || strcmp(rec_get(d, "Set"), set)) continue;
        const char *seq = rec_get(d, "Sequence");
        long n = 0;
        if (seq && *seq && strspn(seq, "0123456789") == strlen(seq)) n = atol(seq);
        else {
            discid_parts p;
            if (discid_parse(rec_get(d, "Id") ? rec_get(d, "Id") : "", &p)) n = p.sequence;
        }
        if (n > most) most = n;
    }
    return most + 1;
}

const char *disc_access(const rec_record *d)
{
    static char buf[16];
    const char *v = rec_get(d, "Access");
    if (!v) return "private";
    char *n = norm_code(v);
    for (char *q = n; *q; q++) *q = (char)tolower((unsigned char)*q);
    snprintf(buf, sizeof buf, "%s", n);
    free(n);
    return !strcmp(buf, "public") || !strcmp(buf, "private") || !strcmp(buf, "sealed") ? buf : "private";
}

static int is_sealed(const archive *a, const char *id)
{
    rec_record *d = archive_disc(a, id);
    return d && !strcmp(disc_access(d), "sealed");
}

static int in_list(const strlist *l, const char *s)
{
    return s && strlist_has(l, s);
}

/* the Location records discs refer to, with the places containing them */
void archive_locations_for(const archive *a, const recs *discs, recs *out)
{
    strlist codes = { 0 };
    for (size_t i = 0; i < discs->n; i++)
        for (size_t f = 0; f < discs->v[i]->nfields; f++) {
            if (strcmp(discs->v[i]->fields[f].name, "Location")) continue;
            rec_record *l = archive_location(a, discs->v[i]->fields[f].value);
            for (int depth = 0; l && depth < 64; depth++) {
                if (in_list(&codes, rec_get(l, "Code"))) break;
                strlist_add(&codes, rec_get(l, "Code"));
                l = archive_location(a, rec_get(l, "Parent"));
            }
        }
    for (size_t i = 0; i < a->locations.n; i++)
        if (in_list(&codes, rec_get(a->locations.v[i], "Code"))) recs_add(out, a->locations.v[i]);
    strlist_free(&codes);
}

const char *selection_target(const char *target)
{
    if (!strncmp(target, "selection:", 10)) return target + 10;
    if (!strncmp(target, "collection:", 11)) return target + 11;
    return NULL;
}

/* "DISC-ID:path" -> the disc id (and whether there is a path) */
static char *item_disc(const char *item, int *has_path)
{
    while (isspace((unsigned char)*item)) item++;
    const char *colon = strchr(item, ':');
    size_t n = colon ? (size_t)(colon - item) : strlen(item);
    char *id = xmalloc(n + 1);
    memcpy(id, item, n);
    id[n] = 0;
    while (n && isspace((unsigned char)id[n - 1])) id[--n] = 0;
    const char *path = colon ? colon + 1 : "";
    while (*path == ' ' || *path == '/') path++;
    if (has_path) *has_path = *path != 0;
    return id;
}

/* selections as a snapshot may carry them: items only for disc_ids, no paths on sealed discs */
void archive_selections_for(const archive *a, const strlist *disc_ids, recs *out)
{
    strlist keep_codes = { 0 };
    recs kept = { 0 };
    for (size_t i = 0; i < a->selections.n; i++) {
        rec_record *col = a->selections.v[i], *copy = rec_alloc("Selection");
        int any = 0;
        for (size_t f = 0; f < col->nfields; f++)
            if (strcmp(col->fields[f].name, "Item")) rec_add(copy, col->fields[f].name, col->fields[f].value);
        for (size_t f = 0; f < col->nfields; f++) {
            if (strcmp(col->fields[f].name, "Item")) continue;
            int has_path;
            char *id = item_disc(col->fields[f].value, &has_path);
            if (strlist_has(disc_ids, id) && !(has_path && is_sealed(a, id))) {
                rec_add(copy, "Item", col->fields[f].value);
                any = 1;
            }
            free(id);
        }
        recs_add(&kept, copy);
        if (any)        /* it and every selection it is in */
            for (rec_record *c = col; c; c = by_code(&a->selections, rec_get(c, "Parent"))) {
                if (in_list(&keep_codes, rec_get(c, "Code"))) break;
                strlist_add(&keep_codes, rec_get(c, "Code"));
            }
    }
    for (size_t i = 0; i < a->selections.n; i++)
        if (in_list(&keep_codes, rec_get(a->selections.v[i], "Code"))) recs_add(out, kept.v[i]);
    free(kept.v);
    strlist_free(&keep_codes);
}

static const char *const SEALED_FIELDS[] = { "Id", "Uuid", "IdScheme", "Set", "Category", "Path", "Sequence",
                                            "Coverage", "Date", "Part", "Location", "Copies", "MediaId",
                                            "Access", NULL };

/* what other discs may carry about a sealed disc */
rec_record *sealed_view(const rec_record *d)
{
    rec_record *r = rec_alloc("Disc");
    int title_done = 0;
    for (size_t f = 0; f < d->nfields; f++) {
        for (int k = 0; SEALED_FIELDS[k]; k++)
            if (!strcmp(d->fields[f].name, SEALED_FIELDS[k])) {
                rec_add(r, d->fields[f].name, d->fields[f].value);
                if (!title_done && r->nfields == 1) {      /* Title goes second, after the first field */
                    rec_add(r, "Title", "(sealed disc)");
                    title_done = 1;
                }
                break;
            }
    }
    if (!title_done) rec_add(r, "Title", "(sealed disc)");
    rec_add(r, "Withheld", "title, description, notes, subjects and file lists (Access: sealed)");
    return r;
}

static const char *target_disc(const char *target, char *buf, size_t n)
{
    if (!target || !strncmp(target, "set:", 4) || selection_target(target)) return NULL;
    char *id = item_disc(target, NULL);
    snprintf(buf, n, "%s", id);
    free(id);
    return buf;
}

/* catalog.subset(ids).shared_view(): the prior discs as another disc may carry them */
void archive_shared_subset(const archive *a, const strlist *ids, archive *out)
{
    char buf[256];
    memset(out, 0, sizeof *out);
    strlist sealed = { 0 };
    for (size_t i = 0; i < a->discs.n; i++) {
        rec_record *d = a->discs.v[i];
        if (!in_list(ids, rec_get(d, "Id"))) continue;
        if (!strcmp(disc_access(d), "sealed")) {
            strlist_add(&sealed, rec_get(d, "Id"));
            recs_add(&out->discs, sealed_view(d));
        } else {
            recs_add(&out->discs, d);
        }
    }
    for (size_t i = 0; i < a->bindings.n; i++)
        if (in_list(ids, rec_get(a->bindings.v[i], "Volume"))) recs_add(&out->bindings, a->bindings.v[i]);
    for (size_t i = 0; i < a->events.n; i++) {
        const char *d = rec_get(a->events.v[i], "Disc");
        if (in_list(ids, d) && !in_list(&sealed, d)) recs_add(&out->events, a->events.v[i]);
    }
    for (size_t i = 0; i < a->appraisals.n; i++) {
        const char *d = target_disc(rec_get(a->appraisals.v[i], "Target"), buf, sizeof buf);
        if (in_list(ids, d) && !in_list(&sealed, d)) recs_add(&out->appraisals, a->appraisals.v[i]);
    }
    strlist_free(&sealed);
}

/* every place a disc's copies are kept, as readable paths ("" when not recorded) */
char *archive_where(const archive *a, const rec_record *d)
{
    sbuf out = { 0 };
    sb_puts(&out, "");
    for (size_t f = 0; f < d->nfields; f++) {
        if (strcmp(d->fields[f].name, "Location")) continue;
        if (out.len) sb_puts(&out, "; ");
        const rec_record *chain[64];
        size_t n = 0;
        for (rec_record *l = archive_location(a, d->fields[f].value); l && n < 64;
             l = archive_location(a, rec_get(l, "Parent"))) {
            size_t k;
            for (k = 0; k < n && chain[k] != l; k++) {}
            if (k < n) break;
            chain[n++] = l;
        }
        if (!n) sb_puts(&out, d->fields[f].value);
        while (n--) {
            const char *name = rec_get(chain[n], "Name") ? rec_get(chain[n], "Name") : rec_get(chain[n], "Code");
            sb_printf(&out, "%s%s", name, n ? " / " : "");
        }
    }
    return out.s;
}

rec_record *new_event(const char *disc_id, const char *type, const char *outcome, const char *agent,
                             const char *authorship, const char *note)
{
    char today[11];
    today_iso(today);
    rec_record *r = rec_alloc("Event");
    rec_add(r, "Disc", disc_id);
    rec_add(r, "Type", type);
    rec_add(r, "Date", today);
    rec_add(r, "Outcome", outcome);
    rec_add(r, "Authorship", authorship);
    rec_add(r, "Agent", agent);
    if (note) rec_add(r, "Note", note);
    return r;
}

/* human:LOGIN, found as Python's getpass.getuser() finds it */
char *person(void)
{
    const char *vars[] = { "LOGNAME", "USER", "LNAME", "USERNAME" };
    for (int i = 0; i < 4; i++)
        if (getenv(vars[i]) && *getenv(vars[i])) return xprintf("human:%s", getenv(vars[i]));
    struct passwd *pw = getpwuid(getuid());
    return xprintf("human:%s", pw && pw->pw_name ? pw->pw_name : "unknown");
}

/* a Location code when text names one (any case), else the text as given (trimmed) */
char *place(const archive *a, const char *text)
{
    rec_record *l = archive_location(a, text);
    if (l) return xstrdup(rec_get(l, "Code"));
    while (isspace((unsigned char)*text)) text++;
    char *p = xstrdup(text);
    size_t n = strlen(p);
    while (n && isspace((unsigned char)p[n - 1])) p[--n] = 0;
    return p;
}

/* appraisal.IMPORTANCE_RE, ^\s*(\w+)\s+for\s+(.+?)\s*$ (any case): the level and the audience;
 * 0, or -1 when the text does not read "<level> for <audience>" */
static int parse_importance(const char *text, char *level, size_t nlevel, char *audience, size_t naudience)
{
    const char *p = text;
    while (isspace((unsigned char)*p)) p++;
    const char *w = p;
    while (isalnum((unsigned char)*p) || *p == '_' || ((unsigned char)*p & 0x80)) p++;
    if (p == w || (size_t)(p - w) >= nlevel || !isspace((unsigned char)*p)) return -1;
    memcpy(level, w, (size_t)(p - w));
    level[p - w] = 0;
    while (isspace((unsigned char)*p)) p++;
    if (tolower((unsigned char)p[0]) != 'f' || tolower((unsigned char)p[1]) != 'o' || tolower((unsigned char)p[2]) != 'r'
        || !isspace((unsigned char)p[3]))
        return -1;
    p += 3;
    while (isspace((unsigned char)*p)) p++;
    const char *e = p + strlen(p);
    while (e > p && isspace((unsigned char)e[-1])) e--;
    if (e == p || (size_t)(e - p) >= naudience) return -1;
    memcpy(audience, p, (size_t)(e - p));
    audience[e - p] = 0;
    return 0;
}

/* --importance '<level> for <audience>' (src/arv/appraisal.py) */
rec_record *new_appraisal(const char *target, const strlist *importance, const char *basis, const char *review)
{
    static const char *const levels[] = { "essential", "important", "useful", "incidental", NULL };
    rec_record *r = rec_alloc("Appraisal");
    strlist audiences = { 0 };
    rec_add(r, "Target", target);
    for (size_t i = 0; i < importance->n; i++) {
        char level[64], audience[256];
        if (parse_importance(importance->v[i], level, sizeof level, audience, sizeof audience))
            die("importance must read '<level> for <audience>', e.g. 'essential for family' "
                "(levels: essential, important, useful, incidental), not '%s'", importance->v[i]);
        for (char *p = level; *p; p++) *p = (char)tolower((unsigned char)*p);
        for (char *p = audience; *p; p++) *p = (char)tolower((unsigned char)*p);
        int known = 0;
        for (int k = 0; levels[k]; k++) known |= !strcmp(level, levels[k]);
        if (!known) die("unknown importance level '%s' (levels, most first: essential, important, useful, incidental)", level);
        if (strspn(audience, "abcdefghijklmnopqrstuvwxyz0123456789:_-") != strlen(audience) || !isalnum((unsigned char)audience[0]))
            die("audience '%s': one word of letters, digits, '-', '_' or ':' (e.g. self, family, heirs, colleagues, "
                "public)", audience);
        if (strlist_has(&audiences, audience)) die("%s", "one importance per audience");
        strlist_add(&audiences, audience);
        char *text = xprintf("%s for %s", level, audience);
        rec_add(r, "Importance", text);
        free(text);
    }
    if (!importance->n && !basis) die("%s", "an appraisal needs an importance or a basis");
    if (basis) rec_add(r, "Basis", basis);
    char today[11], *agent = person();
    today_iso(today);
    rec_add(r, "Date", today);
    rec_add(r, "Authorship", "human");
    rec_add(r, "Agent", agent);
    free(agent);
    if (review) rec_add(r, "Review", review);
    strlist_free(&audiences);
    return r;
}

static const char *get_or_empty(const rec_record *r, const char *name)
{
    return rec_get(r, name) ? rec_get(r, name) : "";
}

static int same_fields(const rec_record *a, const rec_record *b)
{
    if (a->nfields != b->nfields) return 0;
    for (size_t i = 0; i < a->nfields; i++)
        if (strcmp(a->fields[i].name, b->fields[i].name) || strcmp(a->fields[i].value, b->fields[i].value)) return 0;
    return 1;
}

static void replace_fields(rec_record *dst, const rec_record *src)
{
    rec_clear(dst);
    rec_copy(dst, src);
}

static rec_record *by_key(const recs *l, const char *key, const char *value, int code)
{
    char *want = code ? norm_code(value) : xstrdup(value ? value : "");
    rec_record *found = NULL;
    for (size_t i = 0; i < l->n && !found; i++)
        if (rec_get(l->v[i], key) && !strcmp(rec_get(l->v[i], key), want)) found = l->v[i];
    free(want);
    return found;
}

static int in_recs(const recs *l, const rec_record *r)
{
    for (size_t i = 0; i < l->n; i++)
        if (same_fields(l->v[i], r)) return 1;
    return 0;
}

/* Merges other into home (catalog.merge): new discs, bindings, places, selections, collections and
 * revisions are added, selection items unioned, events and appraisals appended when new. With
 * prefer_other, existing
 * records take the other's fields (a sealed disc's cut-down record never replaces a full one).
 * Returns the number of new events; added and updated get disc ids. */
size_t archive_merge(archive *home, const archive *other, int prefer_other, strlist *added, strlist *updated)
{
    size_t events = 0;
    for (size_t i = 0; i < other->discs.n; i++) {
        rec_record *d = other->discs.v[i], *existing = archive_disc(home, get_or_empty(d, "Id"));
        if (!existing) {
            recs_add(&home->discs, d);
            strlist_add(added, get_or_empty(d, "Id"));
        } else if (prefer_other && !same_fields(existing, d) && !(rec_get(d, "Withheld") && !rec_get(existing, "Withheld"))) {
            replace_fields(existing, d);
            strlist_add(updated, get_or_empty(d, "Id"));
        }
    }
    for (size_t i = 0; i < other->bindings.n; i++) {
        rec_record *b = other->bindings.v[i], *existing = by_key(&home->bindings, "Volume", rec_get(b, "Volume"), 0);
        if (!existing) recs_add(&home->bindings, b);
        else if (prefer_other) replace_fields(existing, b);
    }
    for (size_t i = 0; i < other->selections.n; i++) {     /* items are unioned: a filtered copy never removes any */
        rec_record *c = other->selections.v[i], *existing = by_key(&home->selections, "Code", rec_get(c, "Code"), 1);
        if (!existing) {
            recs_add(&home->selections, c);
            continue;
        }
        if (prefer_other) {
            rec_record merged = { 0 };
            merged.type = "Selection";
            for (size_t f = 0; f < c->nfields; f++)
                if (strcmp(c->fields[f].name, "Item")) rec_add(&merged, c->fields[f].name, c->fields[f].value);
            for (size_t f = 0; f < existing->nfields; f++)
                if (!strcmp(existing->fields[f].name, "Item")) rec_add(&merged, "Item", existing->fields[f].value);
            replace_fields(existing, &merged);
            rec_clear(&merged);
        }
        strlist have = { 0 };
        for (size_t f = 0; f < existing->nfields; f++)
            if (!strcmp(existing->fields[f].name, "Item")) strlist_add(&have, existing->fields[f].value);
        for (size_t f = 0; f < c->nfields; f++)
            if (!strcmp(c->fields[f].name, "Item") && !strlist_has(&have, c->fields[f].value)) {
                rec_add(existing, "Item", c->fields[f].value);
                strlist_add(&have, c->fields[f].value);
            }
        strlist_free(&have);
    }
    for (size_t i = 0; i < other->locations.n; i++) {
        rec_record *l = other->locations.v[i], *existing = archive_location(home, rec_get(l, "Code"));
        if (!existing) recs_add(&home->locations, l);
        else if (prefer_other) replace_fields(existing, l);
    }
    for (size_t i = 0; i < other->collections.n; i++) {     /* the same collection is the same Uuid */
        rec_record *c = other->collections.v[i], *existing = by_key(&home->collections, "Uuid", rec_get(c, "Uuid"), 0);
        if (!existing) recs_add(&home->collections, c);
        else if (prefer_other) replace_fields(existing, c);
    }
    for (size_t i = 0; i < other->revisions.n; i++)          /* a log: revisions are added, never edited */
        if (!by_key(&home->revisions, "Node", rec_get(other->revisions.v[i], "Node"), 0))
            recs_add(&home->revisions, other->revisions.v[i]);
    for (size_t i = 0; i < other->events.n; i++)
        if (!in_recs(&home->events, other->events.v[i])) {
            recs_add(&home->events, other->events.v[i]);
            events++;
        }
    for (size_t i = 0; i < other->appraisals.n; i++)
        if (!in_recs(&home->appraisals, other->appraisals.v[i])) recs_add(&home->appraisals, other->appraisals.v[i]);
    return events;
}
