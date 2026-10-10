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

static const char *const TYPES[] = { "Home", "Disc", "Binding", "Location", "Selection", "Collection", "Revision",
                                     "Object", "Event", "Appraisal" };
enum { NTYPES = 10, DISC_TYPE = 1, EVENT_TYPE = 8 };

static recs *group(archive *a, const char *type)
{
    recs *g[] = { &a->homes, &a->discs, &a->bindings, &a->locations, &a->selections, &a->collections, &a->revisions,
                  &a->objects, &a->events, &a->appraisals };
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
}

/* every record in file order: each type's descriptor, then its records (as Catalog.records) */
void archive_records(const archive *a, recs *out)
{
    const recs *g[] = { &a->homes, &a->discs, &a->bindings, &a->locations, &a->selections, &a->collections,
                        &a->revisions, &a->objects, &a->events, &a->appraisals };
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

/* the id an event goes by: the one it was given, else its content's (an event written before ids) */
void event_key(const rec_record *e, char out[33])
{
    const char *id = rec_get(e, "EventId");
    if (id && strlen(id) == 32) memcpy(out, id, 33);
    else event_content_id(e, out);
}

static int is_event(const rec_record *r)
{
    return !r->descriptor && r->type && !strcmp(r->type, "Event");
}

/* each event without an id gets one, as its first field; two events with the same content are
 * kept apart by a Nonce (2, 3 ...) in the later one */
static void assign_event_ids(const recs *r)
{
    strlist ids = { 0 };
    for (size_t i = 0; i < r->n; i++)
        if (is_event(r->v[i]) && rec_get(r->v[i], "EventId")) strlist_add(&ids, rec_get(r->v[i], "EventId"));
    for (size_t i = 0; i < r->n; i++) {
        rec_record *e = r->v[i];
        if (!is_event(e) || rec_get(e, "EventId")) continue;
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

rec_record *archive_disc(const archive *a, const char *id)
{
    for (size_t i = 0; i < a->discs.n; i++)
        if (rec_get(a->discs.v[i], "Id") && !strcmp(rec_get(a->discs.v[i], "Id"), id)) return a->discs.v[i];
    return NULL;
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
    return !strncmp(target, "selection:", 10) ? target + 10 : NULL;
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
                                            "Coverage", "Date", "Part", "Location", "Access", NULL };

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
        const char *d = target_disc(rec_get(a->appraisals.v[i], "Target"), buf, sizeof buf);
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
/* ------------------------------------------------------------------ copies
 * Each copy of a disc is one replication event (burned, an image file, or a folder), named by a
 * letter unique among the disc's copies: A, B ... Z, AA, AB ... A check of one copy names it too. */

static int is_copy_event(const rec_record *e, const char *disc_id)
{
    const char *d = rec_get(e, "Disc"), *t = rec_get(e, "Type"), *o = rec_get(e, "Outcome");
    return d && t && !strcmp(d, disc_id) && !strcmp(t, "replication") && !(o && !strcmp(o, "failure"));
}

size_t disc_copies(const archive *a, const char *disc_id)
{
    size_t n = 0;
    for (size_t i = 0; i < a->events.n; i++) n += is_copy_event(a->events.v[i], disc_id);
    return n;
}

int copy_exists(const archive *a, const char *disc_id, const char *letter)
{
    for (size_t i = 0; i < a->events.n; i++)
        if (is_copy_event(a->events.v[i], disc_id) && rec_get(a->events.v[i], "Copy")
            && !strcmp(rec_get(a->events.v[i], "Copy"), letter))
            return 1;
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

/* the first letter no copy of the disc has (copies recorded before letters count as taking one each) */
char *copy_next(const archive *a, const char *disc_id)
{
    size_t unnamed = 0;
    for (size_t i = 0; i < a->events.n; i++)
        unnamed += is_copy_event(a->events.v[i], disc_id) && !rec_get(a->events.v[i], "Copy");
    for (size_t k = unnamed;; k++) {
        char buf[8], tmp[8];
        size_t n = 0, v = k + 1;               /* bijective base 26: 1 = A, 26 = Z, 27 = AA */
        while (v && n < sizeof tmp - 1) {
            v--;
            tmp[n++] = (char)('A' + v % 26);
            v /= 26;
        }
        for (size_t j = 0; j < n; j++) buf[j] = tmp[n - 1 - j];
        buf[n] = 0;
        if (!copy_exists(a, disc_id, buf)) return xstrdup(buf);
    }
}

/* a copy known good: read back identical as it was recorded, or a later check of that copy passed */
/* the date of the newest passed check of a copy, or NULL: a check that named it, or (of a disc with
 * one copy, burned) a drive's check that named none */
const char *copy_last_check(const archive *a, const char *disc_id, const rec_record *copy)
{
    const char *letter = rec_get(copy, "Copy"), *newest = NULL;
    int only = disc_copies(a, disc_id) == 1;
    for (size_t i = 0; i < a->events.n; i++) {
        const rec_record *e = a->events.v[i];
        const char *d = rec_get(e, "Disc"), *t = rec_get(e, "Type"), *o = rec_get(e, "Outcome"), *c = rec_get(e, "Copy");
        if (!d || !t || !o || strcmp(d, disc_id) || strcmp(t, "fixity check") || strcmp(o, "success")) continue;
        const char *note = get_or(e, "Note", "");     /* a drive's check (arv check --device), not an image file's */
        int of_a_disc = !strncmp(note, "read-back of the whole image from", 33) || !strncmp(note, "disc scan with", 14);
        if (c ? letter && !strcmp(c, letter) : only && of_a_disc && !strcmp(get_or(copy, "Form", "disc"), "disc")) {
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

/* a copy's BCA serial: recorded with it, or read by a passed check that named it; NULL if unknown */
const char *copy_bca(const archive *a, const char *disc_id, const rec_record *copy)
{
    if (rec_get(copy, "Bca")) return rec_get(copy, "Bca");
    const char *letter = rec_get(copy, "Copy");
    for (size_t i = 0; letter && i < a->events.n; i++) {
        const rec_record *e = a->events.v[i];
        const char *d = rec_get(e, "Disc"), *t = rec_get(e, "Type"), *o = rec_get(e, "Outcome"), *c = rec_get(e, "Copy");
        if (d && t && o && c && rec_get(e, "Bca") && !strcmp(d, disc_id) && !strcmp(t, "fixity check")
            && !strcmp(o, "success") && !strcmp(c, letter))
            return rec_get(e, "Bca");
    }
    return NULL;
}

/* the copy a BCA serial belongs to: its replication, or a passed check that named it and read the serial */
const rec_record *copy_by_bca(const archive *a, const char *bca)
{
    for (size_t i = 0; i < a->events.n; i++) {
        const rec_record *e = a->events.v[i];
        const char *b = rec_get(e, "Bca"), *c = rec_get(e, "Copy"), *t = rec_get(e, "Type"), *o = rec_get(e, "Outcome");
        if (b && c && t && o && !strcmp(b, bca) && strcmp(o, "failure")
            && (!strcmp(t, "replication") || !strcmp(t, "fixity check")))
            return e;
    }
    return NULL;
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
size_t archive_merge(archive *home, const archive *other, int prefer_other, strlist *added, strlist *updated, strlist *clashes)
{
    size_t events = 0;
    if (!home->homes.n && other->homes.n) recs_add(&home->homes, other->homes.v[0]);   /* a home rebuilt from a disc */
    for (size_t i = 0; i < other->discs.n; i++) {
        rec_record *d = other->discs.v[i], *existing = archive_disc(home, get_or_empty(d, "Id"));
        if (!existing) {
            recs_add(&home->discs, d);
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
    for (size_t i = 0; i < other->bindings.n; i++) {
        rec_record *b = other->bindings.v[i], *existing = by_key(&home->bindings, "Volume", rec_get(b, "Volume"), 0);
        if (strlist_has(clashes, get_or_empty(b, "Volume"))) continue;
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
    for (size_t i = 0; i < other->objects.n; i++) {       /* one object version on one disc: Uuid, Version, Disc */
        const rec_record *o = other->objects.v[i];
        int have = strlist_has(clashes, get_or(o, "Disc", ""));
        for (size_t k = 0; k < home->objects.n && !have; k++) {
            const rec_record *x = home->objects.v[k];
            have = !strcmp(get_or(x, "Uuid", ""), get_or(o, "Uuid", "")) && !strcmp(get_or(x, "Version", ""), get_or(o, "Version", ""))
                   && !strcmp(get_or(x, "Disc", ""), get_or(o, "Disc", ""));
        }
        if (!have) recs_add(&home->objects, other->objects.v[i]);
    }
    /* events, by id: the same event in both is kept once (the home's copy; arv audit says if they differ) */
    char (*keys)[33] = xmalloc((home->events.n + other->events.n + 1) * sizeof *keys);
    size_t nkeys = 0;
    for (size_t i = 0; i < home->events.n; i++) event_key(home->events.v[i], keys[nkeys++]);
    for (size_t i = 0; i < other->events.n; i++) {
        char k[33];
        if (rec_get(other->events.v[i], "Disc") && strlist_has(clashes, rec_get(other->events.v[i], "Disc"))) continue;
        event_key(other->events.v[i], k);
        int have = 0;
        for (size_t j = 0; j < nkeys && !have; j++) have = !strcmp(keys[j], k);
        if (have) continue;
        recs_add(&home->events, other->events.v[i]);
        memcpy(keys[nkeys++], k, 33);
        events++;
    }
    free(keys);
    for (size_t i = 0; i < other->appraisals.n; i++)
        if (!in_recs(&home->appraisals, other->appraisals.v[i])) recs_add(&home->appraisals, other->appraisals.v[i]);
    return events;
}
