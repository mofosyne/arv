/* The set vocabulary (see vocab.h). */
#define _XOPEN_SOURCE 700
#include "vocab.h"
#include "rec.h"

#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *must(void *p)
{
    if (!p) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static char *dup(const char *s) { return must(strdup(s)); }

void strlist_add(strlist *l, const char *s)
{
    l->v = must(realloc(l->v, (l->n + 1) * sizeof *l->v));
    l->v[l->n++] = dup(s);
}

void strlist_free(strlist *l)
{
    for (size_t i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = 0;
}

int strlist_has(const strlist *l, const char *s)
{
    for (size_t i = 0; i < l->n; i++)
        if (!strcmp(l->v[i], s)) return 1;
    return 0;
}

void vocab_word(const char *text, char *out)
{
    for (; text && *text; text++)
        if (isalnum((unsigned char)*text) && !((unsigned char)*text & 0x80)) *out++ = (char)toupper((unsigned char)*text);
    *out = 0;
}

static char *trimmed(const char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *p = dup(s), *e = p + strlen(p);
    while (e > p && isspace((unsigned char)e[-1])) *--e = 0;
    return p;
}

static void upper(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

const vset *vocab_get(const vocab *v, const char *code)
{
    if (!code) return NULL;
    char *c = dup(code);
    upper(c);
    for (size_t i = 0; i < v->n; i++)
        if (!strcmp(v->e[i].code, c)) { free(c); return &v->e[i]; }
    free(c);
    return NULL;
}

static const char *alias(const vocab *v, const char *key)
{
    for (size_t i = 0; i < v->alias_keys.n; i++)
        if (!strcmp(v->alias_keys.v[i], key)) return v->alias_codes.v[i];
    return NULL;
}

static int cycle_from(const vocab *v, size_t i, char *state, char *err, size_t errlen)
{
    if (state[i] == 2) return 0;
    if (state[i] == 1) {
        snprintf(err, errlen, "%s: cycle in parents through %s", v->path ? v->path : "vocabulary", v->e[i].code);
        return -1;
    }
    state[i] = 1;
    for (size_t p = 0; p < v->e[i].parents.n; p++) {
        const vset *parent = vocab_get(v, v->e[i].parents.v[p]);
        if (cycle_from(v, (size_t)(parent - v->e), state, err, errlen)) return -1;
    }
    state[i] = 2;
    return 0;
}

int vocab_load(vocab *v, const char *path, const char *text, char *err, size_t errlen)
{
    rec_file f;
    int bad = 0;
    memset(v, 0, sizeof *v);
    v->path = path ? dup(path) : NULL;
    if (path ? rec_read(path, &f, &bad) : rec_parse(text, &f, &bad)) {
        snprintf(err, errlen, "cannot read the vocabulary %s%s", path ? path : "(built in)",
                 bad ? " (a line is not a field)" : "");
        return -1;
    }
    for (size_t i = 0; i < f.nrecords; i++) {
        const rec_record *r = &f.records[i];
        if (r->descriptor || !rec_get(r, "Code")) continue;
        vset e;
        memset(&e, 0, sizeof e);
        e.code = trimmed(rec_get(r, "Code"));
        upper(e.code);
        size_t len = strlen(e.code);
        int ok = len >= 2 && len <= 8;
        for (size_t k = 0; k < len; k++) if (!isupper((unsigned char)e.code[k]) && !isdigit((unsigned char)e.code[k])) ok = 0;
        if (!ok) {
            snprintf(err, errlen, "%s: code %s must be 2-8 capital letters or digits", path ? path : "vocabulary", e.code);
            free(e.code);
            rec_free(&f);
            return -1;
        }
        e.name = dup(rec_get(r, "Name") ? rec_get(r, "Name") : e.code);
        e.scope_note = dup(rec_get(r, "ScopeNote") ? rec_get(r, "ScopeNote") : "");
        {
            char *o = trimmed(rec_get(r, "Order") ? rec_get(r, "Order") : "");
            e.order = *o && strspn(o, "0123456789") == strlen(o) ? atoi(o) : 50;
            free(o);
        }
        for (size_t k = 0; k < r->nfields; k++) {
            const char *n = r->fields[k].name;
            char *val = trimmed(r->fields[k].value);
            if (*val && !strcmp(n, "Parent")) { upper(val); strlist_add(&e.parents, val); }
            else if (*val && !strcmp(n, "Alias")) strlist_add(&e.aliases, val);
            else if (*val && !strcmp(n, "Match")) strlist_add(&e.matches, val);
            free(val);
        }
        v->e = must(realloc(v->e, (v->n + 1) * sizeof *v->e));
        v->e[v->n++] = e;
    }
    rec_free(&f);
    for (size_t i = 0; i < v->n; i++) {
        for (size_t p = 0; p < v->e[i].parents.n; p++)
            if (!vocab_get(v, v->e[i].parents.v[p])) {
                snprintf(err, errlen, "%s: %s has unknown parent %s", path ? path : "vocabulary", v->e[i].code,
                         v->e[i].parents.v[p]);
                return -1;
            }
        for (size_t a = 0; a < v->e[i].aliases.n; a++) {
            char *key = must(malloc(strlen(v->e[i].aliases.v[a]) + 1));
            vocab_word(v->e[i].aliases.v[a], key);
            const char *other = alias(v, key);
            if (other && strcmp(other, v->e[i].code)) {
                snprintf(err, errlen, "%s: alias %s of %s is already %s", path ? path : "vocabulary",
                         v->e[i].aliases.v[a], v->e[i].code, other);
                free(key);
                return -1;
            }
            if (!other) {
                strlist_add(&v->alias_keys, key);
                strlist_add(&v->alias_codes, v->e[i].code);
            }
            free(key);
        }
    }
    char *state = must(calloc(v->n + 1, 1));
    for (size_t i = 0; i < v->n; i++)
        if (cycle_from(v, i, state, err, errlen)) { free(state); return -1; }
    free(state);
    return 0;
}

void vocab_free(vocab *v)
{
    for (size_t i = 0; i < v->n; i++) {
        free(v->e[i].code);
        free(v->e[i].name);
        free(v->e[i].scope_note);
        strlist_free(&v->e[i].parents);
        strlist_free(&v->e[i].aliases);
        strlist_free(&v->e[i].matches);
    }
    free(v->e);
    strlist_free(&v->alias_keys);
    strlist_free(&v->alias_codes);
    free(v->path);
    memset(v, 0, sizeof *v);
}

const char *vocab_resolve(const vocab *v, const char *text)
{
    char *key = must(malloc(strlen(text ? text : "") + 1));
    const char *found = NULL;
    vocab_word(text, key);
    size_t len = strlen(key);
    for (int pass = 0; pass < 2 && !found; pass++) {      /* the word, then without a plural s */
        if (pass == 1) {
            if (len > 3 && key[len - 1] == 'S') key[len - 1] = 0;
            else break;
        }
        const vset *e = vocab_get(v, key);
        found = e && *key ? e->code : alias(v, key);
    }
    free(key);
    return found;
}

static int by_length_desc(const void *a, const void *b)
{
    const char *const *x = a, *const *y = b;
    size_t lx = strlen(x[0]), ly = strlen(y[0]);
    if (lx != ly) return lx > ly ? -1 : 1;
    return x[1] < y[1] ? -1 : x[1] > y[1];     /* stable: first seen first */
}

const char *vocab_guess(const vocab *v, const char *text)
{
    char *w = must(malloc(strlen(text ? text : "") + 1));
    vocab_word(text, w);
    const char *key = w + strspn(w, "0123456789");
    if (!*key) key = w;
    const char *found = NULL;
    size_t klen = strlen(key);
    if (!klen) { free(w); return NULL; }
    if ((found = alias(v, key))) goto done;
    {
        const vset *e = vocab_get(v, key);
        if (e) { found = e->code; goto done; }
    }
    {   /* codes, longest first: the word starts with one, or one starts with the word */
        const char *(*order)[2] = must(malloc((v->n + 1) * sizeof *order));
        for (size_t i = 0; i < v->n; i++) { order[i][0] = v->e[i].code; order[i][1] = (const char *)(size_t)(i + 1); }
        if (v->n) qsort(order, v->n, sizeof *order, by_length_desc);
        for (size_t i = 0; i < v->n && !found; i++) {
            const char *c = order[i][0];
            if (!strncmp(key, c, strlen(c)) || (klen >= 3 && !strncmp(c, key, klen))) found = c;
        }
        free(order);
        if (found) goto done;
    }
    {   /* aliases of four letters or more that the word starts with, longest first */
        size_t n = v->alias_keys.n;
        const char *(*order)[2] = must(malloc((n + 1) * sizeof *order));
        for (size_t i = 0; i < n; i++) { order[i][0] = v->alias_keys.v[i]; order[i][1] = (const char *)(size_t)(i + 1); }
        if (n) qsort(order, n, sizeof *order, by_length_desc);
        for (size_t i = 0; i < n && !found; i++)
            if (strlen(order[i][0]) >= 4 && !strncmp(key, order[i][0], strlen(order[i][0]))) found = alias(v, order[i][0]);
        free(order);
        if (found) goto done;
    }
    {   /* a word of an entry's name, singular */
        char *singular = dup(key);
        for (char *p = singular; *p; p++) *p = (char)tolower((unsigned char)*p);
        size_t sl = strlen(singular);
        while (sl && singular[sl - 1] == 's') singular[--sl] = 0;
        for (size_t i = 0; i < v->n && !found; i++) {
            char *name = dup(v->e[i].name);
            for (char *p = name; *p; p++) *p = *p == ',' ? ' ' : (char)tolower((unsigned char)*p);
            for (char *t = strtok(name, " \t\n"); t && !found; t = strtok(NULL, " \t\n")) {
                size_t tl = strlen(t);
                while (tl && t[tl - 1] == 's') t[--tl] = 0;
                if (!strcmp(t, singular)) found = v->e[i].code;
            }
            free(name);
        }
        free(singular);
    }
done:
    free(w);
    return found;
}

static int by_string(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void paths_into(const vocab *v, const char *code, strlist *out, int depth)
{
    const vset *e = vocab_get(v, code);
    if (!e || depth > 64) return;
    if (!e->parents.n) {
        strlist_add(out, e->code);
        return;
    }
    for (size_t p = 0; p < e->parents.n; p++) {
        strlist up = { 0 };
        paths_into(v, e->parents.v[p], &up, depth + 1);
        for (size_t i = 0; i < up.n; i++) {
            char *s = must(malloc(strlen(up.v[i]) + strlen(e->code) + 2));
            sprintf(s, "%s/%s", up.v[i], e->code);
            if (!strlist_has(out, s)) strlist_add(out, s);
            free(s);
        }
        strlist_free(&up);
    }
}

void vocab_paths(const vocab *v, const char *code, strlist *out)
{
    paths_into(v, code, out, 0);
    if (out->n) qsort(out->v, out->n, sizeof *out->v, by_string);
}

int vocab_is_ancestor(const vocab *v, const char *above, const char *code)
{
    strlist paths = { 0 };
    int found = 0;
    vocab_paths(v, code, &paths);
    for (size_t i = 0; i < paths.n && !found; i++) {
        char *p = paths.v[i], *last = strrchr(p, '/');
        if (!last) continue;
        *last = 0;
        for (char *t = strtok(p, "/"); t && !found; t = strtok(NULL, "/")) found = !strcmp(t, above);
    }
    strlist_free(&paths);
    return found;
}

void vocab_near(const vocab *v, const char *code, strlist *out)
{
    char *c = dup(code ? code : "");
    upper(c);
    for (size_t i = 0; i < v->n && out->n < 5; i++) {
        const char *e = v->e[i].code;
        if (!strncmp(e, c, 3) || strstr(c, e) || strstr(e, c)) strlist_add(out, e);
    }
    free(c);
}

static int glob(const char *pattern, const char *text)
{
    return !fnmatch(pattern, text, FNM_NOESCAPE);
}

int vocab_path_matches(const char *pattern, const char *path)
{
    char *pat = trimmed(pattern), *p = dup(path);
    int r = 0;
    for (char *q = pat; *q; q++) *q = (char)tolower((unsigned char)*q);
    for (char *q = p; *q; q++) *q = (char)tolower((unsigned char)*q);
    if (strchr(pat, '/')) {
        char *s = pat;
        while (*s == '/') s++;
        char *e = s + strlen(s);
        while (e > s && e[-1] == '/') *--e = 0;
        char *under = must(malloc(strlen(s) + 3));
        sprintf(under, "%s/*", s);
        r = glob(s, p) || glob(under, p);
        free(under);
    } else {
        for (char *t = p, *slash; !r; t = slash + 1) {
            slash = strchr(t, '/');
            if (slash) *slash = 0;
            r = glob(pat, t);
            if (!slash) break;
        }
    }
    free(pat);
    free(p);
    return r;
}

typedef struct {
    const char *code;
    size_t count;
} tally;

static int by_count(const void *a, const void *b)
{
    const tally *x = a, *y = b;
    if (x->count != y->count) return x->count > y->count ? -1 : 1;
    return strcmp(x->code, y->code);
}

void vocab_rule_suggestions(const vocab *v, char *const *paths, size_t n, strlist *out)
{
    tally *t = must(calloc(v->n + 1, sizeof *t));
    size_t nt = 0;
    double least = n * 0.1 > 1 ? n * 0.1 : 1;
    for (size_t i = 0; i < v->n; i++) {
        size_t count = 0;
        for (size_t k = 0; k < n; k++)
            for (size_t m = 0; m < v->e[i].matches.n; m++)
                if (vocab_path_matches(v->e[i].matches.v[m], paths[k])) { count++; break; }
        if (count && count >= least) {
            t[nt].code = v->e[i].code;
            t[nt++].count = count;
        }
    }
    if (nt) qsort(t, nt, sizeof *t, by_count);
    for (size_t i = 0; i < nt; i++) {       /* keep the most specific: drop codes above another */
        int above = 0;
        for (size_t j = 0; j < nt && !above; j++) above = vocab_is_ancestor(v, t[i].code, t[j].code);
        if (!above) strlist_add(out, t[i].code);
    }
    free(t);
}
