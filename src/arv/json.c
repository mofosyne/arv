/*
 * A small JSON tree: written as Python's json.dumps(indent=1, ensure_ascii=False) writes it
 * (ro-crate-metadata.json), and read (make --draft: the drafts arv describe and arv tag save).
 */
#define _XOPEN_SOURCE 700
#include "arv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jv *jnew(char kind, const char *s)
{
    jv *v = xmalloc(sizeof *v);
    memset(v, 0, sizeof *v);
    v->kind = s || kind != 's' ? kind : 'n';      /* 't': a number or true/false, as written */
    if (s) v->str = xstrdup(s);
    return v;
}
jv *jstr(const char *s) { return jnew('s', s); }
jv *jobj(void) { return jnew('o', NULL); }
jv *jarr(void) { return jnew('a', NULL); }

void jput(jv *o, const char *key, jv *val)    /* sets or appends, like d[key] = val */
{
    for (size_t i = 0; key && i < o->n; i++)
        if (!strcmp(o->keys[i], key)) { o->vals[i] = val; return; }
    o->keys = xrealloc(o->keys, (o->n + 1) * sizeof *o->keys);
    o->vals = xrealloc(o->vals, (o->n + 1) * sizeof *o->vals);
    o->keys[o->n] = key ? xstrdup(key) : NULL;
    o->vals[o->n++] = val;
}
void jpush(jv *a, jv *val) { jput(a, NULL, val); }
jv *jref(const char *id)
{
    jv *o = jobj();
    jput(o, "@id", jstr(id));
    return o;
}
void jfree(jv *v)
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

void jdump(sbuf *b, const jv *v, int depth)    /* json.dumps(indent=1) */
{
    if (v->kind == 'n') { sb_puts(b, "null"); return; }
    if (v->kind == 't') { sb_puts(b, v->str); return; }
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


/* ------------------------------------------------------------------ reading */

typedef struct {
    const char *p;
    int bad;
} jreader;

static void jspace(jreader *r)
{
    while (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r') r->p++;
}

static void utf8_put(sbuf *b, unsigned long c)
{
    char o[4];
    int n;
    if (c < 0x80) { o[0] = (char)c; n = 1; }
    else if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
    else if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F)); n = 3; }
    else { o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F)); o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F)); n = 4; }
    sb_add(b, o, (size_t)n);
}

static unsigned long hex4(jreader *r)
{
    unsigned long v = 0;
    for (int i = 0; i < 4; i++) {
        char c = *r->p++;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned long)(c - 'A' + 10);
        else { r->bad = 1; return 0; }
    }
    return v;
}

static char *jstring(jreader *r)
{
    sbuf b = { 0 };
    r->p++;                                         /* the opening quote */
    while (*r->p && *r->p != '"') {
        if (*r->p != '\\') { sb_add(&b, r->p++, 1); continue; }
        r->p++;
        char c = *r->p++;
        switch (c) {
        case '"': sb_puts(&b, "\""); break;
        case '\\': sb_puts(&b, "\\"); break;
        case '/': sb_puts(&b, "/"); break;
        case 'b': sb_puts(&b, "\b"); break;
        case 'f': sb_puts(&b, "\f"); break;
        case 'n': sb_puts(&b, "\n"); break;
        case 'r': sb_puts(&b, "\r"); break;
        case 't': sb_puts(&b, "\t"); break;
        case 'u': {
            unsigned long u = hex4(r);
            if (u >= 0xD800 && u < 0xDC00 && r->p[0] == '\\' && r->p[1] == 'u') {   /* a surrogate pair */
                r->p += 2;
                unsigned long lo = hex4(r);
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            }
            utf8_put(&b, u);
            break;
        }
        default: r->bad = 1;
        }
    }
    if (*r->p != '"') r->bad = 1;
    else r->p++;
    return b.s ? b.s : xstrdup("");
}

static jv *jvalue(jreader *r, int depth)
{
    jspace(r);
    if (depth > 64) { r->bad = 1; return jnew('n', NULL); }
    if (*r->p == '"') {
        char *s = jstring(r);
        jv *v = jstr(s);
        free(s);
        return v;
    }
    if (*r->p == '{' || *r->p == '[') {
        int obj = *r->p == '{';
        jv *v = obj ? jobj() : jarr();
        r->p++;
        jspace(r);
        if (*r->p == (obj ? '}' : ']')) { r->p++; return v; }
        while (!r->bad) {
            char *key = NULL;
            if (obj) {
                jspace(r);
                if (*r->p != '"') { r->bad = 1; break; }
                key = jstring(r);
                jspace(r);
                if (*r->p != ':') { r->bad = 1; free(key); break; }
                r->p++;
            }
            jv *item = jvalue(r, depth + 1);
            if (obj) jput(v, key, item);
            else jpush(v, item);
            free(key);
            jspace(r);
            if (*r->p == ',') { r->p++; continue; }
            if (*r->p == (obj ? '}' : ']')) { r->p++; break; }
            r->bad = 1;
        }
        return v;
    }
    /* numbers, true, false, null: kept as their text (drafts have none that matter) */
    const char *start = r->p;
    while (*r->p && !strchr(",}] \t\r\n", *r->p)) r->p++;
    if (r->p == start) r->bad = 1;
    if (r->p - start == 4 && !strncmp(start, "null", 4)) return jnew('n', NULL);
    char *text = xprintf("%.*s", (int)(r->p - start), start);
    jv *v = jnew('t', NULL);
    v->str = text;
    return v;
}

/* the JSON document in text; NULL when it is not JSON */
jv *json_parse(const char *text)
{
    jreader r = { text, 0 };
    jv *v = jvalue(&r, 0);
    jspace(&r);
    if (r.bad || *r.p) {
        jfree(v);
        return NULL;
    }
    return v;
}

jv *json_get(const jv *o, const char *key)
{
    for (size_t i = 0; o && o->kind == 'o' && i < o->n; i++)
        if (!strcmp(o->keys[i], key)) return o->vals[i];
    return NULL;
}
