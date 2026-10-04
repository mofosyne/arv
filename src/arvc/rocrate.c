/*
 * RO-Crate 1.2 metadata for a disc's payload (--ro-crate; src/arv/rocrate.py): the crate files go in
 * data/ (ro-crate-metadata.json, ro-crate-preview.html) and are listed in the bag manifests like any
 * payload file, as the RO-Crate 1.2 BagIt notes describe. The source folder is not changed.
 */
#define _XOPEN_SOURCE 700
#include "arvc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FILE_LIMIT 100000   /* above this, files are summarised rather than listed one by one */

/* ------------------------------------------------------------------ a small JSON tree */

typedef struct jv {
    char kind;                  /* 'o' object, 'a' array, 's' string, 'n' null */
    char *str;
    char **keys;                /* objects: in insertion order, as Python dicts keep them */
    struct jv **vals;
    size_t n;
} jv;

static jv *jnew(char kind, const char *s)
{
    jv *v = xmalloc(sizeof *v);
    memset(v, 0, sizeof *v);
    v->kind = s || kind != 's' ? kind : 'n';
    if (s) v->str = xstrdup(s);
    return v;
}
static jv *jstr(const char *s) { return jnew('s', s); }
static jv *jobj(void) { return jnew('o', NULL); }
static jv *jarr(void) { return jnew('a', NULL); }

static void jput(jv *o, const char *key, jv *val)    /* sets or appends, like d[key] = val */
{
    for (size_t i = 0; key && i < o->n; i++)
        if (!strcmp(o->keys[i], key)) { o->vals[i] = val; return; }
    o->keys = xrealloc(o->keys, (o->n + 1) * sizeof *o->keys);
    o->vals = xrealloc(o->vals, (o->n + 1) * sizeof *o->vals);
    o->keys[o->n] = key ? xstrdup(key) : NULL;
    o->vals[o->n++] = val;
}
static void jpush(jv *a, jv *val) { jput(a, NULL, val); }
static jv *jref(const char *id)
{
    jv *o = jobj();
    jput(o, "@id", jstr(id));
    return o;
}
static void jfree(jv *v)
{
    if (!v) return;
    for (size_t i = 0; i < v->n; i++) {
        free(v->keys[i]);
        int shared = 0;                 /* the same value may sit under two keys (creator, publisher) */
        for (size_t k = 0; k < i; k++) shared |= v->vals[k] == v->vals[i];
        if (!shared) jfree(v->vals[i]);
    }
    free(v->keys);
    free(v->vals);
    free(v->str);
    free(v);
}

static void jquote(sbuf *b, const char *s)      /* json.dumps(ensure_ascii=False) */
{
    sb_puts(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\n': sb_puts(b, "\\n"); break;
        case '\r': sb_puts(b, "\\r"); break;
        case '\t': sb_puts(b, "\\t"); break;
        case '\b': sb_puts(b, "\\b"); break;
        case '\f': sb_puts(b, "\\f"); break;
        default:
            if (c < 0x20) sb_printf(b, "\\u%04x", c);
            else sb_add(b, s, 1);
        }
    }
    sb_puts(b, "\"");
}

static void jdump(sbuf *b, const jv *v, int depth)    /* json.dumps(indent=1) */
{
    if (v->kind == 'n') { sb_puts(b, "null"); return; }
    if (v->kind == 's') { jquote(b, v->str); return; }
    const char *open = v->kind == 'o' ? "{" : "[", *close = v->kind == 'o' ? "}" : "]";
    if (!v->n) { sb_puts(b, open); sb_puts(b, close); return; }
    sb_puts(b, open);
    for (size_t i = 0; i < v->n; i++) {
        sb_puts(b, i ? ",\n" : "\n");
        for (int d = 0; d <= depth; d++) sb_puts(b, " ");
        if (v->kind == 'o') {
            jquote(b, v->keys[i]);
            sb_puts(b, ": ");
        }
        jdump(b, v->vals[i], depth + 1);
    }
    sb_puts(b, "\n");
    for (int d = 0; d < depth; d++) sb_puts(b, " ");
    sb_puts(b, close);
}

/* ------------------------------------------------------------------ the crate */

