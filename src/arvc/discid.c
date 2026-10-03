/* Disc ids (see discid.h). */
#include "discid.h"
#include "edtf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char ALPHABET[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

static int value(int c)
{
    const char *p;
    c = toupper(c);
    return c && (p = strchr(ALPHABET, c)) ? (int)(p - ALPHABET) : -1;
}

char discid_check(const char *payload)
{
    int total = 0, factor = 2;
    for (size_t i = strlen(payload); i-- > 0;) {
        int v = value((unsigned char)payload[i]);
        if (v < 0) continue;
        int addend = factor * v;
        total += addend / 36 + addend % 36;
        factor = factor == 2 ? 1 : 2;
    }
    return ALPHABET[(36 - total % 36) % 36];
}

static void trim(const char *s, const char **start, size_t *len)
{
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    *start = s;
    *len = n;
}

int discid_to_edtf(const char *coverage, char *out, size_t n)
{
    const char *c;
    size_t len;
    long a, b;
    trim(coverage ? coverage : "", &c, &len);
    if (len + 1 > n) return -1;
    memcpy(out, c, len);
    out[len] = 0;
    if (len == 9 && out[4] == '-' && atoi(out + 5) > 12) {
        int digits = 1;
        for (int i = 0; i < 9; i++) if (i != 4 && !isdigit((unsigned char)out[i])) digits = 0;
        if (digits) out[4] = '/';
    }
    return edtf_span(out, &a, &b) < 0 ? -1 : 0;
}

/* YYYY[MM] of one EDTF date (qualifiers dropped), as in ids */
static int one(const char *v, size_t len, char *out, size_t n)
{
    char s[16];
    size_t k = 0;
    while (len && isspace((unsigned char)*v)) { v++; len--; }
    while (len && isspace((unsigned char)v[len - 1])) len--;
    for (size_t i = 0; i < len; i++)
        if (!strchr("~?%", v[i])) {
            if (k + 1 >= sizeof s) return -1;
            s[k++] = v[i];
        }
    s[k] = 0;
    long a, b;
    if (k < 4 || edtf_date(s, k, &a, &b) < 0) return -1;
    size_t keep = k >= 7 ? 6 : 4;
    if (keep + 1 > n) return -1;
    memcpy(out, s, 4);
    if (keep == 6) memcpy(out + 4, s + 5, 2);
    out[keep] = 0;
    return 0;
}

int discid_compact(const char *coverage, char *out, size_t n)
{
    char c[64], x[16], y[16];
    long a, b;
    if (discid_to_edtf(coverage, c, sizeof c)) return -1;
    if (c[0] == '[' || c[0] == '{') {
        if (edtf_span(c, &a, &b) <= 0) return -1;
        return a / 10000 == b / 10000 ? (snprintf(out, n, "%ld", a / 10000) < (int)n ? 0 : -1)
                                       : (snprintf(out, n, "%ld-%ld", a / 10000, b / 10000) < (int)n ? 0 : -1);
    }
    char *slash = strchr(c, '/');
    if (slash) {
        const char *end = slash + 1;
        if (slash == c || !*end || !strncmp(c, "..", 2) || !strcmp(end, "..")) return -1;   /* open-ended */
        if (one(c, (size_t)(slash - c), x, sizeof x) || one(end, strlen(end), y, sizeof y)) return -1;
        return (strcmp(x, y) ? snprintf(out, n, "%s-%s", x, y) : snprintf(out, n, "%s", x)) < (int)n ? 0 : -1;
    }
    if (one(c, strlen(c), x, sizeof x)) return -1;
    return snprintf(out, n, "%s", x) < (int)n ? 0 : -1;
}

int discid_compose(const char *set, long sequence, const char *coverage, char *out, size_t n)
{
    char s[9], cov[32], body[64];
    size_t k = 0, all = 0;
    for (const char *p = set ? set : ""; *p; p++)
        if (isalnum((unsigned char)*p) && !((unsigned char)*p & 0x80)) {
            all++;
            if (k < 8) s[k++] = (char)toupper((unsigned char)*p);
        }
    s[k] = 0;
    if (all < 2 || discid_compact(coverage, cov, sizeof cov)) return -1;
    snprintf(body, sizeof body, "%s-%02ld_%s", s, sequence, cov);
    if (strlen(body) + 3 > DISCID_MAX || strlen(body) + 3 > n) return -1;
    sprintf(out, "%s_%c", body, discid_check(body));
    return 0;
}

static size_t span_of(const char *s, const char *chars)
{
    size_t n = 0;
    while (s[n] && strchr(chars, s[n])) n++;
    return n;
}

#define UPPER_DIGITS "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
#define DIGITS "0123456789"

int discid_parse(const char *text, discid_parts *p)
{
    const char *t;
    size_t len;
    char u[128];
    trim(text ? text : "", &t, &len);
    if (len >= sizeof u) return 0;
    for (size_t i = 0; i < len; i++) u[i] = (char)toupper((unsigned char)t[i]);
    u[len] = 0;
    memset(p, 0, sizeof *p);

    /* SET-SEQ_COVERAGE_CHECK: [A-Z0-9]{2,8}-\d{2,3}_[0-9X]{4,6}(-[0-9X]{4,6})?_[0-9A-Z] */
    size_t a = span_of(u, UPPER_DIGITS);
    if (a >= 2 && a <= 8 && u[a] == '-') {
        const char *q = u + a + 1;
        size_t b = span_of(q, DIGITS);
        if (b >= 2 && b <= 3 && q[b] == '_') {
            const char *r = q + b + 1;
            size_t c1 = span_of(r, DIGITS "X"), c = c1;
            int ok = c1 >= 4 && c1 <= 6;
            if (ok && r[c1] == '-') {
                size_t c2 = span_of(r + c1 + 1, DIGITS "X");
                ok = c2 >= 4 && c2 <= 6;
                c = c1 + 1 + c2;
            }
            if (ok && r[c] == '_' && r[c + 1] && strchr(UPPER_DIGITS, r[c + 1]) && !r[c + 2]) {
                p->scheme = DISCID_SCHEME;
                memcpy(p->set, u, a);
                p->sequence = strtol(q, NULL, 10);
                memcpy(p->coverage, r, c);
                p->check = r[c + 1];
                char body[128];
                memcpy(body, u, (size_t)(r + c - u));
                body[r + c - u] = 0;
                p->valid = discid_check(body) == p->check;
                return 1;
            }
        }
    }
    /* legacy COVERAGE_SET_SEQ: \d{4}(-\d{4})?_[A-Z0-9-]+_\d+ */
    size_t y = span_of(u, DIGITS);
    const char *s = u + 4;
    if (y == 4 && *s == '-' && span_of(s + 1, DIGITS) == 4) s += 5;
    if (y == 4 && *s == '_') {
        const char *set = s + 1, *last = strrchr(set, '_');
        if (last && last > set && span_of(set, UPPER_DIGITS "-") == (size_t)(last - set)
            && last[1] && span_of(last + 1, DIGITS) == strlen(last + 1)) {
            p->scheme = DISCID_LEGACY;
            memcpy(p->coverage, u, (size_t)(s - u));
            memcpy(p->set, set, (size_t)(last - set) < sizeof p->set ? (size_t)(last - set) : sizeof p->set - 1);
            p->sequence = strtol(last + 1, NULL, 10);
            p->valid = 1;
            return 1;
        }
    }
    return 0;
}
