/* Reading recfiles (see rec.h). */
#define _POSIX_C_SOURCE 200809L
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

int rec_read(const char *path, rec_file *out, int *bad_line)
{
    FILE *fp = fopen(path, "rb");
    char *line = NULL;
    size_t cap = 0, rcap = 0, fcap = 0;
    ssize_t len;
    int lineno = 0, err = 0;
    rec_record cur = { 0 };
    const char *cur_type = NULL;

    memset(out, 0, sizeof *out);
    if (!fp) return -1;
    for (;;) {
        len = getline(&line, &cap, fp);
        int end = len < 0;
        if (!end) {
            lineno++;
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
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
            ssize_t nlen = getline(&next, &ncap, fp);
            if (nlen < 0) { free(next); break; }
            lineno++;
            while (nlen > 0 && (next[nlen - 1] == '\n' || next[nlen - 1] == '\r')) next[--nlen] = 0;
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
    fclose(fp);
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