static char *temporal(const char *coverage)     /* '2020-2025' -> '2020/2025'; '2023' stays */
{
    if (coverage && strlen(coverage) == 9 && coverage[4] == '-' && strspn(coverage, "0123456789") == 4
        && strspn(coverage + 5, "0123456789") == 4)
        return xprintf("%.4s/%s", coverage, coverage + 5);
    return xstrdup(coverage);
}

static char *const *format_row(const formats *f, const char *path)
{
    for (size_t i = 0; f && i < f->n; i++)
        if (!strcmp(f->rows[i][0], path)) return f->rows[i];
    return NULL;
}

/* ro-crate-metadata.json for one disc (rocrate.build, then rocrate.dumps) */
char *rocrate_metadata(const rec_record *disc, const entries *files, const formats *fmt)
{
    jv *root = jobj(), *graph = jarr(), *meta = jobj(), *id = jobj();
    jput(root, "@id", jstr("./"));
    jput(root, "@type", jstr("Dataset"));
    jput(root, "name", jstr(rec_get(disc, "Title")));
    jput(root, "identifier", jref("#disc-id"));
    jput(root, "datePublished", jstr(rec_get(disc, "Date")));
    char *desc = rec_get(disc, "Description") && *rec_get(disc, "Description")
        ? xstrdup(rec_get(disc, "Description")) : xprintf("Archival disc %s", rec_get(disc, "Id"));
    jput(root, "description", jstr(desc));
    jput(meta, "@id", jstr("ro-crate-metadata.json"));
    jput(meta, "@type", jstr("CreativeWork"));
    jput(meta, "conformsTo", jref("https://w3id.org/ro/crate/1.2"));
    jput(meta, "about", jref("./"));
    jpush(graph, meta);
    jpush(graph, root);
    jput(id, "@id", jstr("#disc-id"));
    jput(id, "@type", jstr("PropertyValue"));
    jput(id, "name", jstr("Disc id"));
    jput(id, "propertyID", jstr("archive disc id"));
    jput(id, "value", jstr(rec_get(disc, "Id")));
    jpush(graph, id);
    if (rec_get(disc, "Coverage") && *rec_get(disc, "Coverage")) {
        char *t = temporal(rec_get(disc, "Coverage"));
        jput(root, "temporalCoverage", jstr(t));
        free(t);
    }
    jv *subjects = jarr();
    for (size_t i = 0; i < disc->nfields; i++)
        if (!strcmp(disc->fields[i].name, "Subject")) jpush(subjects, jstr(disc->fields[i].value));
    if (subjects->n == 1) {
        jput(root, "keywords", subjects->vals[0]);
        subjects->n = 0;
        jfree(subjects);
    } else if (subjects->n) {
        jput(root, "keywords", subjects);
    } else {
        jfree(subjects);
    }
    const char *creator = rec_get(disc, "Creator");
    if (creator && *creator) {
        jv *ref = jref("#creator"), *person = jobj();
        jput(root, "creator", ref);
        jput(root, "publisher", ref);
        jput(person, "@id", jstr("#creator"));
        jput(person, "@type", jstr("Person"));
        jput(person, "name", jstr(creator));
        jpush(graph, person);
    }
    const char *rights = rec_get(disc, "Rights");
    if (rights && *rights) {
        if (!strncmp(rights, "http://", 7) || !strncmp(rights, "https://", 8)) {
            jv *lic = jobj();
            jput(root, "license", jref(rights));
            jput(lic, "@id", jstr(rights));
            jput(lic, "@type", jstr("CreativeWork"));
            jput(lic, "name", jstr(rights));
            jput(lic, "description", jstr("Licence for the contents of this disc"));
            jpush(graph, lic);
        } else {
            jput(root, "copyrightNotice", jstr(rights));
        }
    }
    if (rec_get(disc, "Part") && *rec_get(disc, "Part")) {
        jv *set = jobj();
        char *name = xprintf("%s (%s)", rec_get(disc, "Set") ? rec_get(disc, "Set") : "None", rec_get(disc, "Part"));
        jput(root, "isPartOf", jref("#set"));
        jput(set, "@id", jstr("#set"));
        jput(set, "@type", jstr("CreativeWork"));
        jput(set, "name", jstr(name));
        jpush(graph, set);
        free(name);
    }
    uint64_t total = 0;
    for (size_t i = 0; i < files->n; i++) total += files->v[i].size;
    char num[32];
    snprintf(num, sizeof num, "%llu", (unsigned long long)total);
    jput(root, "contentSize", jstr(num));

    if (files->n > FILE_LIMIT) {
        char *more = xprintf("%s (%zu files; per-file details are in the disc's catalog/listings, catalog/formats and "
                             "manifest files)", desc, files->n);
        jput(root, "description", jstr(more));
        free(more);
    } else {
        jv *parts = jarr();
        strlist urls = { 0 };
        char *const **url_rows = NULL;
        for (size_t i = 0; i < files->n; i++) {
            const entry *e = &files->v[i];
            sbuf q = { 0 };
            url_quote(&q, e->path);
            jv *ent = jobj();
            jput(ent, "@id", jstr(q.s ? q.s : ""));
            jput(ent, "@type", jstr("File"));
            jput(ent, "name", jstr(strrchr(e->path, '/') ? strrchr(e->path, '/') + 1 : e->path));
            snprintf(num, sizeof num, "%llu", (unsigned long long)e->size);
            jput(ent, "contentSize", jstr(num));
            struct tm tm;
            char when[32];
            gmtime_r(&e->mtime, &tm);
            strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tm);
            jput(ent, "dateModified", jstr(when));
            char *const *row = format_row(fmt, e->path);
            if (row) {
                jv *enc = jarr();
                if (*row[4]) jpush(enc, jstr(row[4]));
                if (*row[1] && strcmp(row[1], "UNKNOWN")) {
                    char *url = xprintf("https://www.nationalarchives.gov.uk/PRONOM/%s", row[1]);
                    jpush(enc, jref(url));
                    if (!strlist_has(&urls, url)) {
                        strlist_add(&urls, url);
                        url_rows = xrealloc(url_rows, urls.n * sizeof *url_rows);
                        url_rows[urls.n - 1] = row;
                    }
                    free(url);
                }
                if (enc->n == 1) {
                    jput(ent, "encodingFormat", enc->vals[0]);
                    enc->n = 0;
                    jfree(enc);
                } else if (enc->n) {
                    jput(ent, "encodingFormat", enc);
                } else {
                    jfree(enc);
                }
            }
            jpush(parts, jref(q.s ? q.s : ""));
            jpush(graph, ent);
            free(q.s);
        }
        jput(root, "hasPart", parts);
        for (size_t i = 0; i < urls.n; i++) {
            char *const *row = url_rows[i];
            jv *page = jobj();
            char *name = *row[3] ? xprintf("%s %s", *row[2] ? row[2] : row[1], row[3]) : xstrdup(*row[2] ? row[2] : row[1]);
            jput(page, "@id", jstr(urls.v[i]));
            jput(page, "@type", jstr("WebPage"));
            jput(page, "name", jstr(name));
            jpush(graph, page);
            free(name);
        }
        strlist_free(&urls);
        free(url_rows);
    }
    free(desc);
    jv *doc = jobj();
    jput(doc, "@context", jstr("https://w3id.org/ro/crate/1.2/context"));
    jput(doc, "@graph", graph);
    sbuf out = { 0 };
    jdump(&out, doc, 0);
    sb_puts(&out, "\n");
    jfree(doc);
    return out.s;
}

