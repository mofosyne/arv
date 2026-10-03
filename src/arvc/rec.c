/* Reading recfiles (see rec.h). */
#define _XOPEN_SOURCE 700
#include "rec.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup_n(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = 0;
    }
    return p;
}

static int is_name_start(int c) { return isalpha(c) || c == '%'; }
static int is_name_char(int c) { return isalnum(c) || c == '_'; }

/* the next line of text (without its line ending) in *line; returns 0 at the end */
static int next_line(const char **text, char **line, size_t *cap, ssize_t *len)
{
    const char *s = *text, *e;
    if (!*s) return 0;
    e = strchr(s, '\n');
    size_t n = e ? (size_t)(e - s) : strlen(s);
    if (n + 1 > *cap) {
        char *grown = realloc(*line, n + 1);
        if (!grown) return -1;
        *line = grown;
        *cap = n + 1;
    }
    memcpy(*line, s, n);
    (*line)[n] = 0;
    *len = (ssize_t)n;
    *text = e ? e + 1 : s + n;
    return 1;
}

int rec_parse(const char *text, rec_file *out, int *bad_line)
{
    char *line = NULL;
    size_t cap = 0, fcap = 0;
    ssize_t len = 0;
    int lineno = 0, err = 0;
    rec_record cur = { 0 };
    const char *cur_type = NULL;
    size_t rcap = 0;

    memset(out, 0, sizeof *out);
    for (;;) {
        int got = next_line(&text, &line, &cap, &len);
        if (got < 0) { err = ENOMEM; break; }
        int end = !got;
        if (!end) {
            lineno++;
            while (len > 0 && line[len - 1] == '\r') line[--len] = 0;
        }
        if (end || len == 0 || strspn(line, " \t") == (size_t)len) {   /* blank: the record ends */
            if (cur.nfields) {
                for (size_t i = 0; i < cur.nfields; i++)
                    if (cur.fields[i].name[0] == '%') cur.descriptor = 1;
                if (cur.descriptor)   /* the name lives in the descriptor's own fields, kept to the end */
                    cur_type = rec_get(&cur, "%rec");
                cur.type = cur_type;
                if (out->nrecords == rcap) {
                    rcap = rcap ? rcap * 2 : 32;
                    rec_record *n = realloc(out->records, rcap * sizeof *n);
                    if (!n) { err = ENOMEM; break; }
                    out->records = n;
                }
                out->records[out->nrecords++] = cur;
                memset(&cur, 0, sizeof cur);
                fcap = 0;
            }
            if (end) break;
            continue;
        }
        if (line[0] == '#') continue;
        if (line[0] == '+' && cur.nfields) {                           /* continuation */
            const char *more = line + 1 + (line[1] == ' ');
            rec_field *f = &cur.fields[cur.nfields - 1];
            size_t old = strlen(f->value), add = strlen(more);
            char *v = realloc(f->value, old + add + 2);
            if (!v) { err = ENOMEM; break; }
            v[old] = '\n';
            memcpy(v + old + 1, more, add + 1);
            f->value = v;
            continue;
        }
        while (len > 0 && line[len - 1] == '\\') {             /* a trailing \ joins the next line */
            char *next = NULL;
            size_t ncap = 0;
            ssize_t nlen = 0;
            int more = next_line(&text, &next, &ncap, &nlen);
            if (more <= 0) { free(next); break; }
            lineno++;
            while (nlen > 0 && next[nlen - 1] == '\r') next[--nlen] = 0;
            if ((size_t)(len + nlen + 1) > cap) {
                char *grown = realloc(line, (size_t)(len + nlen + 1));
                if (!grown) { free(next); err = ENOMEM; break; }
                line = grown;
                cap = (size_t)(len + nlen + 1);
            }
            memcpy(line + len - 1, next, (size_t)nlen + 1);
            len += nlen - 1;
            free(next);
        }
        if (err) break;
        size_t n = 0;
        if (!is_name_start((unsigned char)line[0])) n = (size_t)-1;
        else
            for (n = 1; is_name_char((unsigned char)line[n]); n++) {}
        if (n == (size_t)-1 || line[n] != ':') {
            if (bad_line) *bad_line = lineno;
            err = EINVAL;
            break;
        }
        const char *value = line + n + 1;
        if (*value == ' ' || *value == '\t') value++;
        if (cur.nfields == fcap) {
            fcap = fcap ? fcap * 2 : 16;
            rec_field *nf = realloc(cur.fields, fcap * sizeof *nf);
            if (!nf) { err = ENOMEM; break; }
            cur.fields = nf;
        }
        cur.fields[cur.nfields].name = dup_n(line, n);
        cur.fields[cur.nfields].value = dup_n(value, strlen(value));
        if (!cur.fields[cur.nfields].name || !cur.fields[cur.nfields].value) { err = ENOMEM; break; }
        cur.nfields++;
    }
    free(line);
    if (err) {
        for (size_t i = 0; i < cur.nfields; i++) {
            free(cur.fields[i].name);
            free(cur.fields[i].value);
        }
        free(cur.fields);
        rec_free(out);
        errno = err;
        return -1;
    }
    return 0;
}

