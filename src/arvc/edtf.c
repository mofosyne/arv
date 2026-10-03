/* Coverage dates (see edtf.h). */
#include "edtf.h"

#include <ctype.h>
#include <string.h>

static int leap(long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int month_days(long y, int m)
{
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && leap(y) ? 29 : days[m - 1];
}

/* digits or X: [0-9X]{n}; returns the value with X as 0 (lo) and as 9 (hi) */
static int part(const char *s, int n, int allow_x, long *lo, long *hi)
{
    *lo = *hi = 0;
    for (int i = 0; i < n; i++) {
        if (isdigit((unsigned char)s[i])) {
            *lo = *lo * 10 + (s[i] - '0');
            *hi = *hi * 10 + (s[i] - '0');
        } else if (s[i] == 'X' && allow_x) {
            *lo *= 10;
            *hi = *hi * 10 + 9;
        } else {
            return 0;
        }
    }
    return 1;
}

/* One date: YYYY, YYYY-MM, YYYY-MM-DD, with X for unknown digits (19XX, 199X, 2019-XX) and the
 * qualifiers ~ ? % anywhere (they mark it approximate or uncertain; they do not widen it). */
static int date_span(const char *in, size_t len, long *first, long *last)
{
    char s[16];
    size_t n = 0;
    while (len && isspace((unsigned char)*in)) { in++; len--; }
    while (len && isspace((unsigned char)in[len - 1])) len--;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '~' || in[i] == '?' || in[i] == '%') continue;
        if (n + 1 >= sizeof s) return -1;
        s[n++] = in[i];
    }
    s[n] = 0;
    long y0, y1, m0 = 1, m1 = 12, d0 = 1, d1 = 0, t0, t1;
    if (n < 4 || !part(s, 4, 1, &y0, &y1)) return -1;
    /* the year: three digits then a digit or X, or two digits then XX */
    if (s[0] == 'X' || s[1] == 'X' || (s[2] == 'X' && s[3] != 'X')) return -1;
    if (n > 4) {
        if (s[4] != '-' || n < 7 || !part(s + 5, 2, 1, &t0, &t1)) return -1;
        if (strncmp(s + 5, "XX", 2)) {
            if (s[5] == 'X' || s[6] == 'X') return -1;
            m0 = m1 = t0;
        }
        if (m0 < 1 || m0 > 12) return -1;
        if (n > 7) {
            if (s[7] != '-' || n != 10 || !part(s + 8, 2, 1, &t0, &t1)) return -1;
            if (strncmp(s + 8, "XX", 2)) {
                if (s[8] == 'X' || s[9] == 'X') return -1;
                d0 = d1 = t0;
            }
        }
    }
    if (!d1) d1 = month_days(y1, (int)m1);
    if (d0 < 1 || d0 > month_days(y0, (int)m0) || d1 > month_days(y1, (int)m1)) return -1;
    *first = y0 * 10000 + m0 * 100 + d0;
    *last = y1 * 10000 + m1 * 100 + d1;
    return 1;
}

static int has_dotdot(const char *p, const char *stop)
{
    for (; p + 1 < stop; p++)
        if (p[0] == '.' && p[1] == '.') return 1;
    return 0;
}

int edtf_span(const char *c, long *first, long *last)
{
    size_t len;
    long a, b;
    while (c && isspace((unsigned char)*c)) c++;
    if (!c || !*c) return 0;
    len = strlen(c);
    while (len && isspace((unsigned char)c[len - 1])) len--;
    /* legacy year range: 2015-2024 */
    if (len == 9 && c[4] == '-' && isdigit((unsigned char)c[0]) && isdigit((unsigned char)c[5])) {
        long lo, hi, x;
        if (part(c, 4, 0, &lo, &x) && part(c + 5, 4, 0, &hi, &x) && hi > 12) {
            *first = lo * 10000 + 101;
            *last = hi * 10000 + 1231;
            return 1;
        }
    }
    if (c[0] == '[' || c[0] == '{') {                      /* one of / all of a set of dates */
        int any = 0;
        const char *p = c + 1, *end = c + len;
        while (end > p && (end[-1] == ']' || end[-1] == '}')) end--;
        while (p < end) {
            const char *comma = memchr(p, ',', (size_t)(end - p));
            const char *stop = comma ? comma : end;
            int blank = 1;
            for (const char *q = p; q < stop; q++) if (!isspace((unsigned char)*q)) blank = 0;
            if (!blank && !has_dotdot(p, stop)) {
                if (date_span(p, (size_t)(stop - p), &a, &b) < 0) return -1;
                if (!any || a < *first) *first = a;
                if (!any || b > *last) *last = b;
                any = 1;
            }
            p = comma ? comma + 1 : end;
        }
        return any;
    }
    const char *slash = memchr(c, '/', len);
    if (slash) {                                           /* an interval; .. or empty: open */
        size_t l1 = (size_t)(slash - c), l2 = len - l1 - 1;
        int open1 = l1 == 0 || (l1 == 2 && !strncmp(c, "..", 2));
        int open2 = l2 == 0 || (l2 == 2 && !strncmp(slash + 1, "..", 2));
        if (open1 && open2) return 0;
        *first = EDTF_MIN;
        *last = EDTF_MAX;
        if (!open1 && date_span(c, l1, first, &b) < 0) return -1;
        if (!open2 && date_span(slash + 1, l2, &a, last) < 0) return -1;
        return 1;
    }
    return date_span(c, len, first, last) < 0 ? -1 : 1;
}

int edtf_covers(const char *coverage, const char *query)
{
    long a, b, qa, qb;
    int r1 = edtf_span(coverage, &a, &b), r2 = edtf_span(query, &qa, &qb);
    if (r1 < 0 || r2 < 0) return -1;
    return r1 && r2 && a <= qb && qa <= b;
}
