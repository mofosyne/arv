/* File names a disc image cannot keep exactly (src/arv/names.py; tests/fixtures/names.tsv), and
 * volume labels (src/arv/image.py volume_label; tests/fixtures/labels.tsv). */
#define _XOPEN_SOURCE 700
#include "arvc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOLIET_MAX 103

static void issue(name_issues *out, const char *path, int error, const char *problem)
{
    out->v = xrealloc(out->v, (out->n + 1) * sizeof *out->v);
    out->v[out->n].path = xstrdup(path);
    out->v[out->n].error = error;
    out->v[out->n].problem = xstrdup(problem);
    out->n++;
}

/* the characters of name that are in set, sorted and space separated (Python: sorted(set(...))) */
static void chars_in(const char *name, const char *set, char *out)
{
    size_t n = 0;
    for (const char *c = "\"*:;<>?\\|"; *c; c++)     /* ASCII order */
        if (strchr(set, *c) && strchr(name, *c)) {
            if (n) out[n++] = ' ';
            out[n++] = *c;
        }
    out[n] = 0;
}

/* the issues of one name (the last part of key), worded as src/arv/names.py words them */
static void check_one(const char *key, const char *name, int udf250, name_issues *out)
{
    size_t chars = 0;
    int wide = 0, nbeyond = 0;
    sbuf beyond = { 0 };
    for (const char *s = name; *s;) {
        const char *start = s;
        unsigned long c = utf8_next(&s);
        chars++;
        if (c > 0xFF) wide = 1;
        if (c > 0xFFFF && nbeyond++ < 3) sb_add(&beyond, start, (size_t)(s - start));
    }
    char msg[512], bad[32];
    if (udf250) {
        if (nbeyond) {
            snprintf(msg, sizeof msg, "has characters beyond U+FFFF (%s), which UDF cannot store", beyond.s);
            issue(out, key, 1, msg);
        } else if (1 + chars * (wide ? 2 : 1) > 255) {
            snprintf(msg, sizeof msg, "%zu characters; UDF holds at most %s", chars,
                     wide ? "127 when a name has characters beyond U+00FF" : "254");
            issue(out, key, 1, msg);
        }
        chars_in(name, "<>:\"\\|?*", bad);
        if (*bad) {
            snprintf(msg, sizeof msg, "%s not allowed in Windows names: shown changed there", bad);
            issue(out, key, 0, msg);
        }
    } else {
        if (nbeyond) {
            const char *s = beyond.s;
            utf8_next(&s);
            snprintf(msg, sizeof msg, "cut at %.*s on Windows/macOS (Joliet/UDF end the name there)",
                     (int)(s - beyond.s), beyond.s);
            issue(out, key, 0, msg);
        }
        if (chars > JOLIET_MAX) {
            snprintf(msg, sizeof msg, "%zu characters: shortened to %d on Windows/macOS", chars, JOLIET_MAX);
            issue(out, key, 0, msg);
        }
        chars_in(name, "*:;?\\<>\"|", bad);
        if (*bad) {
            snprintf(msg, sizeof msg, "%s shown as _ or changed on Windows/macOS", bad);
            issue(out, key, 0, msg);
        }
    }
    free(beyond.s);
}

static int by_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void names_check(char *const *paths, size_t n, int udf250, name_issues *out)
{
    char **seen = NULL;
    size_t nseen = 0;
    for (size_t i = 0; i < n; i++) {
        const char *p = paths[i];
        for (const char *slash = p;; slash++) {     /* every folder above, then the file */
            if (*slash && *slash != '/') continue;
            size_t klen = (size_t)(slash - p);
            char *key = xmalloc(klen + 1);
            memcpy(key, p, klen);
            key[klen] = 0;
            int dup = 0;
            for (size_t k = 0; k < nseen && !dup; k++) dup = !strcmp(seen[k], key);
            if (!dup) {
                seen = xrealloc(seen, (nseen + 1) * sizeof *seen);
                seen[nseen++] = key;
                const char *name = strrchr(key, '/');
                check_one(key, name ? name + 1 : key, udf250, out);
            } else {
                free(key);
            }
            if (!*slash) break;
        }
    }
    /* names in one folder that differ only in (ASCII) letter case */
    if (nseen) qsort(seen, nseen, sizeof *seen, by_str);
    for (size_t i = 0; i < nseen; i++)
        for (size_t j = 0; j < i; j++) {
            const char *a = strrchr(seen[i], '/'), *b = strrchr(seen[j], '/');
            size_t pa = a ? (size_t)(a - seen[i]) : 0, pb = b ? (size_t)(b - seen[j]) : 0;
            if (pa != pb || strncmp(seen[i], seen[j], pa)) continue;
            const char *na = a ? a + 1 : seen[i], *nb = b ? b + 1 : seen[j];
            if (strcmp(na, nb) && strlen(na) == strlen(nb)) {
                int same = 1;
                for (size_t k = 0; na[k] && same; k++) same = tolower((unsigned char)na[k]) == tolower((unsigned char)nb[k]);
                if (same) {
                    /* Python's repr of the other name: '...', or "..." when it holds a ' */
                    char *msg = strchr(nb, '\'') && !strchr(nb, '"')
                        ? xprintf("differs from \"%s\" only in letter case: Windows shows only one", nb)
                        : xprintf("differs from '%s' only in letter case: Windows shows only one", nb);
                    issue(out, seen[i], 0, msg);
                    free(msg);
                    break;
                }
            }
        }
    for (size_t i = 0; i < nseen; i++) free(seen[i]);
    free(seen);
}

void names_free(name_issues *x)
{
    for (size_t i = 0; i < x->n; i++) {
        free(x->v[i].path);
        free(x->v[i].problem);
    }
    free(x->v);
    x->v = NULL;
    x->n = 0;
}

/* The volume label: the disc id, then the text as far as it fits (udf250: 126 characters, or 63
 * with any above U+00FF, no commas; hybrid: 32 bytes). Characters beyond U+FFFF are dropped. */
char *volume_label(const char *disc_id, const char *text, int udf250)
{
    sbuf t = { 0 }, label = { 0 };
    int space = 0;
    for (const char *s = text ? text : ""; *s;) {           /* drop, then collapse whitespace */
        const char *start = s;
        unsigned long c = utf8_next(&s);
        if (c > 0xFFFF || (udf250 && c == ',')) continue;
        if (c < 128 && isspace((int)c)) {
            space = t.len > 0;
            continue;
        }
        if (space) sb_puts(&t, " ");
        space = 0;
        sb_add(&t, start, (size_t)(s - start));
    }
    sb_puts(&label, disc_id);
    if (t.len) {
        sb_puts(&label, " ");
        sb_puts(&label, t.s);
    }
    free(t.s);
    if (udf250) {
        int wide = 0;
        for (const char *s = label.s; *s;) wide |= utf8_next(&s) > 0xFF;
        size_t max = wide ? 63 : 126, n = 0;
        const char *s = label.s;
        while (*s && n < max) { utf8_next(&s); n++; }
        label.len = (size_t)(s - label.s);
        label.s[label.len] = 0;
    } else {
        while (label.len > 32) {                            /* whole characters only */
            do label.len--; while (label.len && ((unsigned char)label.s[label.len] & 0xC0) == 0x80);
            label.s[label.len] = 0;
        }
    }
    if (strncmp(label.s, disc_id, strlen(disc_id))) {
        free(label.s);
        return xstrdup(disc_id);
    }
    while (label.len && label.s[label.len - 1] == ' ') label.s[--label.len] = 0;
    return label.s;
}
