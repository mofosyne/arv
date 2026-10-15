/* The catalogue as arv writes it: Disc, Binding, Location, Collection, Revision, Object,
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

static const char *const TYPES[] = { "Home", "Disc", "Binding", "Location", "Collection", "Revision", "Object",
                                     "Event", "Appraisal" };
enum { NTYPES = 9, DISC_TYPE = 1, EVENT_TYPE = 7 };

static recs *group(archive *a, const char *type)
{
    recs *g[] = { &a->homes, &a->discs, &a->bindings, &a->locations, &a->collections, &a->revisions, &a->objects,
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
        recs *g = r->descriptor || !r->type ? NULL : group(a, r->type);
        if (g) recs_add(g, r);
    }
    archive_same_objects(a);
}

/* Two data objects said to be one (arv same: an event about A, "SameAs: object:B"): A's records are
 * read as B's, each the version of B with its Tree (a copy of it), however they were recorded (on
 * discs, and in snapshots, they keep A's Uuid). Applied on every load, so a merge from another
 * catalogue or a disc is read the same way; records then alike (Uuid, Version, Disc, Path) are one. */
typedef struct {
    rec_record *r;
    size_t at;
} id_entry;              /* a record, and where it was listed: for stable sorts */

/* records by one field's value (objects by Uuid or by Disc), each value's records in the list's order,
 * for the last few lists and fields asked about; sorted again when the list grows, or when arv
 * same changes a Uuid (issue #37) */
typedef struct {
    rec_record *const *list;
    size_t n, m, generation;
    const char *field;          /* a string literal */
    rec_record **sorted;
    unsigned long used;
} field_index;

static field_index field_cache[6];
static unsigned long field_clock;
static size_t uuid_generation = 1;          /* bumped when a record's Uuid changes in memory */
static const char *sort_field;

static int by_field_then_order(const void *x, const void *y)
{
    const id_entry *a = x, *b = y;
    int c = strcmp(rec_get(a->r, sort_field), rec_get(b->r, sort_field));
    return c ? c : a->at < b->at ? -1 : a->at > b->at;
}

static recs records_by(const recs *list, const char *field, const char *value)
{
    recs out = { NULL, 0 };
    if (!value || !list->n) return out;
    field_index *d = NULL;
    for (int i = 0; i < 6 && !d; i++)
        if (field_cache[i].list == list->v && field_cache[i].n == list->n && field_cache[i].list
            && field_cache[i].generation == uuid_generation && !strcmp(field_cache[i].field, field))
            d = &field_cache[i];
    if (!d) {
        d = &field_cache[0];
        for (int i = 1; i < 6; i++)
            if (field_cache[i].used < d->used) d = &field_cache[i];
        free(d->sorted);
        id_entry *all = xmalloc((list->n + 1) * sizeof *all);
        size_t m = 0;
        for (size_t i = 0; i < list->n; i++)
            if (rec_get(list->v[i], field)) all[m++] = (id_entry){ list->v[i], i };
        sort_field = field;
        qsort(all, m, sizeof *all, by_field_then_order);
        d->sorted = xmalloc((m + 1) * sizeof *d->sorted);
        for (size_t i = 0; i < m; i++) d->sorted[i] = all[i].r;
        free(all);
        d->list = list->v;
        d->n = list->n;
        d->m = m;
        d->field = field;
        d->generation = uuid_generation;
    }
    d->used = ++field_clock;
    size_t lo = 0, hi = d->m;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (strcmp(rec_get(d->sorted[mid], field), value) < 0) lo = mid + 1;
        else hi = mid;
    }
    size_t end = lo;
    while (end < d->m && !strcmp(rec_get(d->sorted[end], field), value)) end++;
    out.v = d->sorted + lo;
    out.n = end - lo;
    return out;
}

recs object_records(const archive *a, const char *uuid) { return records_by(&a->objects, "Uuid", uuid); }
recs disc_objects(const archive *a, const char *disc_id) { return records_by(&a->objects, "Disc", disc_id); }

static int object_key_cmp(const rec_record *o, const rec_record *x)
{
    static const char *const key[] = { "Uuid", "Version", "Disc", "Path" };
    for (int f = 0; f < 4; f++) {
        const char *a = rec_get(o, key[f]), *b = rec_get(x, key[f]);
        if (!a || !b) return a ? 1 : b ? -1 : 1;    /* a record missing one is never alike */
        int c = strcmp(a, b);
        if (c) return c;
    }
    return 0;
}

static int by_object_key(const void *x, const void *y)
{
    const id_entry *a = x, *b = y;
    int c = object_key_cmp(a->r, b->r);
    return c ? c : a->at < b->at ? -1 : a->at > b->at;
}

void archive_same_objects(archive *a)
{
    int any_same = 0;
    for (size_t i = 0; i < a->events.n && !any_same; i++) any_same = rec_get(a->events.v[i], "SameAs") != NULL;
    for (int pass = 0; any_same && pass < 8; pass++) {          /* A to B, B to C ... */
        int changed = 0;
        for (size_t i = 0; i < a->events.n; i++) {
            const rec_record *e = a->events.v[i];
            const char *of = rec_get(e, "Object"), *to = rec_get(e, "SameAs");
            if (!of || !to || strncmp(of, "object:", 7) || strncmp(to, "object:", 7)) continue;
            const char *from_u = of + 7, *to_u = to + 7;
            for (size_t k = 0; k < a->objects.n; k++) {
                rec_record *o = a->objects.v[k];
                if (!rec_get(o, "Uuid") || strcmp(rec_get(o, "Uuid"), from_u) || !rec_get(o, "Tree")) continue;
                const rec_record *match = NULL;
                for (size_t j = 0; j < a->objects.n && !match; j++) {
                    const rec_record *x = a->objects.v[j];
                    if (x != o && rec_get(x, "Uuid") && !strcmp(rec_get(x, "Uuid"), to_u) && rec_get(x, "Tree")
                        && !strcmp(rec_get(x, "Tree"), rec_get(o, "Tree")))
                        match = x;
                }
                if (!match) continue;               /* bytes B never had: left as it is (arv same refuses it) */
                char *version = xstrdup(rec_get(match, "Version") ? rec_get(match, "Version") : "1");
                rec_set(o, "Uuid", to_u);
                uuid_generation++;              /* any index of objects by Uuid is stale now */
                rec_set(o, "Version", version);
                free(version);
                size_t kept = 0;                    /* no longer another object */
                for (size_t f = 0; f < o->nfields; f++)
                    if (strcmp(o->fields[f].name, "SameBytes")) o->fields[kept++] = o->fields[f];
                o->nfields = kept;
                changed = 1;
            }
        }
        if (!changed) break;
    }
    if (!any_same) return;                          /* nothing said to be one: nothing to fold */
    id_entry *all = xmalloc((a->objects.n + 1) * sizeof *all);   /* one version on one disc at one path once */
    for (size_t k = 0; k < a->objects.n; k++) all[k] = (id_entry){ a->objects.v[k], k };
    qsort(all, a->objects.n, sizeof *all, by_object_key);
    char *drop = calloc(a->objects.n + 1, 1);
    for (size_t k = 1; k < a->objects.n; k++)
        if (!object_key_cmp(all[k - 1].r, all[k].r)) drop[all[k].at] = 1;   /* the later of two alike */
    free(all);
    size_t kept = 0;
    for (size_t k = 0; k < a->objects.n; k++)
        if (!drop[k]) a->objects.v[kept++] = a->objects.v[k];
    free(drop);
    a->objects.n = kept;
}

/* ------------------------------------------------------------------ events by what they are about
 * Most questions about copies are about one disc's events; asking each over every event made a
 * listing of N discs cost N times the whole history (issue #37). So each disc's events, in order,
 * are found once and kept, for the last few event lists asked about; a list that has grown since
 * (events are only ever appended) is indexed again. */

typedef struct {
    rec_record *const *events;  /* the list indexed, and how long it was */
    size_t n;
    char **keys;                /* disc ids, sorted; each one's events in order from starts[k] */
    size_t nkeys, *starts, *counts;
    rec_record **sorted;
    unsigned long used;
} disc_index;

static disc_index cache[4];
static unsigned long cache_clock;

typedef struct {
    const char *disc;
    size_t at;
    rec_record *e;
} index_entry;

static int by_disc_then_order(const void *x, const void *y)
{
    const index_entry *a = x, *b = y;
    int c = strcmp(a->disc, b->disc);
    return c ? c : a->at < b->at ? -1 : a->at > b->at;
}