/* ro-crate-preview.html: the small page RO-Crate tools show; the full viewer is ../index.html */
char *rocrate_preview(const rec_record *disc, const entries *files)
{
    uint64_t total = 0;
    for (size_t i = 0; i < files->n; i++) total += files->v[i].size;
    char size[32];
    human_size(total, size);
    const char *title = rec_get(disc, "Title") ? rec_get(disc, "Title") : "None";
    sbuf b = { 0 };
    sb_puts(&b, "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>");
    html_esc(&b, title);
    sb_puts(&b, "</title></head><body><h1>");
    html_esc(&b, title);
    sb_puts(&b, "</h1><p>");
    html_esc(&b, rec_get(disc, "Description") ? rec_get(disc, "Description") : "");
    sb_puts(&b, "</p><p>Disc ");
    html_esc(&b, rec_get(disc, "Id") ? rec_get(disc, "Id") : "None");
    sb_printf(&b, " &middot; %zu files, ", files->n);
    html_esc(&b, size);
    sb_puts(&b, "</p><p>See <a href=\"../index.html\">index.html</a> on the disc for the full file list and "
                "<a href=\"ro-crate-metadata.json\">ro-crate-metadata.json</a> for the machine-readable description.</p>"
                "</body></html>\n");
    return b.s;
}