int rec_read(const char *path, rec_file *out, int *bad_line)
{
    FILE *fp = fopen(path, "rb");
    char *text = NULL;
    size_t len = 0, n;
    char buf[65536];
    memset(out, 0, sizeof *out);
    if (!fp) return -1;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) {
        char *grown = realloc(text, len + n + 1);
        if (!grown) { free(text); fclose(fp); errno = ENOMEM; return -1; }
        text = grown;
        memcpy(text + len, buf, n);
        len += n;
    }
    int rerr = ferror(fp);
    fclose(fp);
    if (rerr) { free(text); errno = EIO; return -1; }
    if (!text) text = calloc(1, 1);
    else text[len] = 0;
    if (memchr(text, 0, len)) { free(text); if (bad_line) *bad_line = 0; errno = EINVAL; return -1; }
    int rc = rec_parse(text, out, bad_line);
    int e = errno;
    free(text);
    errno = e;
    return rc;
}

void rec_free(rec_file *f)
{
    for (size_t i = 0; i < f->nrecords; i++) {
        for (size_t j = 0; j < f->records[i].nfields; j++) {
            free(f->records[i].fields[j].name);
            free(f->records[i].fields[j].value);
        }
        free(f->records[i].fields);
    }
    free(f->records);
    memset(f, 0, sizeof *f);
}

const char *rec_get(const rec_record *r, const char *name)
{
    for (size_t i = 0; i < r->nfields; i++)
        if (!strcmp(r->fields[i].name, name)) return r->fields[i].value;
    return NULL;
}

const rec_record *rec_first(const rec_file *f, const char *type)
{
    for (size_t i = 0; i < f->nrecords; i++)
        if (!f->records[i].descriptor && f->records[i].type && !strcmp(f->records[i].type, type))
            return &f->records[i];
    return NULL;
}

static void *grow(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) {
        fputs("out of memory\n", stderr);
        exit(2);
    }
    return p;
}

static char *copy(const char *s)
{
    return dup_n(s, strlen(s));
}

rec_record *rec_new(rec_file *f, const char *type)
{
    f->records = grow(f->records, (f->nrecords + 1) * sizeof *f->records);
    rec_record *r = &f->records[f->nrecords++];
    memset(r, 0, sizeof *r);
    r->type = type;
    return r;
}

void rec_add(rec_record *r, const char *name, const char *value)
{
    r->fields = grow(r->fields, (r->nfields + 1) * sizeof *r->fields);
    r->fields[r->nfields].name = copy(name);
    r->fields[r->nfields].value = copy(value ? value : "");
    if (!r->fields[r->nfields].name || !r->fields[r->nfields].value) {
        fputs("out of memory\n", stderr);
        exit(2);
    }
    r->nfields++;
}

void rec_set(rec_record *r, const char *name, const char *value)
{
    size_t out = 0;
    int done = 0;
    for (size_t i = 0; i < r->nfields; i++) {
        if (!strcmp(r->fields[i].name, name)) {
            if (done) {
                free(r->fields[i].name);
                free(r->fields[i].value);
                continue;
            }
            free(r->fields[i].value);
            r->fields[i].value = copy(value);
            done = 1;
        }
        r->fields[out++] = r->fields[i];
    }
    r->nfields = out;
    if (!done) rec_add(r, name, value);
}

void rec_copy(rec_record *dst, const rec_record *src)
{
    for (size_t i = 0; i < src->nfields; i++) rec_add(dst, src->fields[i].name, src->fields[i].value);
}

void rec_clear(rec_record *r)
{
    for (size_t i = 0; i < r->nfields; i++) {
        free(r->fields[i].name);
        free(r->fields[i].value);
    }
    free(r->fields);
    r->fields = NULL;
    r->nfields = 0;
}

static void put(char **buf, size_t *len, const char *s, size_t n)
{
    *buf = grow(*buf, *len + n + 1);
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

void rec_format(const rec_record *r, char **buf, size_t *len)
{
    for (size_t i = 0; i < r->nfields; i++) {
        const char *v = r->fields[i].value, *nl = strchr(v, '\n');
        size_t first = nl ? (size_t)(nl - v) : strlen(v);
        if (i) put(buf, len, "\n", 1);
        put(buf, len, r->fields[i].name, strlen(r->fields[i].name));
        put(buf, len, first ? ": " : ":", first ? 2 : 1);
        put(buf, len, v, first);
        while (nl) {                                   /* "+ part", or "+" for an empty line */
            v = nl + 1;
            nl = strchr(v, '\n');
            size_t n = nl ? (size_t)(nl - v) : strlen(v);
            put(buf, len, n ? "\n+ " : "\n+", n ? 3 : 2);
            put(buf, len, v, n);
        }
    }
}

int rec_write(const char *path, rec_record *const *records, size_t n)
{
    char *buf = NULL;
    size_t len = 0;
    for (size_t i = 0; i < n; i++) {
        if (i) put(&buf, &len, "\n\n", 2);
        rec_format(records[i], &buf, &len);
    }
    put(&buf, &len, "\n", 1);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        free(buf);
        return -1;
    }
    int bad = fwrite(buf, 1, len, fp) != len;
    bad |= fclose(fp) != 0;
    free(buf);
    return bad ? -1 : 0;
}