static void index_free(disc_index *d)
{
    for (size_t k = 0; k < d->nkeys; k++) free(d->keys[k]);
    free(d->keys);
    free(d->starts);
    free(d->counts);
    free(d->sorted);
    memset(d, 0, sizeof *d);
}

recs disc_events(const archive *a, const char *disc_id)
{
    recs out = { NULL, 0 };
    if (!disc_id || !a->events.n) return out;
    disc_index *d = NULL;
    for (int i = 0; i < 4 && !d; i++)
        if (cache[i].events == a->events.v && cache[i].n == a->events.n && cache[i].events) d = &cache[i];
    if (!d) {                                       /* index it, in place of the one used longest ago */
        d = &cache[0];
        for (int i = 1; i < 4; i++)
            if (cache[i].used < d->used) d = &cache[i];
        index_free(d);
        index_entry *all = xmalloc((a->events.n + 1) * sizeof *all);
        size_t m = 0;
        all = xrealloc(all, (2 * a->events.n + 1) * sizeof *all);
        for (size_t i = 0; i < a->events.n; i++) {     /* by what each is about: a disc, or object:UUID */
            const rec_record *e = a->events.v[i];
            const char *thing = rec_get(e, "Disc") ? rec_get(e, "Disc") : rec_get(e, "Object");
            if (thing) all[m++] = (index_entry){ thing, i, a->events.v[i] };
            if (rec_get(e, "SameAs")) all[m++] = (index_entry){ "#same", i, a->events.v[i] };   /* arv same */
        }
        qsort(all, m, sizeof *all, by_disc_then_order);
        d->sorted = xmalloc((m + 1) * sizeof *d->sorted);
        d->keys = xmalloc((m + 1) * sizeof *d->keys);
        d->starts = xmalloc((m + 1) * sizeof *d->starts);
        d->counts = xmalloc((m + 1) * sizeof *d->counts);
        for (size_t i = 0; i < m; i++) {
            d->sorted[i] = all[i].e;
            if (!d->nkeys || strcmp(d->keys[d->nkeys - 1], all[i].disc)) {
                d->keys[d->nkeys] = xstrdup(all[i].disc);
                d->starts[d->nkeys] = i;
                d->counts[d->nkeys++] = 0;
            }
            d->counts[d->nkeys - 1]++;
        }
        free(all);
        d->events = a->events.v;
        d->n = a->events.n;
    }
    d->used = ++cache_clock;
    size_t lo = 0, hi = d->nkeys;                   /* the disc's run */
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(d->keys[mid], disc_id);
        if (!c) {
            out.v = d->sorted + d->starts[mid];
            out.n = d->counts[mid];
            return out;
        }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return out;
}

/* every record in file order: each type's descriptor, then its records (as Catalog.records) */
void archive_records(const archive *a, recs *out)
{
    const recs *g[] = { &a->homes, &a->discs, &a->bindings, &a->locations, &a->collections, &a->revisions,
                        &a->objects, &a->events, &a->appraisals };
    for (int i = 0; i < NTYPES; i++) {
        int always = i == DISC_TYPE || i == EVENT_TYPE;     /* Disc and Event descriptors are always written */
        if (!always && !g[i]->n) continue;
        recs_add(out, descriptor(TYPES[i]));
        for (size_t k = 0; k < g[i]->n; k++) recs_add(out, g[i]->v[k]);
    }
}

/* ------------------------------------------------------------------ event ids */

/* an event's id from its content (PREMIS eventIdentifier): the first 32 hex digits of the SHA-256
 * of its fields as written, all but EventId. The same event has the same id in every catalogue
 * and on every disc; an event changed after it was written no longer matches the id it was given. */
void event_content_id(const rec_record *e, char out[33])
{
    rec_record view = *e;
    view.fields = xmalloc((e->nfields ? e->nfields : 1) * sizeof *view.fields);
    view.nfields = 0;
    for (size_t i = 0; i < e->nfields; i++)
        if (strcmp(e->fields[i].name, "EventId")) view.fields[view.nfields++] = e->fields[i];
    char *text = NULL, hex[65];
    size_t len = 0;
    rec_format(&view, &text, &len);
    text_sha256(text ? text : "", hex);
    memcpy(out, hex, 32);
    out[32] = 0;
    free(text);
    free(view.fields);
}

/* The id an event was given. Events without one are not identified by their content. */
void event_key(const rec_record *e, char out[33])
{
    const char *id = rec_get(e, "EventId");
    if (id && strlen(id) == 32) memcpy(out, id, 33);
    else out[0] = 0;
}

static int is_event(const rec_record *r)
{
    return !r->descriptor && r->type && !strcmp(r->type, "Event");
}

/* New events are marked in memory by new_event; assign their ids before writing. Never fill an id
 * onto an event read from a catalogue. Two new events with the same content get a Nonce. */
static void assign_event_ids(const recs *r)
{
    strlist ids = { 0 };
    for (size_t i = 0; i < r->n; i++)
        if (is_event(r->v[i]) && rec_get(r->v[i], "EventId")) strlist_add(&ids, rec_get(r->v[i], "EventId"));
    for (size_t i = 0; i < r->n; i++) {
        rec_record *e = r->v[i];
        if (!is_event(e) || !rec_get(e, "PendingId") || rec_get(e, "EventId")) continue;
        for (size_t f = 0; f < e->nfields; f++) {
            if (strcmp(e->fields[f].name, "PendingId")) continue;
            free(e->fields[f].name);
            free(e->fields[f].value);
            memmove(e->fields + f, e->fields + f + 1, (e->nfields - f - 1) * sizeof *e->fields);
            e->nfields--;
            break;
        }
        char id[33];
        event_content_id(e, id);
        for (int nonce = 2; strlist_has(&ids, id); nonce++) {
            char n[16];
            snprintf(n, sizeof n, "%d", nonce);
            rec_set(e, "Nonce", n);
            event_content_id(e, id);
        }
        rec_add(e, "EventId", id);
        rec_field mine = e->fields[e->nfields - 1];
        memmove(e->fields + 1, e->fields, (e->nfields - 1) * sizeof *e->fields);
        e->fields[0] = mine;
        strlist_add(&ids, id);
    }
    strlist_free(&ids);
}

void write_records(const char *path, const recs *r)
{
    assign_event_ids(r);
    char *tmp = xprintf("%s.tmp", path);
    if (rec_write(tmp, r->v, r->n) || rename(tmp, path)) die("cannot write %s", path);
    free(tmp);
}

const char *archive_home_uuid(archive *a)
{
    if (!a->homes.n) {
        char uuid[37], today[11];
        uuid4(uuid);
        today_iso(today);
        rec_record *r = rec_alloc("Home");
        rec_add(r, "Uuid", uuid);
        rec_add(r, "Date", today);
        recs_add(&a->homes, r);
    }
    return rec_get(a->homes.v[0], "Uuid");
}

