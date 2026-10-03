/* The catalogue as arv writes it (src/arv/catalog.py Catalog): Disc, Binding, Location,
 * Collection, Event and Appraisal records, written in that order under the shared descriptors
 * (descriptors.rec), and the views of it a disc may carry (access levels, snapshots). */
#define _XOPEN_SOURCE 700
#include "arvc.h"
#include "data.h"

#include <ctype.h>
#include <errno.h>
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

static const char *const TYPES[] = { "Disc", "Binding", "Location", "Collection", "Event", "Appraisal" };

static recs *group(archive *a, const char *type)
{
    recs *g[] = { &a->discs, &a->bindings, &a->locations, &a->collections, &a->events, &a->appraisals };
    for (int i = 0; i < 6; i++)
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
            fprintf(stderr, "arvc: %s: line %d is not a field\n", path, bad);
            exit(2);
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
    const recs *g[] = { &a->discs, &a->bindings, &a->locations, &a->collections, &a->events, &a->appraisals };
    for (int i = 0; i < 6; i++) {
        int always = i == 0 || i == 4;              /* Disc and Event descriptors are always written */
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

/* collections as a snapshot may carry them: items only for disc_ids, no paths on sealed discs */
void archive_collections_for(const archive *a, const strlist *disc_ids, recs *out)
{
    strlist keep_codes = { 0 };
    recs kept = { 0 };
    for (size_t i = 0; i < a->collections.n; i++) {
        rec_record *col = a->collections.v[i], *copy = rec_alloc("Collection");
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
        if (any)        /* it and every collection it is in */
            for (rec_record *c = col; c; c = by_code(&a->collections, rec_get(c, "Parent"))) {
                if (in_list(&keep_codes, rec_get(c, "Code"))) break;
                strlist_add(&keep_codes, rec_get(c, "Code"));
            }
    }
    for (size_t i = 0; i < a->collections.n; i++)
        if (in_list(&keep_codes, rec_get(a->collections.v[i], "Code"))) recs_add(out, kept.v[i]);
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
    if (!target || !strncmp(target, "set:", 4) || !strncmp(target, "collection:", 11)) return NULL;
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