void archive_save(archive *a, const char *path)
{
    static const char tail[] = "/catalog/archive.rec";    /* a home's catalogue: not where a copy of it is */
    size_t n = strlen(path);
    if (n > sizeof tail - 1 && !strcmp(path + n - (sizeof tail - 1), tail)) {
        char *arv_dir = xprintf("%.*s", (int)(n - (sizeof tail - 1)), path);
        home_guard(arv_dir);
        free(arv_dir);
    }
    archive_home_uuid(a);
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

/* discs by Id, sorted (of two with one id, the one listed first first), for the last few disc lists
 * asked about; a list that has grown is sorted again (issue #37: a linear lookup a disc made
 * listings quadratic) */
typedef struct {
    rec_record *const *discs;   /* the list sorted, and how long it was */
    size_t n, m;                /* m: how many have an Id */
    rec_record **sorted;
    unsigned long used;
} disc_ids;

static disc_ids id_cache[4];
static unsigned long id_clock;

static int by_disc_id(const void *x, const void *y)
{
    const id_entry *a = x, *b = y;
    int c = strcmp(rec_get(a->r, "Id"), rec_get(b->r, "Id"));
    return c ? c : a->at < b->at ? -1 : a->at > b->at;
}

rec_record *archive_disc(const archive *a, const char *id)
{
    if (!id || !a->discs.n) return NULL;
    disc_ids *d = NULL;
    for (int i = 0; i < 4 && !d; i++)
        if (id_cache[i].discs == a->discs.v && id_cache[i].n == a->discs.n && id_cache[i].discs) d = &id_cache[i];
    if (!d) {                               /* sort it, in place of the one used longest ago */
        d = &id_cache[0];
        for (int i = 1; i < 4; i++)
            if (id_cache[i].used < d->used) d = &id_cache[i];
        free(d->sorted);
        id_entry *all = xmalloc((a->discs.n + 1) * sizeof *all);
        size_t m = 0;
        for (size_t i = 0; i < a->discs.n; i++)
            if (rec_get(a->discs.v[i], "Id")) all[m++] = (id_entry){ a->discs.v[i], i };
        qsort(all, m, sizeof *all, by_disc_id);
        d->sorted = xmalloc((m + 1) * sizeof *d->sorted);
        for (size_t i = 0; i < m; i++) d->sorted[i] = all[i].r;
        free(all);
        d->discs = a->discs.v;
        d->n = a->discs.n;
        d->m = m;
    }
    d->used = ++id_clock;
    size_t lo = 0, hi = d->m;               /* the first with this id */
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (strcmp(rec_get(d->sorted[mid], "Id"), id) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo < d->m && !strcmp(rec_get(d->sorted[lo], "Id"), id) ? d->sorted[lo] : NULL;
}

rec_record *archive_location(const archive *a, const char *code) { return by_code(&a->locations, code); }

/* next unused sequence number in a set (numbers are never reused) */
long archive_next_number(const archive *a, const char *set)   /* set: the id prefix */
{
    long most = 0;
    for (size_t i = 0; i < a->discs.n; i++) {
        const rec_record *d = a->discs.v[i];
        discid_parts p;              /* the id's prefix: a set code, or a collection's code */
        int parsed = discid_parse(rec_get(d, "Id") ? rec_get(d, "Id") : "", &p);
        if (parsed ? strcmp(p.set, set) : !rec_get(d, "Set") || strcmp(rec_get(d, "Set"), set)) continue;
        const char *seq = rec_get(d, "Sequence");
        long n = 0;
        if (seq && *seq && strspn(seq, "0123456789") == strlen(seq)) n = atol(seq);
        else if (parsed) n = p.sequence;
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


static int in_list(const strlist *l, const char *s)
{
    return s && strlist_has(l, s);
}

/* locations a disc carries are the places its recorded copies name, and their parents */
void archive_locations_for(const archive *a, const recs *discs, recs *out)
{
    strlist codes = { 0 };
    for (size_t i = 0; i < discs->n; i++) {
        const char *id = rec_get(discs->v[i], "Id");
        if (rec_get(discs->v[i], "Retired") || !strcmp(disc_access(discs->v[i]), "sealed")) continue;
        for (size_t e = 0; id && e < a->events.n; e++) {
            const rec_record *event = a->events.v[e];
            const char *disc = rec_get(event, "Disc"), *place = rec_get(event, "Location");
            if (!disc || strcmp(disc, id) || !place) continue;
            rec_record *l = archive_location(a, place);
            for (int depth = 0; l && depth < 64; depth++) {
                if (in_list(&codes, rec_get(l, "Code"))) break;
                strlist_add(&codes, rec_get(l, "Code"));
                l = archive_location(a, rec_get(l, "Parent"));
            }
        }
    }
    for (size_t i = 0; i < a->locations.n; i++)
        if (in_list(&codes, rec_get(a->locations.v[i], "Code"))) recs_add(out, a->locations.v[i]);
    strlist_free(&codes);
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


static const char *const SEALED_FIELDS[] = { "Id", "Uuid", "IdScheme", "Set", "Category", "Path", "Sequence",
                                            "Coverage", "Date", "Part", "Access", NULL };

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

static const char *target_disc(const archive *a, const char *target, char *buf, size_t n)
{
    if (!target || !strncmp(target, "set:", 4)) return NULL;
    char *id = item_disc(target, NULL);
    if (!archive_disc(a, id)) {
        free(id);
        return NULL;
    }
    snprintf(buf, n, "%s", id);
    free(id);
    return buf;
}

/* catalog.subset(ids).shared_view(): the prior discs as another disc may carry them */
void archive_shared_subset(const archive *a, const strlist *ids, archive *out)
{
    char buf[256];
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < a->homes.n; i++) recs_add(&out->homes, a->homes.v[i]);
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
        const char *d = target_disc(a, rec_get(a->appraisals.v[i], "Target"), buf, sizeof buf);
        if (in_list(ids, d) && !in_list(&sealed, d)) recs_add(&out->appraisals, a->appraisals.v[i]);
    }
    for (size_t i = 0; i < a->objects.n; i++) {          /* without Source: that stays at home */
        const char *d = rec_get(a->objects.v[i], "Disc");
        if (in_list(ids, d) && !in_list(&sealed, d)) recs_add(&out->objects, object_disc_view(a->objects.v[i]));
    }
    strlist_free(&sealed);
}

static const char *get_or(const rec_record *r, const char *name, const char *dflt)
{
    const char *v = rec_get(r, name);
    return v ? v : dflt;
}

/* an object as discs carry it: every field but Source (where it was read from: home only) */
rec_record *object_disc_view(const rec_record *o)
{
    rec_record *r = rec_alloc("Object");
    for (size_t f = 0; f < o->nfields; f++)
        if (strcmp(o->fields[f].name, "Source")) rec_add(r, o->fields[f].name, o->fields[f].value);
    return r;
}

/* every place a disc's copies are kept, as readable paths ("" when not recorded). A record
 * without Id is a temporary one-place view used when showing an individual copy. */
char *archive_where(const archive *a, const rec_record *d)
{
    strlist places = { 0 };
    const char *id = rec_get(d, "Id");
    if (id) archive_copy_locations(a, id, &places);
    else
        for (size_t f = 0; f < d->nfields; f++)
            if (!strcmp(d->fields[f].name, "Location") && !strlist_has(&places, d->fields[f].value))
                strlist_add(&places, d->fields[f].value);
    sbuf out = { 0 };
    sb_puts(&out, "");
    for (size_t f = 0; f < places.n; f++) {
        if (out.len) sb_puts(&out, "; ");
        const rec_record *chain[64];
        size_t n = 0;
        for (rec_record *l = archive_location(a, places.v[f]); l && n < 64;
             l = archive_location(a, rec_get(l, "Parent"))) {
            size_t k;
            for (k = 0; k < n && chain[k] != l; k++) {}
            if (k < n) break;
            chain[n++] = l;
        }
        if (!n) sb_puts(&out, places.v[f]);
        while (n--) {
            const char *name = rec_get(chain[n], "Name") ? rec_get(chain[n], "Name") : rec_get(chain[n], "Code");
            sb_printf(&out, "%s%s", name, n ? " / " : "");
        }
    }
    strlist_free(&places);
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
    rec_add(r, "PendingId", "yes");
    return r;
}

const char *place_temperature(const archive *a, const char *code)
{
    int guard = 0;
    for (const rec_record *l = code ? archive_location(a, code) : NULL; l && guard < 64; guard++) {
        if (rec_get(l, "Temperature")) return rec_get(l, "Temperature");
        l = rec_get(l, "Parent") ? archive_location(a, rec_get(l, "Parent")) : NULL;
    }
    return NULL;
}

int temperature_ok(const char *t)
{
    return t && (!strcmp(t, "hot") || !strcmp(t, "warm") || !strcmp(t, "cold"));
}

/* human:LOGIN from the account database */
char *person(void)
{
    const char *vars[] = { "LOGNAME", "USER", "LNAME", "USERNAME" };
    for (int i = 0; i < 4; i++)
        if (getenv(vars[i]) && *getenv(vars[i])) return xprintf("human:%s", getenv(vars[i]));
    struct passwd *pw = getpwuid(getuid());
    return xprintf("human:%s", pw && pw->pw_name ? pw->pw_name : "unknown");
}

/* a Location code when text names one (any case), else the text as given (trimmed) */
/* ------------------------------------------------------------------ copies
 * Each copy of a disc is one replication event (burned, an image file, or a folder), named by a
 * letter unique among the disc's copies: A, B ... Z, AA, AB ... A check of one copy names it too. */

int is_copy_event(const rec_record *e, const char *disc_id)
{
    const char *d = rec_get(e, "Disc"), *t = rec_get(e, "Type"), *o = rec_get(e, "Outcome");
    const char *id = rec_get(e, "EventId"), *copy = rec_get(e, "Copy");
    return d && t && id && strlen(id) == 32 && copy && *copy && !strcmp(d, disc_id)
        && !strcmp(t, "replication") && !(o && !strcmp(o, "failure"));
}

size_t disc_copies(const archive *a, const char *disc_id)
{
    recs ev = disc_events(a, disc_id);   /* only its events */
    size_t n = 0;                   /* independent copies: a disc recorded twice (a strong identifier shared) is one,
                                     * as are copies on one carrier (a tape holding both) */
    for (size_t i = 0; i < ev.n; i++)
        n += is_copy_event(ev.v[i], disc_id) && !copy_same_as(a, disc_id, ev.v[i])
             && !copy_shares_carrier(a, disc_id, ev.v[i]);
    return n;
}

/* Whether event e names the copy whose replication is rep, by its EventId. */
int event_names_copy(const char *disc_id, const rec_record *e, const rec_record *rep)
{
    const char *d = rec_get(e, "Disc"), *t = rec_get(e, "Type");
    if (e == rep || !d || strcmp(d, disc_id) || (t && !strcmp(t, "replication")) || !rec_get(rep, "EventId")) return 0;
    const char *r = rec_get(e, "Replication");
    if (!r) return 0;
    char k[33];
    event_key(rep, k);
    return *k && !strcmp(r, k);
}

/* a copy's latest recorded place: the replication's initial place, then any event naming it */
const char *copy_location(const archive *a, const char *disc_id, const rec_record *copy)
{
    recs ev = disc_events(a, disc_id);   /* only its events */
    const char *loc = rec_get(copy, "Location");
    for (size_t i = 0; i < ev.n; i++) {
        const rec_record *e = ev.v[i];
        if (rec_get(e, "Location") && event_names_copy(disc_id, e, copy)) loc = rec_get(e, "Location");
    }
    return loc;
}

/* distinct current places for every recorded physical copy of one disc */
void archive_copy_locations(const archive *a, const char *disc_id, strlist *out)
{
    recs ev = disc_events(a, disc_id);   /* only its events */
    memset(out, 0, sizeof *out);
    if (!disc_id) return;
    rec_record *disc = archive_disc(a, disc_id);
    if (disc && rec_get(disc, "Retired")) return;
    for (size_t i = 0; i < ev.n; i++) {
        const rec_record *copy = ev.v[i];
        if (!is_copy_event(copy, disc_id)) continue;
        const char *loc = copy_location(a, disc_id, copy);
        if (loc && !strlist_has(out, loc)) strlist_add(out, loc);
    }
}

/* a copy's letter now: the one on the newest event naming it by Replication (arv relabel gives a
 * new one), else the one it was given */
const char *copy_letter(const archive *a, const char *disc_id, const rec_record *rep)
{
    recs ev = disc_events(a, disc_id);   /* only its events */
    const char *letter = rec_get(rep, "Copy");
    if (!rec_get(rep, "EventId") || !letter) return NULL;
    char k[33];
    event_key(rep, k);
    for (size_t i = 0; i < ev.n; i++) {
        const rec_record *e = ev.v[i];
        const char *r = rec_get(e, "Replication"), *d = rec_get(e, "Disc");
        if (r && d && rec_get(e, "Copy") && !strcmp(d, disc_id) && !strcmp(r, k)) letter = rec_get(e, "Copy");
    }
    return letter;
}

/* a copy's letter now */
const char *letter_of(const archive *a, const rec_record *rep)
{
    const char *d = rec_get(rep, "Disc"), *l = d ? copy_letter(a, d, rep) : NULL;
    return l;
}

/* how many of the disc's copies have this letter now; *first gets the first */
size_t copies_lettered(const archive *a, const char *disc_id, const char *letter, const rec_record **first)
{
    recs ev = disc_events(a, disc_id);   /* only its events */
    size_t n = 0;
    if (first) *first = NULL;
    for (size_t i = 0; i < ev.n; i++) {
        const rec_record *e = ev.v[i];
        if (!is_copy_event(e, disc_id)) continue;
        const char *now = copy_letter(a, disc_id, e);
        if (!now || strcmp(now, letter)) continue;
        if (first && !*first) *first = e;
        n++;
    }
    return n;
}

int copy_exists(const archive *a, const char *disc_id, const char *letter)
{
    return copies_lettered(a, disc_id, letter, NULL) > 0;
}

/* a letter ever given to one of the disc's copies (now, or before a relabel): not given again */
static int letter_used(const archive *a, const char *disc_id, const char *letter)
{
    recs ev = disc_events(a, disc_id);   /* only its events */
    for (size_t i = 0; i < ev.n; i++) {
        const rec_record *e = ev.v[i];
        if (rec_get(e, "Disc") && !strcmp(rec_get(e, "Disc"), disc_id) && rec_get(e, "Copy") && !strcmp(rec_get(e, "Copy"), letter))
            return 1;
    }
    return 0;
}

/* A copy letter as written on a disc: 1-3 capital letters */
int copy_letter_ok(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 3) return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] < 'A' || s[i] > 'Z') return 0;
    return 1;
}

/* the first letter no copy of the disc has */
char *copy_next(const archive *a, const char *disc_id)
{
    for (size_t k = 0;; k++) {
        char buf[8], tmp[8];
        size_t n = 0, v = k + 1;               /* bijective base 26: 1 = A, 26 = Z, 27 = AA */
        while (v && n < sizeof tmp - 1) {
            v--;
            tmp[n++] = (char)('A' + v % 26);
            v /= 26;
        }
        for (size_t j = 0; j < n; j++) buf[j] = tmp[n - 1 - j];
        buf[n] = 0;
        if (!letter_used(a, disc_id, buf)) return xstrdup(buf);
    }
}

/* a copy known good: read back identical as it was recorded, or a later check of that copy passed */
/* the date of a disc's last good check (success, or a warning: repaired and whole), not counting the
 * test of its image when it was made; "" when there is none */
const char *disc_last_check(const archive *a, const char *disc_id)
{
    recs ev = disc_events(a, disc_id);   /* only its events */
    const char *last = "";
    for (size_t i = 0; i < ev.n; i++) {
        const rec_record *e = ev.v[i];
        const char *d = rec_get(e, "Disc"), *t = rec_get(e, "Type"), *o = rec_get(e, "Outcome"), *date = rec_get(e, "Date"),
                   *note = rec_get(e, "Note");
        if (!d || !t || !o || !date || strcmp(d, disc_id) || strcmp(t, "fixity check")) continue;
        if (strcmp(o, "success") && strcmp(o, "warning")) continue;
        if (note && !strncmp(note, "image test after creation", 25)) continue;
        if (strncmp(date, last, 10) > 0) last = date;
    }
    return last;
}

/* the date of the newest passed check naming this copy, or NULL */
const char *copy_last_check(const archive *a, const char *disc_id, const rec_record *copy)
{
    recs ev = disc_events(a, disc_id);   /* only its events */
    const char *newest = NULL;

    for (size_t i = 0; i < ev.n; i++) {
        const rec_record *e = ev.v[i];
        const char *d = rec_get(e, "Disc"), *t = rec_get(e, "Type"), *o = rec_get(e, "Outcome");
        if (!d || !t || !o || strcmp(d, disc_id) || strcmp(t, "fixity check") || strcmp(o, "success")) continue;
        if (event_names_copy(disc_id, e, copy)) {
            const char *date = get_or(e, "Date", "");
            if (!newest || strcmp(date, newest) >= 0) newest = date;
        }
    }
    return newest;
}

int copy_read_back(const archive *a, const char *disc_id, const rec_record *copy)
{
    if (rec_get(copy, "ReadBack") && !strcmp(rec_get(copy, "ReadBack"), "identical")) return 1;
    return copy_last_check(a, disc_id, copy) != NULL;
}

/* ------------------------------------------------------------------ copies' identifiers
 * How one physical copy is recognised, as "scheme:value": bca: (the factory serial a drive reads),
 * barcode: (a label stuck on and scanned), serial: (a manufacturer's serial). bca: and barcode:
 * are strong: two records that share one are one disc. A copy may have several, recorded with it or
 * by a later event that names it (a check that read the BCA, arv identify). */

static const char *const SCHEMES[] = { "bca", "barcode", "serial", NULL };

char *identifier_normal(const char *text, const char **why)
{
    const char *colon = text ? strchr(text, ':') : NULL;
    *why = "SCHEME:VALUE, the scheme one of bca, barcode, serial (bca:041e..., barcode:ARV-000123)";
    if (!colon || colon == text || !colon[1]) return NULL;
    char scheme[16];
    size_t n = (size_t)(colon - text);
    if (n >= sizeof scheme) return NULL;
    for (size_t i = 0; i < n; i++) scheme[i] = (char)tolower((unsigned char)text[i]);
    scheme[n] = 0;
    int known = 0;
    for (int k = 0; SCHEMES[k]; k++) known |= !strcmp(scheme, SCHEMES[k]);
    if (!known) return NULL;
    const char *value = colon + 1;
    while (isspace((unsigned char)*value)) value++;
    if (!*value || strchr(value, '\n')) return NULL;
    if (!strcmp(scheme, "bca")) {
        char hex[33];
        *why = "bca:SERIAL, the BCA serial: 32 hex digits (dev-tools/disc-probe prints it)";
        if (bca_normal(value, hex)) return NULL;
        return xprintf("bca:%s", hex);
    }
    char *out = xprintf("%s:%s", scheme, value), *e = out + strlen(out);
    while (e > out && isspace((unsigned char)e[-1])) *--e = 0;
    return out;
}

int identifier_strong(const char *id)
{
    return id && (!strncmp(id, "bca:", 4) || !strncmp(id, "barcode:", 8) || !strncmp(id, "serial:", 7));
}

/* the medium a stored copy is on, when it holds more than this one image (a tape, a hard drive):
 * the newest Carrier recorded for it, on its replication or a later event naming it; NULL if none.
 * A burned disc is its own medium and has none. */
const char *copy_carrier(const archive *a, const char *disc_id, const rec_record *copy)
{
    recs ev = disc_events(a, disc_id);   /* only its events */
    const char *carrier = rec_get(copy, "Carrier");
    for (size_t i = 0; i < ev.n; i++) {
        const rec_record *e = ev.v[i];
        if (rec_get(e, "Carrier") && event_names_copy(disc_id, e, copy)) carrier = rec_get(e, "Carrier");
    }
    return carrier;
}

/* an earlier copy of the same disc on the same carrier (one tape holding both), or NULL: the two
 * are lost together, so they count as one */
const rec_record *copy_shares_carrier(const archive *a, const char *disc_id, const rec_record *copy)
{
    recs ev = disc_events(a, disc_id);   /* only its events */
    const char *mine = copy_carrier(a, disc_id, copy);
    if (!mine || !identifier_strong(mine)) return NULL;
    for (size_t i = 0; i < ev.n && ev.v[i] != copy; i++) {
        const rec_record *e = ev.v[i];
        const char *theirs = is_copy_event(e, disc_id) ? copy_carrier(a, disc_id, e) : NULL;
        if (theirs && !strcmp(theirs, mine)) return e;
    }
    return NULL;
}

/* every identifier of a copy: on its replication, and on any later event naming its EventId */
void copy_identifiers(const archive *a, const char *disc_id, const rec_record *copy, strlist *out)
{
    recs ev = disc_events(a, disc_id);   /* only its events */
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < ev.n; i++) {
        const rec_record *e = ev.v[i];
        if (e != copy) {        /* a later event naming its Replication EventId */
            const char *o = rec_get(e, "Outcome");
            if ((o && !strcmp(o, "failure")) || !event_names_copy(disc_id, e, copy)) continue;
        }
        for (size_t f = 0; f < e->nfields; f++)
            if (!strcmp(e->fields[f].name, "Identifier") && !strlist_has(out, e->fields[f].value))
                strlist_add(out, e->fields[f].value);
    }
}

static int copy_has(const archive *a, const char *disc_id, const rec_record *copy, const char *id)
{
    strlist ids;
    copy_identifiers(a, disc_id, copy, &ids);
    int has = strlist_has(&ids, id);
    strlist_free(&ids);
    return has;
}

/* where a stored copy is: the newest Path recorded for it (on its replication, or an event that
 * moved it, naming its letter); NULL for a burned disc */
const char *copy_path(const archive *a, const char *disc_id, const rec_record *copy)
{
    recs ev = disc_events(a, disc_id);   /* only its events */
    const char *path = rec_get(copy, "Path");
    for (size_t i = 0; i < ev.n; i++) {
        const rec_record *e = ev.v[i];
        if (rec_get(e, "Path") && event_names_copy(disc_id, e, copy)) path = rec_get(e, "Path");
    }
    return path;
}

/* the copy (its replication) an identifier names, in any disc's copies; NULL if none */
const rec_record *copy_by_identifier(const archive *a, const char *id)
{
    for (size_t i = 0; i < a->events.n; i++) {
        const rec_record *e = a->events.v[i];
        if (rec_get(e, "Disc") && is_copy_event(e, rec_get(e, "Disc")) && copy_has(a, rec_get(e, "Disc"), e, id)) return e;
    }
    return NULL;
}

/* an earlier record of the same physical copy (a strong identifier shared), or NULL: the same disc
 * recorded twice, counted once */
const rec_record *copy_same_as(const archive *a, const char *disc_id, const rec_record *copy)
{
    recs ev = disc_events(a, disc_id);   /* only its events */
    strlist mine;
    copy_identifiers(a, disc_id, copy, &mine);
    const rec_record *found = NULL;
    for (size_t i = 0; i < ev.n && !found && ev.v[i] != copy; i++) {
        const rec_record *e = ev.v[i];
        if (!is_copy_event(e, disc_id)) continue;
        for (size_t k = 0; k < mine.n && !found; k++)
            if (identifier_strong(mine.v[k]) && copy_has(a, disc_id, e, mine.v[k])) found = e;
    }
    strlist_free(&mine);
    return found;
}

/* what the copies' identifiers say that a person must sort out: one disc recorded twice, an
 * identifier on copies of two discs, a copy with two BCA serials. Printed; returns how many. */
typedef struct {
    const char *id;
    size_t copy, at;            /* which copy, and which of its identifiers */
} id_hit;

typedef struct {
    size_t i, j, k;
} id_pair;

static int by_id_hit(const void *x, const void *y)
{
    const id_hit *a = x, *b = y;
    int c = strcmp(a->id, b->id);
    return c ? c : a->copy < b->copy ? -1 : a->copy > b->copy;
}

static int by_id_pair(const void *x, const void *y)
{
    const id_pair *a = x, *b = y;
    if (a->i != b->i) return a->i < b->i ? -1 : 1;
    if (a->j != b->j) return a->j < b->j ? -1 : 1;
    return a->k < b->k ? -1 : a->k > b->k;
}

size_t identity_report(const archive *a)
{
    size_t n = 0, m = 0;
    const rec_record **copies = xmalloc((a->events.n + 1) * sizeof *copies);
    strlist *ids = xmalloc((a->events.n + 1) * sizeof *ids);
    for (size_t i = 0; i < a->events.n; i++) {
        const rec_record *e = a->events.v[i];
        if (!rec_get(e, "Disc") || !is_copy_event(e, rec_get(e, "Disc"))) continue;
        copies[m] = e;
        copy_identifiers(a, rec_get(e, "Disc"), e, &ids[m++]);
    }
    /* a later record sharing a strong identifier: copies by identifier (sorted), so each pair is found
     * without comparing every copy with every other; reported in the same order, i then j */
    id_hit *hits = NULL;
    size_t nh = 0, caph = 0;
    for (size_t i = 0; i < m; i++)
        for (size_t k = 0; k < ids[i].n; k++) {
            if (!identifier_strong(ids[i].v[k])) continue;
            if (nh == caph) hits = xrealloc(hits, (caph = caph ? 2 * caph : 64) * sizeof *hits);
            hits[nh++] = (id_hit){ ids[i].v[k], i, k };
        }
    if (nh) qsort(hits, nh, sizeof *hits, by_id_hit);
    id_pair *pairs = NULL;
    size_t np = 0, capp = 0;
    for (size_t g = 0; g < nh;) {
        size_t end = g + 1;
        while (end < nh && !strcmp(hits[end].id, hits[g].id)) end++;
        for (size_t x = g; x < end; x++)
            for (size_t y = x + 1; y < end; y++) {
                if (hits[x].copy == hits[y].copy) continue;
                if (np == capp) pairs = xrealloc(pairs, (capp = capp ? 2 * capp : 16) * sizeof *pairs);
                pairs[np++] = (id_pair){ hits[x].copy, hits[y].copy, hits[x].at };
            }
        g = end;
    }
    if (np) qsort(pairs, np, sizeof *pairs, by_id_pair);
    size_t q = 0;
    for (size_t i = 0; i < m; i++) {
        const char *disc = rec_get(copies[i], "Disc"), *bca = NULL;
        for (size_t k = 0; k < ids[i].n; k++) {
            if (strncmp(ids[i].v[k], "bca:", 4)) continue;
            if (bca && strcmp(bca, ids[i].v[k])) {
                printf("TWO BCA  %s copy %s: %s and %s (one disc has one serial: a mix-up, the wrong disc in the drive?)\n",
                       disc, letter_of(a, copies[i]), bca, ids[i].v[k]);
                printf("  Put the disc in a drive: arv drives says which disc and copy it is.\n");
                n++;
            }
            bca = ids[i].v[k];
        }
        for (; q < np && pairs[q].i == i; q++) {
            if (q && pairs[q].i == pairs[q - 1].i && pairs[q].j == pairs[q - 1].j) continue;   /* its first identifier */
            size_t j = pairs[q].j;
            const char *other = rec_get(copies[j], "Disc"), *id = ids[i].v[pairs[q].k];
            if (strcmp(disc, other)) {
                printf("TWO DISCS  %s is on copy %s of %s and copy %s of %s: one physical disc cannot be both\n",
                       id, letter_of(a, copies[i]), disc, letter_of(a, copies[j]), other);
                printf("  Put the disc in a drive: arv drives says which disc and copy it is.\n");
            } else
                printf("TWICE  %s copies %s and %s are one disc (%s): recorded twice, counted once\n", disc,
                       letter_of(a, copies[i]), letter_of(a, copies[j]), id);
            n++;
        }
    }
    free(hits);
    free(pairs);
    for (size_t i = 0; i < m; i++) strlist_free(&ids[i]);
    free(ids);
    free(copies);
    return n;
}

/* ------------------------------------------------------------------ field histories
 * An event that changes a field of a thing (a disc's Access, a place's Name ...) says so, Sets: FIELD,
 * and names the changes to that field it replaces, Replaces: EVENTID (the latest it knew of). Each
 * field then has its own small history; two changes that replace the same one were made apart (in
 * two catalogues) and are a conflict, which a merge reports and a person settles by setting it again. */

static const char *thing_of(const rec_record *e)
{
    return rec_get(e, "Disc") ? rec_get(e, "Disc") : rec_get(e, "Object");
}

static int sets(const rec_record *e, const char *thing, const char *field)
{
    const char *t = thing_of(e);
    if (!t || strcmp(t, thing)) return 0;
    for (size_t i = 0; i < e->nfields; i++)
        if (!strcmp(e->fields[i].name, "Sets") && !strcmp(e->fields[i].value, field)) return 1;
    return 0;
}

/* the changes to a field no later change replaces: one, or none yet; two or more is a conflict */
void field_heads(const archive *a, const char *thing, const char *field, strlist *heads)
{
    strlist replaced = { 0 };
    memset(heads, 0, sizeof *heads);
    recs ev = disc_events(a, thing);        /* only the events about it */
    for (size_t i = 0; i < ev.n; i++) {
        const rec_record *e = ev.v[i];
        if (!sets(e, thing, field)) continue;
        for (size_t f = 0; f < e->nfields; f++)
            if (!strcmp(e->fields[f].name, "Replaces")) strlist_add(&replaced, e->fields[f].value);
    }
    for (size_t i = 0; i < ev.n; i++) {
        const rec_record *e = ev.v[i];
        char k[33];
        if (!sets(e, thing, field) || !rec_get(e, "EventId")) continue;
        event_key(e, k);
        if (*k && !strlist_has(&replaced, k) && !strlist_has(heads, k)) strlist_add(heads, k);
    }
    strlist_free(&replaced);
}

/* e (not yet in the catalogue) changes field of thing: it says so, and replaces every head */
void sets_field(const archive *a, rec_record *e, const char *thing, const char *field)
{
    strlist heads;
    field_heads(a, thing, field, &heads);
    rec_add(e, "Sets", field);
    for (size_t i = 0; i < heads.n; i++) {
        int have = 0;
        for (size_t f = 0; f < e->nfields && !have; f++)
            have = !strcmp(e->fields[f].name, "Replaces") && !strcmp(e->fields[f].value, heads.v[i]);
        if (!have) rec_add(e, "Replaces", heads.v[i]);
    }
    strlist_free(&heads);
}

/* ------------------------------------------------------------------ a disc's tags after it was burned
 * A disc's tags.tsv is as it was burned; a later change to one path's tags or caption is a field
 * of an event about the disc, "Tags: PATH<TAB>a, b" (or "Caption: PATH<TAB>text"), with
 * "Sets: Tags:PATH" and Replaces, so changes are appended, merge by event id, and two made
 * apart are a conflict (field_conflicts). The tags now are the burned ones with each path's
 * newest change laid over them. */

void tag_change(const archive *a, rec_record *ev, const char *disc_id, const char *field, const char *path, const char *value)
{
    char *v = xprintf("%s\t%s", path, value), *key = xprintf("%s:%s", field, path);
    rec_add(ev, field, v);
    sets_field(a, ev, disc_id, key);
    free(v);
    free(key);
}

/* the value an event gives one path's field (Tags or Caption), or NULL */
static const char *changed_value(const rec_record *e, const char *field, const char *path)
{
    size_t n = strlen(path);
    for (size_t f = 0; f < e->nfields; f++)
        if (!strcmp(e->fields[f].name, field) && !strncmp(e->fields[f].value, path, n) && e->fields[f].value[n] == '\t')
            return e->fields[f].value + n + 1;
    return NULL;
}

typedef struct {
    char *path, *tags, *caption;
} tag_line;

static int by_tag_path(const void *x, const void *y)
{
    return strcmp(((const tag_line *)x)->path, ((const tag_line *)y)->path);
}

char *disc_tags_text(const archive *a, const char *disc_id, const char *burned)
{
    recs ev = disc_events(a, disc_id);   /* only its events */
    tag_line *v = NULL;
    size_t n = 0;
    char *text = burned ? read_text(burned) : NULL;
    for (char *l = text; l && *l;) {                    /* as burned: path <TAB> tags [<TAB> caption] */
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        if (*l && *l != '#') {
            char *t1 = strchr(l, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
            if (t1) *t1 = 0;
            if (t2) *t2 = 0;
            v = xrealloc(v, (n + 1) * sizeof *v);
            v[n++] = (tag_line){ xstrdup(l), xstrdup(t1 ? t1 + 1 : ""), xstrdup(t2 ? t2 + 1 : "") };
        }
        l = nl ? nl + 1 : l + strlen(l);
    }
    free(text);
    strlist done = { 0 };                               /* each path's newest change, over it */
    for (size_t i = 0; i < ev.n; i++) {
        const rec_record *e = ev.v[i];
        const char *d = rec_get(e, "Disc");
        if (!d || strcmp(d, disc_id)) continue;
        for (size_t f = 0; f < e->nfields; f++) {
            if (strcmp(e->fields[f].name, "Sets") || (strncmp(e->fields[f].value, "Tags:", 5) && strncmp(e->fields[f].value, "Caption:", 8)))
                continue;
            const char *key = e->fields[f].value, *colon = strchr(key, ':'), *path = colon + 1;
            if (strlist_has(&done, key)) continue;
            strlist_add(&done, key);
            char field[8];
            snprintf(field, sizeof field, "%.*s", (int)(colon - key), key);
            strlist heads;
            field_heads(a, disc_id, key, &heads);
            const char *value = NULL;                   /* of two made apart (a conflict), the later */
            for (size_t k = 0; k < ev.n; k++) {
                char id[33];
                event_key(ev.v[k], id);
                if (strlist_has(&heads, id) && changed_value(ev.v[k], field, path)) value = changed_value(ev.v[k], field, path);
            }
            strlist_free(&heads);
            if (!value) continue;
            size_t at = 0;
            while (at < n && strcmp(v[at].path, path)) at++;
            if (at == n) {
                v = xrealloc(v, (n + 1) * sizeof *v);
                v[n++] = (tag_line){ xstrdup(path), xstrdup(""), xstrdup("") };
            }
            char **slot = !strcmp(field, "Tags") ? &v[at].tags : &v[at].caption;
            free(*slot);
            *slot = xstrdup(value);
        }
    }
    strlist_free(&done);
    recs mine = disc_objects(a, disc_id);
    for (size_t i = 0; i < mine.n; i++) {               /* its data objects' tags now, at their places on it */
        const rec_record *o = mine.v[i];
        const char *d = rec_get(o, "Disc"), *path = rec_get(o, "Path"), *value;
        if (!d || strcmp(d, disc_id) || !path || !rec_get(o, "Uuid") || !(value = object_field(a, rec_get(o, "Uuid"), "Tags")))
            continue;
        size_t at = 0;
        while (at < n && strcmp(v[at].path, path)) at++;
        if (at == n) {
            v = xrealloc(v, (n + 1) * sizeof *v);
            v[n++] = (tag_line){ xstrdup(path), xstrdup(""), xstrdup("") };
        }
        free(v[at].tags);
        v[at].tags = xstrdup(value);
    }
    if (n) qsort(v, n, sizeof *v, by_tag_path);
    sbuf out = { 0 };
    sb_puts(&out, "# path (a folder or a file, relative to data/)\ttags\tcaption (what sampled images show, if analysed)\n");
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        if (*v[i].tags || *v[i].caption) {
            sb_printf(&out, "%s\t%s%s%s\n", v[i].path, v[i].tags, *v[i].caption ? "\t" : "", v[i].caption);
            kept++;
        }
        free(v[i].path);
        free(v[i].tags);
        free(v[i].caption);
    }
    free(v);
    if (!kept) {
        free(out.s);
        return NULL;
    }
    return out.s;
}

/* ------------------------------------------------------------------ a data object's tags and name now
 * A data object's tags and its name on discs (Slug) change by events about it, "Object:
 * object:UUID", with "Tags: a, b" or "Slug: NAME" and Sets and Replaces, so they follow the object
 * to every disc it is on and every disc made from it, not a path on one disc. */

static const char *object_field_own(const archive *a, const char *uuid, const char *field);

const char *object_field(const archive *a, const char *uuid, const char *field)
{
    const char *own = object_field_own(a, uuid, field);
    if (own) return own;
    recs same = disc_events(a, "#same");
    for (size_t i = 0; i < same.n; i++) {          /* one said to be this one (arv same): its value */
        const char *of = rec_get(same.v[i], "Object"), *to = rec_get(same.v[i], "SameAs");
        if (of && to && !strncmp(of, "object:", 7) && !strncmp(to, "object:", 7) && !strcmp(to + 7, uuid)) {
            const char *v = object_field_own(a, of + 7, field);
            if (v) return v;
        }
    }
    return NULL;
}

static const char *object_field_own(const archive *a, const char *uuid, const char *field)
{
    char *thing = xprintf("object:%s", uuid);
    strlist heads;
    field_heads(a, thing, field, &heads);
    const char *value = NULL;               /* of two made apart (a conflict), the later */
    recs ev = disc_events(a, thing);
    for (size_t i = 0; heads.n && i < ev.n; i++) {
        char id[33];
        event_key(ev.v[i], id);
        if (strlist_has(&heads, id) && rec_get(ev.v[i], field)) value = rec_get(ev.v[i], field);
    }
    strlist_free(&heads);
    free(thing);
    return value;
}

rec_record *object_event_like(const rec_record *ev, const char *uuid)
{
    rec_record *r = rec_alloc("Event");
    char *thing = xprintf("object:%s", uuid);
    rec_add(r, "Object", thing);
    free(thing);
    static const char *const kept[] = { "Type", "Date", "Outcome", "Authorship", "Agent", "Note", NULL };
    for (size_t f = 0; f < ev->nfields; f++)
        for (int k = 0; kept[k]; k++)
            if (!strcmp(ev->fields[f].name, kept[k])) rec_add(r, kept[k], ev->fields[f].value);
    rec_add(r, "PendingId", "yes");
    return r;
}

const rec_record *object_at(const archive *a, const char *disc_id, const char *path)
{
    for (size_t i = 0; i < a->objects.n; i++) {
        const rec_record *o = a->objects.v[i];
        if (rec_get(o, "Disc") && !strcmp(rec_get(o, "Disc"), disc_id) && rec_get(o, "Path") && !strcmp(rec_get(o, "Path"), path)
            && rec_get(o, "Uuid"))
            return o;
    }
    return NULL;
}

char *object_tags_now(const arv_home *h, const archive *a, const rec_record *o)
{
    const char *set = rec_get(o, "Uuid") ? object_field(a, rec_get(o, "Uuid"), "Tags") : NULL;
    if (set) return xstrdup(set);
    const char *disc = rec_get(o, "Disc"), *path = rec_get(o, "Path");
    if (!disc || !path) return xstrdup("");
    char *tf = home_volume_file(h, disc, "tags.tsv"), *text = disc_tags_text(a, disc, access(tf, F_OK) ? NULL : tf);
    sbuf b = { 0 };
    sb_puts(&b, "");
    strlist seen = { 0 };
    for (char *l = text; l && *l;) {        /* its folders' lines and its own */
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        char *t1 = *l != '#' ? strchr(l, '\t') : NULL;
        if (t1) {
            *t1 = 0;
            char *t2 = strchr(t1 + 1, '\t');
            if (t2) *t2 = 0;
            size_t n = strlen(l);
            if (!strcmp(l, ".") || !strcmp(l, path) || (!strncmp(path, l, n) && path[n] == '/'))
                for (char *t = strtok(t1 + 1, ","); t; t = strtok(NULL, ",")) {
                    while (*t == ' ') t++;
                    if (!*t || strlist_has(&seen, t)) continue;
                    strlist_add(&seen, t);
                    sb_printf(&b, "%s%s", b.len ? ", " : "", t);
                }
        }
        l = nl ? nl + 1 : l + strlen(l);
    }
    strlist_free(&seen);
    free(text);
    free(tf);
    return b.s;
}

const rec_record *object_at_uuid(const archive *a, const char *uuid)
{
    const rec_record *o = NULL;             /* its newest record */
    for (size_t i = 0; i < a->objects.n; i++)
        if (rec_get(a->objects.v[i], "Uuid") && !strcmp(rec_get(a->objects.v[i], "Uuid"), uuid)) o = a->objects.v[i];
    return o;
}

const char *object_slug(const archive *a, const rec_record *o)
{
    const char *s = rec_get(o, "Uuid") ? object_field(a, rec_get(o, "Uuid"), "Slug") : NULL;
    return s ? s : rec_get(o, "Slug") ? rec_get(o, "Slug") : rec_get(o, "Name") ? rec_get(o, "Name") : "?";
}

/* the data object a word names: its Uuid (or its first 8 or more characters), or its name on discs
 * now; refused when it names none, or more than one */
const char *object_by_word(const archive *cat, const char *text)
{
    strlist uuids = { 0 }, names = { 0 };
    size_t n = strlen(text);
    for (int pass = 0; pass < 2 && !uuids.n; pass++)
        for (size_t i = 0; i < cat->objects.n; i++) {
            const rec_record *o = cat->objects.v[i];
            const char *u = rec_get(o, "Uuid");
            if (!u || strlist_has(&uuids, u)) continue;
            int hit = !pass ? n >= 8 && !strncmp(u, text, n) : !strcmp(object_slug(cat, o), text) || !strcmp(get_or(o, "Name", ""), text);
            if (!hit) continue;
            strlist_add(&uuids, u);
            char *line = xprintf("  %s  %s (version %s on %s, data/%s)", u, object_slug(cat, o), get_or(o, "Version", "?"),
                                 get_or(o, "Disc", "?"), get_or(o, "Path", "?"));
            strlist_add(&names, line);
            free(line);
        }
    if (!uuids.n) die("%s: no data object has that name or Uuid (arv objects lists them)", text);
    if (uuids.n > 1) {
        fprintf(stderr, "Error: %s names %zu data objects; give one by its Uuid:\n", text, uuids.n);
        for (size_t i = 0; i < names.n; i++) fprintf(stderr, "%s\n", names.v[i]);
        exit(1);
    }
    for (size_t i = 0; i < cat->objects.n; i++)
        if (!strcmp(get_or(cat->objects.v[i], "Uuid", ""), uuids.v[0])) return rec_get(cat->objects.v[i], "Uuid");
    return NULL;
}

/* the disc's tags now, to read as its tags.tsv would be read; NULL when there are none */
FILE *disc_tags_open(const archive *a, const char *disc_id, const char *burned)
{
    char *text = disc_tags_text(a, disc_id, burned);
    FILE *fp = text ? tmpfile() : NULL;
    if (fp) {
        fputs(text, fp);
        rewind(fp);
    }
    free(text);
    return fp;
}

/* fields changed apart, in two catalogues: printed (CONFLICT), counted */
size_t field_conflicts(const archive *a)
{
    size_t n = 0;
    strlist done = { 0 };
    for (size_t i = 0; i < a->events.n; i++) {
        const rec_record *e = a->events.v[i];
        const char *thing = thing_of(e);
        for (size_t f = 0; thing && f < e->nfields; f++) {
            if (strcmp(e->fields[f].name, "Sets")) continue;
            char *key = xprintf("%s\t%s", thing, e->fields[f].value);
            if (!strlist_has(&done, key)) {
                strlist_add(&done, key);
                strlist heads;
                field_heads(a, thing, e->fields[f].value, &heads);
                if (heads.n > 1) {
                    printf("CONFLICT  %s %s: changed apart, in two catalogues:\n", thing, e->fields[f].value);
                    for (size_t h = 0; h < heads.n; h++)
                        for (size_t j = 0; j < a->events.n; j++) {
                            char k[33];
                            event_key(a->events.v[j], k);
                            if (*k && !strcmp(k, heads.v[h]))
                                printf("  %s  %s  %s\n", get_or(a->events.v[j], "Date", "?"), get_or(a->events.v[j], "Agent", "?"),
                                       get_or(a->events.v[j], "Note", ""));
                        }
                    printf("  Set it again to settle it (the command that changed it); arv keeps this home's value till then.\n");
                    n++;
                }
                strlist_free(&heads);
            }
            free(key);
        }
    }
    strlist_free(&done);
    return n;
}

/* a place given on the command line that looks like a code but is none of the archive's locations:
 * say so (it is kept as written, as text), since it is often a mistyped code */
void place_check(const archive *a, const char *text)
{
    if (!text || !*text || strchr(text, ' ') || archive_location(a, text)) return;   /* words with spaces: meant as text */
    fprintf(stderr, "Note: %s is not one of the archive's locations (arv location list): recorded as written. "
                    "arv location add CODE NAME makes a place arv knows.\n", text);
}

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

/* --importance '<level> for <audience>' */
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

/* A set of strings it owns, for merges that would otherwise compare every pair */
typedef struct {
    char **v;
    size_t n, cap;
} strset;

static size_t str_hash(const char *s)
{
    size_t h = 2166136261u;                                 /* FNV-1a */
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

/* Adds s (taking it) and returns 1, or frees it and returns 0 if it was there */
static int strset_add(strset *set, char *s)
{
    if (2 * (set->n + 1) > set->cap) {
        size_t cap = set->cap ? 2 * set->cap : 64;
        strset grown = { xmalloc(cap * sizeof *grown.v), 0, cap };
        memset(grown.v, 0, cap * sizeof *grown.v);
        for (size_t i = 0; i < set->cap; i++)
            if (set->v[i]) {
                size_t at = str_hash(set->v[i]) & (grown.cap - 1);
                while (grown.v[at]) at = (at + 1) & (grown.cap - 1);
                grown.v[at] = set->v[i];
                grown.n++;
            }
        free(set->v);
        *set = grown;
    }
    size_t at = str_hash(s) & (set->cap - 1);
    for (; set->v[at]; at = (at + 1) & (set->cap - 1))
        if (!strcmp(set->v[at], s)) {
            free(s);
            return 0;
        }
    set->v[at] = s;
    set->n++;
    return 1;
}

static void strset_free(strset *set)
{
    for (size_t i = 0; i < set->cap; i++) free(set->v[i]);
    free(set->v);
    *set = (strset){ 0 };
}

static char *object_version_key(const rec_record *o)
{
    return xprintf("%s\n%s\n%s", get_or(o, "Uuid", ""), get_or(o, "Version", ""), get_or(o, "Disc", ""));
}

/* Merges other into home (catalog.merge): new discs, bindings, places, collections and revisions
 * are added, events and appraisals appended when new. With prefer_other, existing records take the
 * other's fields (a sealed disc's cut-down record never replaces a full one). Returns the number
 * of new events; added and updated get disc ids. */
size_t archive_merge(archive *home, const archive *other, int prefer_other, strlist *added, strlist *updated, strlist *clashes)
{
    size_t events = 0;
    if (!home->homes.n && other->homes.n) recs_add(&home->homes, other->homes.v[0]);   /* a home rebuilt from a disc */
    recs fresh = { 0 };                                    /* added after the loop: archive_disc's index stays */
    strset fresh_ids = { 0 };
    for (size_t i = 0; i < other->discs.n; i++) {
        rec_record *d = other->discs.v[i], *existing = archive_disc(home, get_or_empty(d, "Id"));
        if (!existing && !strset_add(&fresh_ids, xstrdup(get_or_empty(d, "Id"))))
            for (size_t k = 0; k < fresh.n && !existing; k++)  /* the other's own id twice */
                if (!strcmp(get_or_empty(fresh.v[k], "Id"), get_or_empty(d, "Id"))) existing = fresh.v[k];
        if (!existing) {
            recs_add(&fresh, d);
            strlist_add(added, get_or_empty(d, "Id"));
        } else if (rec_get(existing, "Uuid") && rec_get(d, "Uuid") && strcmp(rec_get(existing, "Uuid"), rec_get(d, "Uuid"))) {
            strlist_add(clashes, get_or_empty(d, "Id"));    /* one id, two images: two catalogues each made "the next" disc */
        } else if (prefer_other && !same_fields(existing, d) && !(rec_get(d, "Withheld") && !rec_get(existing, "Withheld"))) {
            replace_fields(existing, d);
            strlist_add(updated, get_or_empty(d, "Id"));
        } else {                                            /* notes only add up: the other's new ones join */
            int more = 0;
            for (size_t f = 0; f < d->nfields; f++) {
                if (strcmp(d->fields[f].name, "Note")) continue;
                int have = 0;
                for (size_t g = 0; g < existing->nfields && !have; g++)
                    have = !strcmp(existing->fields[g].name, "Note") && !strcmp(existing->fields[g].value, d->fields[f].value);
                if (!have) rec_add(existing, "Note", d->fields[f].value), more = 1;
            }
            if (more) strlist_add(updated, get_or_empty(d, "Id"));
        }
    }
    for (size_t i = 0; i < fresh.n; i++) recs_add(&home->discs, fresh.v[i]);
    free(fresh.v);
    strset_free(&fresh_ids);
    for (size_t i = 0; i < other->bindings.n; i++) {
        rec_record *b = other->bindings.v[i], *existing = by_key(&home->bindings, "Volume", rec_get(b, "Volume"), 0);
        if (strlist_has(clashes, get_or_empty(b, "Volume"))) continue;
        if (!existing) recs_add(&home->bindings, b);
        else if (prefer_other) replace_fields(existing, b);
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
    for (size_t i = 0; i < other->revisions.n; i++) {        /* a log: revisions are added, never edited */
        rec_record *r = other->revisions.v[i], *existing = by_key(&home->revisions, "Node", rec_get(r, "Node"), 0);
        if (!existing) {
            recs_add(&home->revisions, r);
            continue;
        }
        for (size_t f = 0; f < r->nfields; f++) {           /* except Keep and Lost, added later: unioned */
            const char *name = r->fields[f].name, *value = r->fields[f].value;
            if (strcmp(name, "Keep") && strcmp(name, "Lost")) continue;
            int have = 0;
            for (size_t g = 0; g < existing->nfields && !have; g++)
                have = !strcmp(existing->fields[g].name, name) && (!strcmp(name, "Keep") || !strcmp(existing->fields[g].value, value));
            if (!have) rec_add(existing, name, value);
        }
    }
    strset have = { 0 };                                   /* one object version on one disc: Uuid, Version, Disc */
    for (size_t k = 0; k < home->objects.n; k++) strset_add(&have, object_version_key(home->objects.v[k]));
    for (size_t i = 0; i < other->objects.n; i++) {
        const rec_record *o = other->objects.v[i];
        if (strlist_has(clashes, get_or(o, "Disc", ""))) continue;
        if (strset_add(&have, object_version_key(o))) recs_add(&home->objects, other->objects.v[i]);
    }
    strset_free(&have);
    /* events, by id: the same event in both is kept once (the home's copy; arv audit says if they differ) */
    strset ids = { 0 };
    for (size_t i = 0; i < home->events.n; i++) {
        const char *id = rec_get(home->events.v[i], "EventId");
        if (id && strlen(id) == 32) strset_add(&ids, xstrdup(id));
    }
    for (size_t i = 0; i < other->events.n; i++) {
        rec_record *event = other->events.v[i];
        const char *id = rec_get(event, "EventId");
        if (rec_get(event, "Disc") && strlist_has(clashes, rec_get(event, "Disc"))) continue;
        if (id && strlen(id) == 32 && !strset_add(&ids, xstrdup(id))) continue;
        recs_add(&home->events, event);
        events++;
    }
    strset_free(&ids);
    for (size_t i = 0; i < other->appraisals.n; i++)
        if (!in_recs(&home->appraisals, other->appraisals.v[i])) recs_add(&home->appraisals, other->appraisals.v[i]);
    return events;
}
