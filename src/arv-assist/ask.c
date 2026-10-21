/*
 * arv describe --ask and --clues: describing without a model (research/plan.md, "Describing without
 * a model"). First the clues the files give by themselves: what kinds of files they are, the dates
 * in their names or their EXIF (exiftool, when it is there), a camera's names, the words of the
 * folders' names that the vocabulary knows (a tag, an alias, a value used before). Then, with
 * --ask, questions about what is still unknown, each offering the answers the clues and the
 * vocabulary make likely, as in twenty questions: the person picks a number, presses Enter for the
 * marked choice, or types. The title and description are made from the answers, to accept or edit.
 * Nothing here is a model's: --clues is automatic, --ask is the person's.
 */
#include "assist.h"

#include <ctype.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_QUESTIONS 20
#define EXIF_SAMPLE 60

typedef struct {
    size_t files;
    strlist kinds;              /* the kinds present, most first */
    size_t kind_n[16];
    const char *kind;           /* the kind of most of them (>= 60%), or NULL: a mix */
    long first, last;           /* dates as YYYYMMDD (00 for an unknown month or day); 0: none */
    const char *date_from;      /* "EXIF", "names" or "file times" */
    char *camera;               /* EXIF's most common Model, or NULL */
    size_t camera_names;        /* files named by a camera or phone (DSC_0001.JPG) */
    size_t pdfs_read;           /* PDFs read for their words, and those with none: scans (OCR them first) */
    strlist scans;
    size_t gps;                 /* photos with a position in their EXIF, and where most were (the median) */
    double lat, lon;
    strlist words;              /* the words of the name and the folders' names, the most telling first */
    strlist terms;              /* the same with each two in a row: what the counts of words and tags are by */
    strlist text;               /* the most telling words of its text files and PDFs */
    strlist titles;             /* its PDFs' own titles */
    strlist text_names;         /* words of its text written as names are (a capital mid-sentence): a person? a place? */
    strlist learned, learned_why;   /* the tags its words point to, and why (words.c) */
} clues;

/* ------------------------------------------------------------------ dates */

static int digits(const char *s, int n)
{
    for (int i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i])) return 0;
    return 1;
}

static int num(const char *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) v = v * 10 + (s[i] - '0');
    return v;
}

/* the first date in a name: 20190712, 2019-07-12, 2019_07, 2019 (a year alone only when it stands
 * alone); YYYYMMDD with 00 for what is not there, or 0 */
static long name_date(const char *s)
{
    for (const char *p = s; *p; p++) {
        if ((p > s && isdigit((unsigned char)p[-1])) || !digits(p, 4)) continue;
        int y = num(p, 4);
        if (y < 1900 || y > 2099) continue;
        const char *q = p + 4;
        int sep = *q == '-' || *q == '_' || *q == '.' || *q == ' ';
        if (sep) q++;
        if (digits(q, 2) && num(q, 2) >= 1 && num(q, 2) <= 12 && !(sep == 0 && !digits(q, 4))) {
            int m = num(q, 2);
            const char *r = q + 2;
            int sep2 = *r == '-' || *r == '_' || *r == '.';
            if ((sep && sep2) || !sep) r += sep2;
            if (digits(r, 2) && num(r, 2) >= 1 && num(r, 2) <= 31 && !isdigit((unsigned char)r[2]))
                return (long)y * 10000 + m * 100 + num(r, 2);
            if (!isdigit((unsigned char)q[2])) return (long)y * 10000 + m * 100;
            continue;
        }
        if (!isdigit((unsigned char)p[4])) return (long)y * 10000;
    }
    return 0;
}

static long time_date(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    return (long)(tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

/* "2019:07:12 18:30:00" (EXIF) -> 20190712 */
static long exif_date(const char *s)
{
    if (!s || !digits(s, 4) || s[4] != ':' || !digits(s + 5, 2) || s[7] != ':' || !digits(s + 8, 2)) return 0;
    long d = (long)num(s, 4) * 10000 + num(s + 5, 2) * 100 + num(s + 8, 2);
    return d / 10000 >= 1900 ? d : 0;
}

/* first..last as EDTF: 2019-07-12, 2019-07, 2019, 2019-07/2019-09, 2015/2019 */
static char *edtf_of(long a, long b)
{
    int ya = (int)(a / 10000), ma = (int)(a / 100 % 100), da = (int)(a % 100);
    int yb = (int)(b / 10000), mb = (int)(b / 100 % 100), db = (int)(b % 100);
    if (ya != yb) return xprintf("%d/%d", ya, yb);
    if (!ma || !mb) return xprintf("%d", ya);
    if (ma != mb) return xprintf("%d-%02d/%d-%02d", ya, ma, yb, mb);
    if (da && da == db) return xprintf("%d-%02d-%02d", ya, ma, da);
    return xprintf("%d-%02d", ya, ma);
}

static const char *const MONTHS[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                      "September", "October", "November", "December" };

/* an EDTF date for people: "July 2019", "12 July 2019", "2015 to 2019", "July to September 2019" */
static char *date_words(const char *edtf)
{
    int y, m, d, y2, m2, d2;
    if (sscanf(edtf, "%d-%d-%d/%d-%d-%d", &y, &m, &d, &y2, &m2, &d2) == 6 && m >= 1 && m <= 12 && m2 >= 1 && m2 <= 12) {
        if (y == y2 && m == m2) return xprintf("%d to %d %s %d", d, d2, MONTHS[m - 1], y);
        return y == y2 ? xprintf("%d %s to %d %s %d", d, MONTHS[m - 1], d2, MONTHS[m2 - 1], y)
                       : xprintf("%d %s %d to %d %s %d", d, MONTHS[m - 1], y, d2, MONTHS[m2 - 1], y2);
    }
    if (strchr(edtf, '/') && !(sscanf(edtf, "%d-%d/%d-%d", &y, &m, &y2, &m2) == 4) && !(sscanf(edtf, "%d/%d", &y, &y2) == 2))
        return xstrdup(edtf);
    if (sscanf(edtf, "%d-%d/%d-%d", &y, &m, &y2, &m2) == 4 && m >= 1 && m <= 12 && m2 >= 1 && m2 <= 12)
        return y == y2 ? xprintf("%s to %s %d", MONTHS[m - 1], MONTHS[m2 - 1], y) : xprintf("%s %d to %s %d", MONTHS[m - 1], y,
                                                                                            MONTHS[m2 - 1], y2);
    if (sscanf(edtf, "%d/%d", &y, &y2) == 2 && !strchr(edtf, '-')) return xprintf("%d to %d", y, y2);
    if (strchr(edtf, '/')) return xstrdup(edtf);
    int n = sscanf(edtf, "%d-%d-%d", &y, &m, &d);
    if (n == 3 && m >= 1 && m <= 12) return xprintf("%d %s %d", d, MONTHS[m - 1], y);
    if (n == 2 && m >= 1 && m <= 12) return xprintf("%s %d", MONTHS[m - 1], y);
    return xstrdup(edtf);
}

/* ------------------------------------------------------------------ words */

static const char *const STOP[] = { "the", "and", "of", "a", "an", "to", "in", "at", "on", "for", "with", "new", "old",
                                    "copy", "misc", "other", "files", "file", "folder", "data", "backup", "final", "img",
                                    "dsc", "dcim", "pxl", "vid", "mvi", "photo", "photos", "images", "image", "jpg", "jpeg",
                                    "export", "untitled", "tmp", "temp", "day", "days", "part", "vol", "disc", "set", "page",
                                    "scan", "scans", "pics", "pictures", "video", "videos", "clip", "clips", "raw", "edit",
                                    "edited", "best", "all", "more", "from", "jpeg", "png", NULL };

/* the words of a name, lower case: runs of letters (digits, dates and separators split them) */
static void name_words(const char *name, strlist *out)
{
    sbuf w = { 0 };
    char *folded = text_fold(name);
    for (const char *p = folded;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c && (isalpha(c) || c >= 0x80)) {
            char l = (char)tolower(c);
            sb_add(&w, &l, 1);
            continue;
        }
        if (w.len >= 3) {
            int stop = 0;
            for (int i = 0; STOP[i] && !stop; i++) stop = !strcmp(w.s, STOP[i]);
            if (!stop) strlist_add(out, w.s);
        }
        w.len = 0;
        if (w.s) w.s[0] = 0;
        if (!c) break;
    }
    free(folded);
    free(w.s);
}

/* ------------------------------------------------------------------ clues */

static int by_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void exif_clues(const items *l, const char *root, clues *c, strlist *dates)
{
    if (!root || !on_path("exiftool")) return;
    strlist files = { 0 };
    size_t media = 0;
    for (size_t i = 0; i < l->n; i++) {
        const char *dot = strrchr(l->v[i].path, '.');
        char ext[16] = "";
        for (size_t k = 0; dot && dot[k] && k < sizeof ext - 1; k++) ext[k] = (char)tolower((unsigned char)dot[k]), ext[k + 1] = 0;
        const char *k = kind_of(ext);
        if (strcmp(k, "photos") && strcmp(k, "videos")) continue;
        media++;
    }
    size_t step = media > EXIF_SAMPLE ? media / EXIF_SAMPLE : 1, at = 0;   /* a sample, spread over them */
    for (size_t i = 0; i < l->n && files.n < EXIF_SAMPLE; i++) {
        const char *dot = strrchr(l->v[i].path, '.');
        char ext[16] = "";
        for (size_t k = 0; dot && dot[k] && k < sizeof ext - 1; k++) ext[k] = (char)tolower((unsigned char)dot[k]), ext[k + 1] = 0;
        const char *k = kind_of(ext);
        if (strcmp(k, "photos") && strcmp(k, "videos")) continue;
        if (at++ % step) continue;
        char *p = join(root, l->v[i].path);
        strlist_add(&files, p);
        free(p);
    }
    if (!files.n) return;
    char **argv = xmalloc((files.n + 12) * sizeof *argv);
    int n = 0;
    argv[n++] = "exiftool";
    argv[n++] = "-j";
    argv[n++] = "-q";
    argv[n++] = "-DateTimeOriginal";
    argv[n++] = "-CreateDate";
    argv[n++] = "-Model";
    argv[n++] = "-n";                   /* positions as signed decimal degrees */
    argv[n++] = "-GPSLatitude";
    argv[n++] = "-GPSLongitude";
    for (size_t i = 0; i < files.n; i++) argv[n++] = files.v[i];
    argv[n] = NULL;
    char *out = NULL;
    run(argv, &out);                    /* a file it cannot read is left out; the rest still come */
    jv *doc = out ? json_parse(strchr(out, '[') ? strchr(out, '[') : out) : NULL;
    strlist models = { 0 };
    double *lats = xmalloc((files.n + 1) * sizeof *lats), *lons = xmalloc((files.n + 1) * sizeof *lons);
    for (size_t i = 0; doc && doc->kind == 'a' && i < doc->n; i++) {
        const jv *o = doc->vals[i];
        const jv *la = json_get(o, "GPSLatitude"), *lo = json_get(o, "GPSLongitude");
        if (la && lo && la->kind == 't' && lo->kind == 't') {
            double y = atof(la->str), x = atof(lo->str);
            if ((y || x) && y >= -90 && y <= 90 && x >= -180 && x <= 180 && c->gps < files.n) lats[c->gps] = y, lons[c->gps++] = x;
        }
        const char *t = jstr_of(json_get(o, "DateTimeOriginal")), *t2 = jstr_of(json_get(o, "CreateDate"));
        long d = exif_date(t ? t : t2);
        if (d) {
            char ds[16];
            snprintf(ds, sizeof ds, "%ld", d);
            strlist_add(dates, ds);
        }
        const char *m = jstr_of(json_get(o, "Model"));   /* the document's own strings: not freed here */
        if (m && *m) strlist_add(&models, m);
    }
    size_t best = 0;
    for (size_t i = 0; i < models.n; i++) {
        size_t k = 0;
        for (size_t j = 0; j < models.n; j++) k += !strcmp(models.v[i], models.v[j]);
        if (k > best) {
            best = k;
            free(c->camera);
            c->camera = xstrdup(models.v[i]);
        }
    }
    if (c->gps) {                       /* the median: a stray photo from elsewhere does not move it */
        qsort(lats, c->gps, sizeof *lats, by_double);
        qsort(lons, c->gps, sizeof *lons, by_double);
        c->lat = lats[c->gps / 2];
        c->lon = lons[c->gps / 2];
    }
    free(lats);
    free(lons);
    if (doc) jfree(doc);
    strlist_free(&models);
    strlist_free(&files);
    free(argv);
    free(out);
}

static int by_count_desc(const void *a, const void *b)
{
    const long *x = a, *y = b;
    return x[1] != y[1] ? (x[1] > y[1] ? -1 : 1) : (x[0] < y[0] ? -1 : x[0] > y[0]);
}

static void gather(const word_stats *ws, const items *l, const char *name, const char *root, clues *c)
{
    memset(c, 0, sizeof *c);
    c->files = l->n;
    strlist kinds = { 0 }, name_dates = { 0 }, exif = { 0 }, all_words = { 0 };
    long t_first = 0, t_last = 0;
    char *last_folder = NULL;
    for (size_t i = 0; i < l->n; i++) {
        const char *path = l->v[i].path, *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        const char *dot = strrchr(base, '.');
        char ext[16] = "";
        for (size_t k = 0; dot && dot[k] && k < sizeof ext - 1; k++) ext[k] = (char)tolower((unsigned char)dot[k]), ext[k + 1] = 0;
        const char *k = kind_of(ext);
        if (!strlist_has(&kinds, k)) strlist_add(&kinds, k);
        for (size_t j = 0; j < kinds.n; j++)
            if (!strcmp(kinds.v[j], k) && j < 16) c->kind_n[j]++;
        c->camera_names += generic_name(base);
        long d = name_date(path);
        if (d) {
            char ds[16];
            snprintf(ds, sizeof ds, "%ld", d);
            strlist_add(&name_dates, ds);
        }
        long t = time_date(l->v[i].mtime);
        if (!t_first || t < t_first) t_first = t;
        if (t > t_last) t_last = t;
        char *folder = xstrdup(path), *slash = strrchr(folder, '/');      /* the folders' names, not the files' */
        if (slash) {
            *slash = 0;
            if (!last_folder || strcmp(last_folder, folder)) {   /* each folder once (a listing is by path) */
                name_words(folder, &all_words);
                name_word_list(folder, &c->terms);
                free(last_folder);
                last_folder = xstrdup(folder);
            }
        }
        free(folder);
    }
    free(last_folder);
    name_words(name, &all_words);
    name_words(name, &all_words);           /* the name itself counts more than one folder */
    name_word_list(name, &c->terms);
    {                                       /* the kinds, most first */
        long (*order)[2] = xmalloc((kinds.n + 1) * sizeof *order);
        for (size_t j = 0; j < kinds.n && j < 16; j++) order[j][0] = (long)j, order[j][1] = (long)c->kind_n[j];
        size_t nk = kinds.n < 16 ? kinds.n : 16;
        qsort(order, nk, sizeof *order, by_count_desc);
        size_t counts[16];
        for (size_t j = 0; j < nk; j++) {
            strlist_add(&c->kinds, kinds.v[order[j][0]]);
            counts[j] = (size_t)order[j][1];
        }
        for (size_t j = 0; j < nk; j++) c->kind_n[j] = counts[j];
        if (nk && c->kind_n[0] * 10 >= l->n * 6 && strcmp(c->kinds.v[0], "files")) c->kind = c->kinds.v[0];
        free(order);
    }
    exif_clues(l, root, c, &exif);
    const strlist *dates = exif.n ? &exif : name_dates.n * 2 >= l->n && name_dates.n ? &name_dates : NULL;
    c->date_from = exif.n ? "EXIF" : dates ? "names" : l->n ? "file times" : NULL;
    if (dates)
        for (size_t i = 0; i < dates->n; i++) {
            long d = atol(dates->v[i]);
            if (!c->first || d < c->first) c->first = d;
            if (d > c->last) c->last = d;
        }
    else if (l->n) {
        c->first = t_first;
        c->last = t_last;
    }
    {                                       /* the words, most used first */
        long (*order)[2] = xmalloc((all_words.n + 1) * sizeof *order);
        strlist uniq = { 0 };
        size_t nu = 0;
        for (size_t i = 0; i < all_words.n; i++) {
            if (strlist_has(&uniq, all_words.v[i])) continue;
            long k = 0;
            for (size_t j = 0; j < all_words.n; j++) k += !strcmp(all_words.v[i], all_words.v[j]);
            strlist_add(&uniq, all_words.v[i]);
            order[nu][0] = (long)nu;
            order[nu][1] = k;
            nu++;
        }
        if (ws)                             /* the most telling: rare in the archive's names, then used here */
            for (size_t i = 0; i < nu; i++)
                order[i][1] = (long)(word_rarity(ws, uniq.v[order[i][0]]) * 100) * 1000 + (order[i][1] < 999 ? order[i][1] : 999);
        qsort(order, nu, sizeof *order, by_count_desc);
        for (size_t i = 0; i < nu && c->words.n < 12; i++)
            if (!ws || !word_common(ws, uniq.v[order[i][0]])) strlist_add(&c->words, uniq.v[order[i][0]]);
        strlist_free(&uniq);
        free(order);
    }
    {                                       /* the terms once each, rare first */
        strlist u = { 0 };
        for (size_t i = 0; i < c->terms.n; i++)
            if (!strlist_has(&u, c->terms.v[i]) && (!ws || !word_common(ws, c->terms.v[i]))) strlist_add(&u, c->terms.v[i]);
        for (size_t i = 1; ws && i < u.n; i++)
            for (size_t j = i; j > 0 && word_rarity(ws, u.v[j]) > word_rarity(ws, u.v[j - 1]); j--) {
                char *t = u.v[j];
                u.v[j] = u.v[j - 1];
                u.v[j - 1] = t;
            }
        while (u.n > 60) free(u.v[--u.n]);
        strlist_free(&c->terms);
        c->terms = u;
    }
    text_clues(ws, root, l, &c->text, &c->titles, &c->text_names, &c->pdfs_read, &c->scans);
    if (ws) word_tags(ws, &c->terms, &c->text, &c->learned, &c->learned_why);
    strlist_free(&kinds);
    strlist_free(&name_dates);
    strlist_free(&exif);
    strlist_free(&all_words);
}

static void clues_free(clues *c)
{
    strlist_free(&c->kinds);
    strlist_free(&c->words);
    strlist_free(&c->terms);
    strlist_free(&c->text);
    strlist_free(&c->titles);
    strlist_free(&c->text_names);
    strlist_free(&c->scans);
    strlist_free(&c->learned);
    strlist_free(&c->learned_why);
    free(c->camera);
}

/* ------------------------------------------------------------------ the vocabulary, and what was used */

/* the values each namespace has had in this archive (discs' tags and objects' Tags), most used first */
static void used_values(const arv_home *h, const archive *cat, const char *ns, strlist *out)
{
    strlist all = { 0 };
    size_t n = strlen(ns);
    for (size_t i = 0; cat && h && i < cat->discs.n; i++) {
        const char *id = rec_get(cat->discs.v[i], "Id");
        if (!id) continue;
        char *tf = home_volume_file(h, id, "tags.tsv"), *text = disc_tags_text(cat, id, access(tf, F_OK) ? NULL : tf);
        for (char *line = text, *next; line && *line; line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            char *tab = line[0] != '#' ? strchr(line, '\t') : NULL;
            if (!tab) continue;
            char *end = strchr(tab + 1, '\t');
            if (end) *end = 0;
            for (char *t = strtok(tab + 1, ","); t; t = strtok(NULL, ",")) {
                while (*t == ' ') t++;
                if (!strncmp(t, ns, n) && t[n] == ':' && t[n + 1]) strlist_add(&all, t + n + 1);
            }
        }
        free(text);
        free(tf);
    }
    for (size_t i = 0; cat && i < cat->objects.n; i++) {
        const char *u = rec_get(cat->objects.v[i], "Uuid"), *set = u ? object_field(cat, u, "Tags") : NULL;
        if (!set) continue;
        char *copy = xstrdup(set);
        for (char *t = strtok(copy, ","); t; t = strtok(NULL, ",")) {
            while (*t == ' ') t++;
            if (!strncmp(t, ns, n) && t[n] == ':' && t[n + 1]) strlist_add(&all, t + n + 1);
        }
        free(copy);
    }
    while (all.n) {                         /* the most used, one at a time */
        size_t best = 0, bk = 0;
        for (size_t i = 0; i < all.n; i++) {
            size_t k = 0;
            for (size_t j = 0; j < all.n; j++) k += !strcmp(all.v[i], all.v[j]);
            if (k > bk) bk = k, best = i;
        }
        char *v = xstrdup(all.v[best]);
        strlist_add(out, v);
        strlist rest = { 0 };
        for (size_t i = 0; i < all.n; i++)
            if (strcmp(all.v[i], v)) strlist_add(&rest, all.v[i]);
        strlist_free(&all);
        all = rest;
        free(v);
        if (out->n >= 8) break;
    }
    strlist_free(&all);
}

/* a value as a tag's value: lower case, words joined by '-' ("Kyoto Trip" -> "kyoto-trip") */
static char *slug(const char *text)
{
    sbuf b = { 0 };
    int gap = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (isalnum(*p) || *p >= 0x80) {
            if (gap && b.len) sb_puts(&b, "-");
            char c = (char)tolower(*p);
            sb_add(&b, &c, 1);
            gap = 0;
        } else {
            gap = 1;
        }
    }
    return b.s ? b.s : xstrdup("");
}

/* a name for a title, without its dates: "2019-07 Kyoto trip" -> "Kyoto trip" */
static char *name_title(const char *name)
{
    const char *s = name, *e = name + strlen(name);
    while (*s && (isdigit((unsigned char)*s) || strchr(" -_.", *s))) s++;
    while (e > s && (isdigit((unsigned char)e[-1]) || strchr(" -_.", e[-1]))) e--;
    if (s == e) s = name, e = name + strlen(name);
    char *t = xprintf("%.*s", (int)(e - s), s);
    for (char *p = t; *p; p++)
        if (*p == '_') *p = ' ';
    if (*t) t[0] = (char)toupper((unsigned char)t[0]);
    return t;
}

/* "3 photos", "1 photo": the kinds are plural */
static char *counted(size_t n, const char *kind)
{
    size_t k = strlen(kind);
    return n == 1 && k > 1 && kind[k - 1] == 's' ? xprintf("1 %.*s", (int)(k - 1), kind) : xprintf("%zu %s", n, kind);
}

static char *pretty(const char *value)      /* "kyoto-trip" -> "Kyoto trip" */
{
    char *s = xstrdup(value);
    for (char *p = s; *p; p++)
        if (*p == '-' || *p == '_') *p = ' ';
    if (*s) s[0] = (char)toupper((unsigned char)s[0]);
    return s;
}

/* ------------------------------------------------------------------ asking */

typedef struct {
    int interactive, asked, stopped;
    const known_entry *kv;      /* the known names (known.c) */
    size_t kn;
    const word_stats *ws;
} asking;

/* answers given ahead (the GUI: describe --answers), replayed in order; when they run out, the next
 * question is kept (pending) and the rest goes as if '.' stopped the asking. dry: nothing is kept
 * (no examples, rules or entries written) until a run with every answer given. */
static const strlist *script;
static size_t script_pos;
static jv *pending;
static int dry;

void ask_replay(const strlist *answers, int dry_run)
{
    script = answers;
    script_pos = 0;
    dry = dry_run;
    if (pending) jfree(pending);
    pending = NULL;
}

jv *ask_pending(void)
{
    jv *p = pending;
    pending = NULL;
    return p;
}

/* an answer: from the terminal, or the next one given ahead; q describes the question (freed here) */
static char *reply(asking *a, const char *prompt, jv *q)
{
    if (!script) {
        jfree(q);
        return ask(prompt);
    }
    if (script_pos < script->n) {
        jfree(q);
        return xstrdup(script->v[script_pos++]);
    }
    if (!pending) pending = q;
    else jfree(q);
    a->stopped = 1;
    return xstrdup(".");
}

static jv *qjson(const char *kind, const char *text)
{
    jv *q = jobj();
    jput(q, "kind", jstr(kind));
    jput(q, "text", jstr(text));
    return q;
}

/* one question: numbered choices (marked: the default), and an answer: Enter takes the marked ones,
 * numbers pick, '-' none, '.' stops asking (the rest as the clues have it), anything else is the
 * answer itself (commas between several, when many). The answer goes into out. */
static void choose(asking *a, const char *heading, const strlist *opts, const strlist *why, const strlist *marked, int many,
                   strlist *out)
{
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < marked->n; i++) strlist_add(out, marked->v[i]);
    if (!a->interactive || a->stopped || a->asked >= MAX_QUESTIONS) return;
    a->asked++;
    fprintf(stderr, "\n%s\n", heading);
    size_t col = 0;                         /* those with a reason one a line; the rest four a line */
    for (size_t i = 0; i < opts->n; i++) {
        int said = why && i < why->n && *why->v[i];
        if (said || opts->n <= 8) {
            if (col) fputc('\n', stderr), col = 0;
            fprintf(stderr, "  %c%zu %s%s%s%s\n", strlist_has(marked, opts->v[i]) ? '*' : ' ', i + 1, opts->v[i],
                    said ? "  (" : "", said ? why->v[i] : "", said ? ")" : "");
        } else {
            char cell[40];
            snprintf(cell, sizeof cell, "%c%zu %s", strlist_has(marked, opts->v[i]) ? '*' : ' ', i + 1, opts->v[i]);
            fprintf(stderr, "%s%-18s", col ? "" : "  ", cell);
            if (++col == 4) fputc('\n', stderr), col = 0;
        }
    }
    if (col) fputc('\n', stderr);
    jv *q = qjson("choose", heading), *list = jarr();
    jput(q, "many", jnum(many ? "true" : "false"));
    for (size_t i = 0; i < opts->n; i++) {
        jv *o = jobj();
        jput(o, "value", jstr(opts->v[i]));
        jput(o, "why", jstr(why && i < why->n ? why->v[i] : ""));
        jput(o, "marked", jnum(strlist_has(marked, opts->v[i]) ? "true" : "false"));
        jpush(list, o);
    }
    jput(q, "options", list);
    char *ans = reply(a, many ? "Enter: the marked ones; numbers (1,3), '-' none, '.' stop asking, or type them, commas between (1, x)\n> "
                              : "Enter: the marked one; a number, '-' none, '.' stop asking, or type it\n> ", q);
    if (!strcmp(ans, ".")) {
        a->stopped = 1;
    } else if (!strcmp(ans, "-")) {
        strlist_free(out);
    } else if (*ans) {
        strlist_free(out);
        int all_numbers = 1;
        for (const char *p = ans; *p && all_numbers; p++) all_numbers = isdigit((unsigned char)*p) || *p == ',' || *p == ' ';
        char *copy = xstrdup(ans);
        for (char *t = strtok(copy, all_numbers ? ", " : ","); t; t = strtok(NULL, all_numbers ? ", " : ",")) {
            while (*t == ' ') t++;
            char *e = t + strlen(t);
            while (e > t && e[-1] == ' ') *--e = 0;
            if (!*t) continue;
            int number = 1;                 /* "1, fushimi-inari": an option by its number, and one typed */
            for (const char *d = t; *d && number; d++) number = isdigit((unsigned char)*d);
            if (all_numbers || number) {
                long k = atol(t);
                if (k >= 1 && (size_t)k <= opts->n && !strlist_has(out, opts->v[k - 1])) strlist_add(out, opts->v[k - 1]);
            } else if (!strlist_has(out, t)) {
                strlist_add(out, t);
            }
            if (!many && out->n) break;
        }
        free(copy);
    }
    free(ans);
}

/* a field's value: Enter keeps the one made from the answers, or type another */
static char *field(asking *a, const char *label, const char *made)
{
    if (!a->interactive || a->stopped) return xstrdup(made);
    fprintf(stderr, "\n%s: %s\n", label, made);
    jv *q = qjson("field", label);
    jput(q, "value", jstr(made));
    char *ans = reply(a, "Enter: as it is; or type another ('.' keeps it and stops asking)\n> ", q);
    if (!strcmp(ans, ".")) {
        a->stopped = 1;
        free(ans);
        return xstrdup(made);
    }
    char *out = *ans ? ans : xstrdup(made);
    if (out != ans) free(ans);
    return out;
}

static const char *ns_desc(const tag_words_t *tw, const char *ns)
{
    for (size_t i = 0; i < tw->ns_names.n; i++)
        if (!strcmp(tw->ns_names.v[i], ns)) return tw->ns_desc.v[i];
    return "";
}

/* the namespaces worth asking about, for a kind of content */
static int asks(const char *kind, const char *ns)
{
    int media = kind && (!strcmp(kind, "photos") || !strcmp(kind, "videos") || !strcmp(kind, "audio recordings"));
    int work = kind && (!strcmp(kind, "source code files") || !strcmp(kind, "text documents") || !strcmp(kind, "PDF documents")
                        || !strcmp(kind, "spreadsheets"));
    if (!strcmp(ns, "date")) return 0;     /* asked on its own */
    if (!strcmp(ns, "project") || !strcmp(ns, "software") || !strcmp(ns, "organisation")) return !media;
    if (!strcmp(ns, "family")) return !work;
    if (!strcmp(ns, "event") || !strcmp(ns, "source")) return !work;
    return 1;
}

/* a form as the clues hold words: lower case, words one space apart ("Anne-Smith" -> "anne smith") */
static char *form_of(const char *s)
{
    sbuf b = { 0 };
    int gap = 0;
    char *folded = text_fold(s);            /* "Zürich" and "Zurich", composed or not: one form */
    for (const unsigned char *p = (const unsigned char *)folded; *p; p++) {
        if (isalnum(*p) || *p >= 0x80) {
            if (gap && b.len) sb_puts(&b, " ");
            char ch = (char)tolower(*p);
            sb_add(&b, &ch, 1);
            gap = 0;
        } else {
            gap = 1;
        }
    }
    free(folded);
    return b.s ? b.s : xstrdup("");
}

/* where a form is among a folder's clues: 1 in its names, 2 a name in its text, 3 a word of its text; 0 not */
static int form_where(const clues *c, const char *form)
{
    if (strlist_has(&c->terms, form)) return 1;
    if (strlist_has(&c->text_names, form)) return 2;
    if (strlist_has(&c->text, form)) return 3;
    return 0;
}

/* the forms an entry is found as: its key's value, its label, its aliases */
static void entry_forms(const known_entry *e, strlist *out)
{
    memset(out, 0, sizeof *out);
    const char *v = strchr(e->key, ':') ? strchr(e->key, ':') + 1 : e->key;
    char *f = form_of(v);
    strlist_add(out, f);
    free(f);
    if (e->label) {
        f = form_of(e->label);
        if (!strlist_has(out, f)) strlist_add(out, f);
        free(f);
    }
    for (size_t i = 0; i < e->aliases.n; i++) {
        if (strchr(e->aliases.v[i], ':')) continue;   /* a merged key, not a form */
        f = form_of(e->aliases.v[i]);
        if (*f && !strlist_has(out, f)) strlist_add(out, f);
        free(f);
    }
}

/* how many entries (not retired) a form names */
/* the country (or the broadest place) a place is in */
static const char *top_of(const asking *a, const known_entry *e)
{
    const char *key = e->key;
    for (int depth = 0; e && e->parent && depth < 16; depth++) {
        key = e->parent;
        e = known_find(a->kv, a->kn, e->parent);
    }
    return key;
}

/* o holds e: o is one of the places e is in, or a region of e's country with e's name (Osaka the
 * prefecture and Osaka the city: the gazetteer's cities are in their countries, not their regions) */
static int holds(const asking *a, const known_entry *o, const known_entry *e)
{
    if (known_within(a->kv, a->kn, e->key, o->key)) return 1;
    return o->kind && e->kind && !strcmp(o->kind, "region") && !strcmp(e->kind, "city") && !strcmp(top_of(a, o), top_of(a, e));
}

static size_t entries_named(const asking *a, const char *form, const known_entry *e, int *outer)
{
    size_t n = 0;
    *outer = 0;
    for (size_t i = 0; i < a->kn; i++) {
        const known_entry *o = &a->kv[i];
        if (o->retired) continue;
        strlist f;
        entry_forms(o, &f);
        int named = strlist_has(&f, form);
        strlist_free(&f);
        if (!named) continue;
        if (o != e && holds(a, e, o)) {
            *outer = 1;                     /* Kyoto Prefecture, which Kyoto the city is in: the city is offered */
            continue;
        }
        if (o != e && holds(a, o, e)) continue;   /* the other way: not two */
        n++;
    }
    return n;
}

/* the known names of a namespace among a folder's clues, as choices: taken without asking (certain)
 * when the form names this entry alone, is in the names or written as a name in the text, the entry
 * was used twice or more, and a word of this folder went with it before; else offered */
static void known_choices(const asking *a, const clues *c, const char *ns, int one, strlist *opts, strlist *why,
                          strlist *marked, strlist *certain)
{
    size_t nl = strlen(ns), places = 0;
    for (size_t i = 0; i < a->kn; i++) {
        const known_entry *e = &a->kv[i];
        if (e->retired || strncmp(e->key, ns, nl) || e->key[nl] != ':') continue;
        strlist forms;
        entry_forms(e, &forms);
        int where = 0, two = 0;
        const char *found = NULL;
        for (size_t f = 0; f < forms.n && !where; f++)
            if ((where = form_where(c, forms.v[f]))) found = forms.v[f];
        if (!where) {
            strlist_free(&forms);
            continue;
        }
        int outer;
        two = entries_named(a, found, e, &outer) > 1;
        if (outer) {
            strlist_free(&forms);
            continue;
        }
        size_t uses = word_tag_uses(a->ws, e->key), with = 0, all = 0;
        int gazetteer = e->kind && (!strcmp(e->kind, "city") || !strcmp(e->kind, "country") || !strcmp(e->kind, "region"));
        if (gazetteer && !uses && ++places > 8) {
            strlist_free(&forms);           /* a text naming many cities: the first few, by where they are found */
            continue;
        }
        int context = 0;
        const strlist *lists[2] = { &c->terms, &c->text };
        for (int k = 0; k < 2 && !context; k++)
            for (size_t w = 0; w < lists[k]->n && !context; w++) {
                if (strstr(found, lists[k]->v[w]) || strstr(lists[k]->v[w], found)) continue;   /* its own words */
                word_counts(a->ws, lists[k]->v[w], e->key, &with, &all);
                context = with >= 2;
            }
        int sure = !two && uses >= 2 && context && where <= 2;
        const char *value = e->key + nl + 1;
        if (!strlist_has(opts, value)) {
            char *place = e->parent ? known_where(a->kv, a->kn, e) : NULL;   /* which Victoria: "Victoria, Canada" */
            const char *shown = place ? place : e->label ? e->label : value;
            char *y = sure ? xprintf("known: %s, in %zu folders before", shown, uses)
                      : two ? xprintf("known: %s%s%s%s, one of two entries of this name", shown,
                                      e->qualifier ? " (" : "", e->qualifier ? e->qualifier : "", e->qualifier ? ")" : "")
                            : xprintf("known: %s%s%s%s", shown, e->qualifier ? " (" : "",
                                      e->qualifier ? e->qualifier : "", e->qualifier ? ")" : "");
            strlist_add(opts, value);
            strlist_add(why, y);
            free(y);
            if (sure) strlist_add(certain, value);
            if (sure || (!two && a->interactive && (!gazetteer || uses || where == 1))) {   /* a city its text mentions: offered */
                if (!marked->n || !one) strlist_add(marked, value);
            }
            free(place);
        }
        strlist_free(&forms);
    }
}

static double km_between(double lat1, double lon1, double lat2, double lon2)
{
    const double r = 6371.0, d = 3.14159265358979323846 / 180;
    double dl = (lat2 - lat1) * d, dn = (lon2 - lon1) * d;
    double h = sin(dl / 2) * sin(dl / 2) + cos(lat1 * d) * cos(lat2 * d) * sin(dn / 2) * sin(dn / 2);
    return 2 * r * atan2(sqrt(h), sqrt(1 - h));
}

/* ------------------------------------------------------------------ where the photos' GPS is */

extern const char COUNTRIES[];              /* data/countries.txt, built in (dev-tools/places.py) */

typedef struct {
    char key[64];
    int *pt;                                /* lon,lat pairs, tenths of a degree */
    size_t n;
    double area;
} ring;

static ring *rings;
static size_t nrings;

static void rings_load(void)
{
    static int done;
    if (done) return;
    done = 1;
    for (const char *s = COUNTRIES, *e; *s; s = *e ? e + 1 : e) {
        e = strchr(s, '\n');
        if (!e) e = s + strlen(s);
        const char *tab = memchr(s, '\t', (size_t)(e - s));
        if (*s == '#' || !tab || tab - s >= 64) continue;
        rings = xrealloc(rings, (nrings + 1) * sizeof *rings);
        ring *r = &rings[nrings++];
        memset(r, 0, sizeof *r);
        memcpy(r->key, s, (size_t)(tab - s));
        for (const char *p = tab + 1; p < e;) {
            char *q;
            long x = strtol(p, &q, 10);
            if (q == p || *q != ',') break;
            long y = strtol(q + 1, &q, 10);
            r->pt = xrealloc(r->pt, (r->n + 1) * 2 * sizeof *r->pt);
            r->pt[2 * r->n] = (int)x, r->pt[2 * r->n + 1] = (int)y;
            r->n++;
            p = q;
            while (p < e && *p == ' ') p++;
        }
        for (size_t i = 0, j = r->n - 1; i < r->n; j = i++)   /* the shoelace: an enclave's ring is the smaller */
            r->area += (double)r->pt[2 * j] * r->pt[2 * i + 1] - (double)r->pt[2 * i] * r->pt[2 * j + 1];
        r->area = r->area < 0 ? -r->area / 2 : r->area / 2;
    }
}

/* the country whose outline holds a point (the smallest ring that does: Lesotho, not South Africa), or NULL */
static const char *country_at(double lat, double lon)
{
    rings_load();
    double x = lon * 10, y = lat * 10;
    const ring *best = NULL;
    for (size_t k = 0; k < nrings; k++) {
        const ring *r = &rings[k];
        int in = 0;                         /* a ray to the east: crossing an odd number of edges is inside */
        for (size_t i = 0, j = r->n - 1; i < r->n; j = i++) {
            double xi = r->pt[2 * i], yi = r->pt[2 * i + 1], xj = r->pt[2 * j], yj = r->pt[2 * j + 1];
            if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
        }
        if (in && (!best || r->area < best->area)) best = r;
    }
    return best ? best->key : NULL;
}

/* whether a country has an outline at this scale (Singapore, Malta, Monaco have none: trust their cities) */
static int outlined(const char *key)
{
    rings_load();
    for (size_t k = 0; key && k < nrings; k++)
        if (!strcmp(rings[k].key, key)) return 1;
    return 0;
}

enum { GPS_NEAR, GPS_INSIDE, GPS_OUTLINE, GPS_CITY_COUNTRY };

/* the place the photos' GPS is in: a place of your own whose Extent holds it or within its Reach
 * (3 km unless it says), else a city of the country it is in within 40 km, else that country (its
 * outline), else, far from any outline (a small island), the country of a city within 400 km.
 * *how says which; *km how far from the place's point. NULL: none. */
static const known_entry *gps_place(const asking *a, const clues *c, int *how, double *km)
{
    const known_entry *site = NULL, *city = NULL, *near = NULL;
    double sd = 0, cd = 0, nd = 0, box = 0;
    int inside = 0;
    *how = GPS_NEAR;
    *km = 0;
    if (!c->gps) return NULL;
    const char *country = country_at(c->lat, c->lon);
    for (size_t i = 0; i < a->kn; i++) {
        const known_entry *e = &a->kv[i];
        if (!e->has_at || e->retired || strncmp(e->key, "place:", 6) || (e->kind && !strcmp(e->kind, "country"))) continue;
        double d = km_between(c->lat, c->lon, e->lat, e->lon);
        if (!e->kind || strcmp(e->kind, "city")) {      /* your own: in its box (the smallest), or the nearest in reach */
            int in = e->has_extent && c->lat >= e->south && c->lat <= e->north && c->lon >= e->west && c->lon <= e->east;
            double area = in ? (e->north - e->south) * (e->east - e->west) : 0;
            if (in ? !inside || area < box : !inside && d <= (e->reach_km > 0 ? e->reach_km : 3) && (!site || d < sd))
                site = e, sd = d, inside = in, box = area;
        } else {
            int same = !country || (e->parent && !strcmp(e->parent, country)) || !outlined(e->parent);   /* not across a border */
            if (same && d <= (e->reach_km > 0 ? e->reach_km : 40) && (!city || d < cd)) city = e, cd = d;
            if (d <= 400 && (!near || d < nd)) near = e, nd = d;
        }
    }
    if (site) {
        *how = inside ? GPS_INSIDE : GPS_NEAR;
        *km = sd;
        return site;
    }
    if (city) {
        *km = cd;
        return city;
    }
    const known_entry *up = country ? known_find(a->kv, a->kn, country) : NULL;
    if (up) {
        *how = GPS_OUTLINE;
        return up;
    }
    up = near ? known_find(a->kv, a->kn, near->parent) : NULL;
    if (up) {
        *how = GPS_CITY_COUNTRY;
        *km = nd;
    }
    return up;
}

/* why a place was offered from the GPS, in words */
static char *gps_why(const clues *c, const known_entry *e, int how, double km)
{
    const char *l = e->label ? e->label : e->key + 6, *s = c->gps == 1 ? "" : "s";
    if (how == GPS_INSIDE) return xprintf("the GPS of %zu photo%s: within %s", c->gps, s, l);
    if (how == GPS_OUTLINE) return xprintf("the GPS of %zu photo%s: in %s, far from its cities", c->gps, s, l);
    if (how == GPS_CITY_COUNTRY) return xprintf("the GPS of %zu photo%s: in %s, perhaps (%.0f km from a city of it)", c->gps, s, l, km);
    return xprintf("the GPS of %zu photo%s: %.0f km from %s", c->gps, s, km, l);
}

/* one namespace's value(s) for a folder: the choices are values used before that its words name,
 * then a value made from its name (for one-valued namespaces), then other values used before */
static void ask_namespace(asking *a, const arv_home *h, const archive *cat, const tag_words_t *tw, const clues *c,
                          const char *name, const char *ns, int one, int sub, strlist *taken, strlist *tags)
{
    strlist used = { 0 }, opts = { 0 }, why = { 0 }, marked = { 0 }, certain = { 0 }, got;
    known_choices(a, c, ns, one, &opts, &why, &marked, &certain);
    size_t strong = opts.n;                 /* known names, learned values and the text's names: worth a question */
    size_t nl = strlen(ns);
    for (size_t i = 0; i < c->learned.n; i++)  /* the values its words went with before (words.c) */
        if (!strncmp(c->learned.v[i], ns, nl) && c->learned.v[i][nl] == ':' && !strlist_has(&opts, c->learned.v[i] + nl + 1)) {
            strlist_add(&opts, c->learned.v[i] + nl + 1);
            strlist_add(&why, c->learned_why.v[i]);
            strong++;
            if (!marked.n || !one) strlist_add(&marked, c->learned.v[i] + nl + 1);
        }
    used_values(h, cat, ns, &used);
    for (size_t i = 0; i < used.n; i++)     /* a value used before, named by a word here */
        for (size_t w = 0; w < c->words.n; w++)
            if (strstr(used.v[i], c->words.v[w]) && !strlist_has(&opts, used.v[i]) && !strlist_has(taken, c->words.v[w])) {
                char *wy = xprintf("used before; '%s' is in the names", c->words.v[w]);
                strlist_add(&opts, used.v[i]);
                strlist_add(&why, wy);
                free(wy);
                if (!marked.n || !one) strlist_add(&marked, used.v[i]);
            }
    if (!strcmp(ns, "source") && c->scans.n && c->scans.n * 2 >= c->pdfs_read && !strlist_has(&opts, "scan")) {
        char *y = xprintf("%zu of %zu PDF%s read have no text", c->scans.n, c->pdfs_read, c->pdfs_read == 1 ? "" : "s");
        strlist_add(&opts, "scan");         /* mostly scans: the folder's; else each scan's own (ask_suggest) */
        strlist_add(&why, y);
        strong++;
        if (!marked.n || !one) strlist_add(&marked, "scan");
        free(y);
    }
    if (!strcmp(ns, "source") && c->camera) {   /* what made the photos, from their EXIF */
        char *v = slug(c->camera);
        if (!strlist_has(&opts, v)) {
            strlist_add(&opts, v);
            strlist_add(&why, "the photos' EXIF");
            strong++;
            if (!marked.n || !one) strlist_add(&marked, v);
        }
        free(v);
    }
    if (!strcmp(ns, "place") && c->gps) {   /* where the photos were taken, from their EXIF: the place, not the position */
        int how;
        double km;
        const known_entry *e = gps_place(a, c, &how, &km);
        if (e && !strlist_has(&opts, e->key + 6)) {
            char *y = gps_why(c, e, how, km);
            strlist_add(&opts, e->key + 6);
            strlist_add(&why, y);
            strong++;
            if (!marked.n || !one) strlist_add(&marked, e->key + 6);
            free(y);
        }
    }
    if (one && !sub && strcmp(ns, "source")) {      /* the name, as a value: "2019-07 Kyoto trip" -> kyoto-trip */
        strlist w = { 0 };
        name_words(name, &w);
        sbuf b = { 0 };
        for (size_t i = 0; i < w.n; i++) sb_printf(&b, "%s%s", i ? "-" : "", w.v[i]);
        if (b.len && !strlist_has(&opts, b.s)) {
            strlist_add(&opts, b.s);
            strlist_add(&why, "from the name");
            if (a->interactive && !marked.n) strlist_add(&marked, b.s);   /* offered first; never taken unasked */
        }
        free(b.s);
        strlist_free(&w);
    }
    for (size_t w = 0; !one && strcmp(ns, "source") && w < c->words.n && opts.n < 6; w++)   /* the names' other words: a place? a person? */
        if (!strlist_has(taken, c->words.v[w]) && !strlist_has(&opts, c->words.v[w])) {
            strlist_add(&opts, c->words.v[w]);
            strlist_add(&why, "a word in the names");
        }
    for (size_t w = 0; !one && (!strcmp(ns, "place") || !strcmp(ns, "person")) && w < c->text_names.n && opts.n < 8; w++) {
        char *v = slug(c->text_names.v[w]);       /* names in the text: a place? a person? */
        if (!strlist_has(taken, c->text_names.v[w]) && !strlist_has(&opts, v)) {
            strlist_add(&opts, v);
            strlist_add(&why, "a name in the text");
            strong++;
        }
        free(v);
    }
    for (size_t i = 0; i < used.n && opts.n < 8; i++)
        if (!strlist_has(&opts, used.v[i])) {
            strlist_add(&opts, used.v[i]);
            strlist_add(&why, "used before");
        }
    int core = !strcmp(ns, "event") || !strcmp(ns, "project") || !strcmp(ns, "place") || !strcmp(ns, "person");
    if (core ? opts.n || (a->interactive && !sub) : strong > 0) {   /* the others: only with something to go on */
        char *q = xprintf("%s: %s?%s", ns, ns_desc(tw, ns), one ? "" : " (one or more)");
        if (certain.n && certain.n == strong) {   /* the likely ones all known and certain: taken without a question */
            memset(&got, 0, sizeof got);
            for (size_t i = 0; i < certain.n && (!one || !got.n); i++) strlist_add(&got, certain.v[i]);
            for (size_t i = 0; i < got.n; i++)
                for (size_t k = 0; k < opts.n; k++)
                    if (!strcmp(opts.v[k], got.v[i])) fprintf(stderr, "%s: %s (%s; taken without asking)\n", ns, got.v[i], why.v[k]);
        } else {
            choose(a, q, &opts, &why, &marked, !one, &got);
        }
        for (size_t i = 0; i < got.n; i++) {
            char *v = slug(got.v[i]), *t = xprintf("%s:%s", ns, v);
            if (*v && !strlist_has(tags, t)) strlist_add(tags, t);
            strlist parts = { 0 };          /* a place's words are spoken for: not offered as a person next (an */
            if (!one) name_words(v, &parts);   /* event's are often its place: "kyoto-trip") */
            for (size_t k = 0; k < parts.n; k++)
                if (!strlist_has(taken, parts.v[k])) strlist_add(taken, parts.v[k]);
            strlist_free(&parts);
            free(v);
            free(t);
        }
        strlist_free(&got);
        free(q);
    }
    strlist_free(&used);
    strlist_free(&opts);
    strlist_free(&why);
    strlist_free(&marked);
    strlist_free(&certain);
}

/* the tags (topics and namespaces) and the date of one folder, asked about */
static char *ask_folder(asking *a, const arv_home *h, const archive *cat, const tag_words_t *tw, const clues *c,
                        const char *name, const items *l, int topics, const char *parent_date, const char *parent_clue,
                        strlist *tags)
{
    int sub = !topics;                      /* one of its folders: only what may differ from the whole */
    strlist taken = { 0 };
    if (topics) {                           /* the vocabulary's topics: those the names or Match rules point to, marked */
        strlist opts = { 0 }, why = { 0 }, marked = { 0 }, got;
        for (size_t w = 0; w < c->words.n; w++) {
            const char *t = NULL, *via = NULL;
            for (size_t i = 0; i < tw->names.n && !t; i++)
                if (!strcmp(tw->names.v[i], c->words.v[w])) t = tw->names.v[i];
            for (size_t i = 0; i < tw->alias_from.n && !t; i++)
                if (!strcmp(tw->alias_from.v[i], c->words.v[w])) t = tw->alias_to.v[i], via = tw->alias_from.v[i];
            if (t && !strlist_has(&marked, t)) {
                strlist_add(&marked, t);
                strlist_add(&taken, c->words.v[w]);
                strlist_add(&opts, t);
                char *wy = via ? xprintf("'%s' is in the names", via) : xstrdup("in the names");
                strlist_add(&why, wy);
                free(wy);
            }
        }
        for (size_t r = 0; r < tw->rule_tag.n; r++) {   /* Match: a tenth of the files or more */
            size_t hit = 0;
            for (size_t i = 0; i < l->n; i++) hit += vocab_path_matches(tw->rule_glob.v[r], l->v[i].path);
            if (hit && hit * 10 >= l->n && !strlist_has(&marked, tw->rule_tag.v[r])) {
                strlist_add(&marked, tw->rule_tag.v[r]);
                strlist_add(&opts, tw->rule_tag.v[r]);
                char *wy = xprintf("%zu files match %s", hit, tw->rule_glob.v[r]);
                strlist_add(&why, wy);
                free(wy);
            }
        }
        for (size_t i = 0; i < c->learned.n; i++)  /* the topics its words went with before (words.c) */
            if (!strchr(c->learned.v[i], ':') && !strlist_has(&marked, c->learned.v[i])) {
                strlist_add(&marked, c->learned.v[i]);
                strlist_add(&opts, c->learned.v[i]);
                strlist_add(&why, c->learned_why.v[i]);
            }
        for (size_t i = 0; i < tw->names.n; i++)
            if (!strchr(tw->names.v[i], ':') && !strlist_has(&opts, tw->names.v[i])) {
                strlist_add(&opts, tw->names.v[i]);
                strlist_add(&why, "");
            }
        choose(a, "Topics: which of these is it about? (one or more)", &opts, &why, &marked, 1, &got);
        for (size_t i = 0; i < got.n; i++) {
            char *t = tag_normalise(got.v[i]);
            for (size_t k = 0; k < tw->alias_from.n; k++)
                if (!strcmp(tw->alias_from.v[k], t)) {
                    free(t);
                    t = xstrdup(tw->alias_to.v[k]);
                    break;
                }
            if (*t && !strlist_has(tags, t)) strlist_add(tags, t);
            free(t);
        }
        strlist_free(&got);
        strlist_free(&opts);
        strlist_free(&why);
        strlist_free(&marked);
    }
    static const char *const ORDER[] = { "event", "project", "place", "person", "family", "organisation", "software", "work",
                                         "source", NULL };   /* the narrowing first */
    strlist order = { 0 };
    for (int k = 0; ORDER[k]; k++)
        if (strlist_has(&tw->ns_names, ORDER[k])) strlist_add(&order, ORDER[k]);
    for (size_t i = 0; i < tw->ns_names.n; i++)
        if (!strlist_has(&order, tw->ns_names.v[i])) strlist_add(&order, tw->ns_names.v[i]);
    for (size_t i = 0; i < order.n; i++) {
        int one = strlist_has(&tw->ns_one, order.v[i]);
        int scanned = !strcmp(order.v[i], "source") && c->scans.n && c->scans.n * 2 >= c->pdfs_read;   /* documents, but scans */
        if ((!asks(c->kind, order.v[i]) && !scanned) || (sub && (one || !strcmp(order.v[i], "source")))) continue;
        ask_namespace(a, h, cat, tw, c, name, order.v[i], one, sub, &taken, tags);
    }
    strlist_free(&order);
    char *date = c->first ? edtf_of(c->first, c->last) : NULL;
    if (sub && (!date || (parent_date && !strcmp(date, parent_date)) || (parent_clue && !strcmp(date, parent_clue))
                || (parent_date && edtf_covers(parent_date, date) == 1 && parent_clue && edtf_covers(date, parent_clue) == 1))) {
        /* within the whole's, as the clues have it: nothing to ask */
        free(date);
        date = NULL;
    } else if (strlist_has(&tw->ns_names, "date")) {
        strlist opts = { 0 }, why = { 0 }, marked = { 0 }, got;
        if (date) {
            strlist_add(&opts, date);
            char *wy = xprintf("from %s", c->date_from);
            strlist_add(&why, wy);
            free(wy);
            if (strcmp(c->date_from, "file times")) strlist_add(&marked, date);   /* a file's time is a weak clue */
        }
        choose(a, "date: when is it from? (an EDTF date: 2019-07, 1995/2008, 197X, 1995~)", &opts, &why, &marked, 0, &got);
        long lo, hi;
        while (got.n && edtf_span(got.v[0], &lo, &hi) != 1 && a->interactive && !a->stopped) {   /* not a date: once more */
            fprintf(stderr, "%s is not an EDTF date.\n", got.v[0]);
            strlist_free(&got);
            choose(a, "date: when is it from? (2019-07, 1995/2008, 197X, 1995~; '-' none)", &opts, &why, &marked, 0, &got);
        }
        free(date);
        date = got.n && edtf_span(got.v[0], &lo, &hi) == 1 ? xstrdup(got.v[0]) : NULL;
        if (date) {
            char *t = xprintf("date:%s", date);
            if (!strlist_has(tags, t)) strlist_add(tags, t);
            free(t);
        }
        strlist_free(&got);
        strlist_free(&opts);
        strlist_free(&why);
        strlist_free(&marked);
    }
    strlist_free(&taken);
    return date;
}

/* the files of one folder of a target, relative to it */
static void folder_items(const items *l, const char *folder, items *out)
{
    memset(out, 0, sizeof *out);
    out->v = xmalloc((l->n + 1) * sizeof *out->v);
    size_t n = strlen(folder);
    for (size_t i = 0; i < l->n; i++)
        if (!strncmp(l->v[i].path, folder, n) && l->v[i].path[n] == '/') {
            out->v[out->n] = l->v[i];
            out->v[out->n++].path = xstrdup(l->v[i].path + n + 1);
        }
}

static const char *tag_value(const strlist *tags, const char *ns)
{
    size_t n = strlen(ns);
    for (size_t i = 0; i < tags->n; i++)
        if (!strncmp(tags->v[i], ns, n) && tags->v[i][n] == ':') return tags->v[i] + n + 1;
    return NULL;
}

void ask_suggest(const arv_home *h, archive *cat, const items *l, const char *name, const char *root, int interactive,
                 int per_folder, suggestion *out)
{
    memset(out, 0, sizeof *out);
    asking a = { interactive, 0, 0, NULL, 0, NULL };
    known_entry *kv = NULL;
    a.kn = h ? known_load(h, &kv) : 0;
    a.kv = kv;
    tag_words_t tw;
    if (!h || tag_words(h, &tw)) memset(&tw, 0, sizeof tw);
    word_stats *ws = h ? word_stats_load(h, cat) : NULL;
    if (ws) word_stats_vocab(ws, &tw.names, &tw.alias_from);   /* your vocabulary's words are never common */
    a.ws = ws;
    clues c;
    gather(ws, l, name, root, &c);
    {                                       /* what the files say by themselves */
        sbuf b = { 0 };
        sb_printf(&b, "Clues: %zu file%s", c.files, c.files == 1 ? "" : "s");
        for (size_t i = 0; i < c.kinds.n && i < 4 && !(c.kinds.n == 1 && !strcmp(c.kinds.v[0], "files")); i++) {
            char *one = counted(c.kind_n[i], c.kinds.v[i]);
            sb_printf(&b, "%s%s", i ? ", " : ": ", one);
            free(one);
        }
        if (c.first) {
            char *e = edtf_of(c.first, c.last);
            sb_printf(&b, "; dates %s (from %s)", e, c.date_from);
            free(e);
        }
        if (c.camera) sb_printf(&b, "; camera %s (EXIF)", c.camera);
        else if (c.camera_names * 2 > c.files) sb_puts(&b, "; named by a camera or phone");
        if (c.scans.n)                  /* scans: their words come once they have a text layer */
            sb_printf(&b, "; %zu of %zu PDF%s read with no text (scans: source:scan; ocrmypdf --skip-text, then describe again)",
                      c.scans.n, c.pdfs_read, c.pdfs_read == 1 ? "" : "s");
        if (c.gps) {
            int how;
            double km;
            const known_entry *e = gps_place(&a, &c, &how, &km);
            sb_printf(&b, "; GPS in %zu photo%s", c.gps, c.gps == 1 ? "" : "s");
            if (e) sb_printf(&b, how == GPS_NEAR ? ", near %s" : how == GPS_CITY_COUNTRY ? ", in %s perhaps" : ", in %s",
                             e->label ? e->label : e->key + 6);
        }
        if (c.words.n) {
            sb_puts(&b, "; words in the names:");
            for (size_t i = 0; i < c.words.n && i < 8; i++) sb_printf(&b, " %s", c.words.v[i]);
        }
        if (c.text.n) {
            sb_puts(&b, "; words in the text:");
            for (size_t i = 0; i < c.text.n && i < 8; i++) sb_printf(&b, "%s %s", i ? "," : "", c.text.v[i]);
        }
        if (c.text_names.n) {
            sb_puts(&b, "; names in the text:");
            for (size_t i = 0; i < c.text_names.n && i < 6; i++) {
                char *nm = xstrdup(c.text_names.v[i]);
                for (char *q = nm; *q; q++)
                    if (q == nm || q[-1] == ' ') *q = (char)toupper((unsigned char)*q);
                sb_printf(&b, "%s %s", i ? "," : "", nm);
                free(nm);
            }
        }
        if (!root && interactive) sb_puts(&b, " (the files themselves are not here: no EXIF or text)");
        fprintf(stderr, "%s\n", b.s);
        for (size_t i = 0; i < c.learned.n && i < 5; i++)
            fprintf(stderr, "  points to %s: %s\n", c.learned.v[i], c.learned_why.v[i]);
        free(b.s);
    }
    if (!c.kind && interactive && c.kinds.n > 1) {   /* what it mostly is decides what is asked next */
        strlist opts = { 0 }, marked = { 0 }, got;
        for (size_t i = 0; i < c.kinds.n; i++) strlist_add(&opts, c.kinds.v[i]);
        strlist_add(&opts, "a mix");
        choose(&a, "What is it mostly?", &opts, NULL, &marked, 0, &got);
        for (size_t i = 0; got.n && i < c.kinds.n; i++)
            if (!strcmp(got.v[0], c.kinds.v[i])) c.kind = c.kinds.v[i];
        strlist_free(&got);
        strlist_free(&opts);
        strlist_free(&marked);
    }
    strlist tags = { 0 };
    char *date = ask_folder(&a, h, cat, &tw, &c, name, l, 1, NULL, NULL, &tags);
    char *clue_date = c.first ? edtf_of(c.first, c.last) : NULL;
    ftags_get(&out->folders, ".", 1);
    for (size_t i = 0; i < tags.n; i++) strlist_add(ftags_get(&out->folders, ".", 1), tags.v[i]);
    if (c.scans.n && !strlist_has(&tags, "source:scan"))   /* some scans among PDFs with text: each its own tag, */
        for (size_t i = 0; i < c.scans.n; i++) {          /* so arv find source:scan lists what still needs OCR */
            strlist *t = ftags_get(&out->folders, c.scans.v[i], 1);
            if (!strlist_has(t, "source:scan")) strlist_add(t, "source:scan");
        }
    for (size_t i = 0; i < tags.n; i++)
        if (!strchr(tags.v[i], ':')) strlist_add(&out->subjects, tags.v[i]);
    /* its folders, each in turn (first level, with files of their own) */
    strlist folders = { 0 };
    for (size_t i = 0; i < l->n; i++) {
        const char *slash = strchr(l->v[i].path, '/');
        if (!slash) continue;
        char *f = xprintf("%.*s", (int)(slash - l->v[i].path), l->v[i].path);
        if (!strlist_has(&folders, f)) strlist_add(&folders, f);
        free(f);
    }
    int each = 0;
    if (per_folder && interactive && !a.stopped && folders.n >= 2 && folders.n <= 40 && a.asked < MAX_QUESTIONS) {
        char *q = xprintf("\nIts %zu folders: ask about each of them too (what is it, where, when)? [y/N] ", folders.n);
        char *ans = reply(&a, q, qjson("yesno", q + 1));
        each = ans[0] == 'y' || ans[0] == 'Y';
        free(ans);
        free(q);
    }
    for (size_t f = 0; each && f < folders.n && !a.stopped && a.asked < MAX_QUESTIONS; f++) {
        items sub;
        folder_items(l, folders.v[f], &sub);
        clues fc;
        char *froot = root ? join(root, folders.v[f]) : NULL;
        gather(ws, &sub, folders.v[f], froot, &fc);
        fc.kind = fc.kind ? fc.kind : c.kind;
        fprintf(stderr, "\n-- %s/ (%zu file%s)\n", folders.v[f], sub.n, sub.n == 1 ? "" : "s");
        strlist ft = { 0 };
        free(ask_folder(&a, h, cat, &tw, &fc, folders.v[f], &sub, 0, date, clue_date, &ft));
        for (size_t i = 0; i < ft.n; i++)
            if (!strlist_has(&tags, ft.v[i])) strlist_add(ftags_get(&out->folders, folders.v[f], 1), ft.v[i]);
        if (interactive && !a.stopped && !dry) word_example_add(h, &ft, &fc.terms, &fc.text);   /* what was told, kept */
        strlist_free(&ft);
        clues_free(&fc);
        items_free(&sub);
        free(froot);
    }
    if (interactive && h) {                 /* what was told, kept: the next folder's questions are shorter */
        if (!dry) word_example_add(h, &tags, &c.terms, &c.text);
        word_rule rules[3];                 /* a word that keeps going with a tag: offered as a rule */
        size_t offered = word_rules_offered(ws, &tw, &tags, &c.terms, 1, rules, 3);
        for (size_t i = 0; i < offered && !a.stopped; i++) {
            char *q = xprintf("\n'%s' has been in %zu of %zu folders tagged %s: make it a rule (Match: %s), so "
                              "folders with it are tagged without asking? [y/N] ", rules[i].word, rules[i].with, rules[i].all,
                              rules[i].tag, rules[i].glob);
            char *ans = reply(&a, q, qjson("yesno", q + 1));
            if ((ans[0] == 'y' || ans[0] == 'Y') && !dry) {
                if (word_rule_add(h, rules[i].tag, rules[i].word)) fprintf(stderr, "Could not write %s/tags.rec\n", h->config_dir);
                else fprintf(stderr, "Added to the vocabulary (%s/tags.rec): %s, Match: %s\n", h->config_dir, rules[i].tag,
                             rules[i].glob);
            }
            free(ans);
            free(q);
        }
        word_rules_free(rules, offered);
    }
    for (size_t f = 0; interactive && h && !dry && f < out->folders.n; f++)   /* a place from extra data, used: copied home */
        for (size_t i = 0; i < out->folders.tags[f].n; i++)
            if (!strncmp(out->folders.tags[f].v[i], "place:", 6)) known_keep(h, a.kv, a.kn, out->folders.tags[f].v[i]);
    if (interactive && h && cat && !dry) {  /* a name confirmed: known from now on (an entry, and an event) */
        static const char *const NAMED[] = { "person:", "family:", "organisation:", "software:", "work:", "place:", NULL };
        strlist made = { 0 };
        char today[11], *who = person();
        today_iso(today);
        for (size_t f = 0; f < out->folders.n; f++)
            for (size_t i = 0; i < out->folders.tags[f].n; i++) {
                const char *t = out->folders.tags[f].v[i];
                int named = 0;
                for (int k = 0; NAMED[k] && !named; k++) named = !strncmp(t, NAMED[k], strlen(NAMED[k]));
                int known = strlist_has(&made, t);
                for (size_t k = 0; k < a.kn && !known; k++) known = !strcmp(a.kv[k].key, t);
                if (!named || known) continue;
                char *label = known_label_of(t), *basis = xprintf("confirmed by %s in arv describe --ask of %s, %s", who, name, today);
                known_made(h, cat, t, label, basis);
                strlist_add(&made, t);
                const known_entry *in = NULL;
                if (!strncmp(t, "place:", 6)) {     /* a place of the gazetteer's own: in a known place it was named with, */
                    known_field(h, t, "Kind", "site", 0);   /* or the city nearest the photos' GPS */
                    for (size_t k = 0; k < out->folders.tags[f].n && !in; k++) {
                        const known_entry *e = known_find(a.kv, a.kn, out->folders.tags[f].v[k]);
                        if (e && strcmp(e->key, t) && !strncmp(e->key, "place:", 6)) in = e;
                    }
                    int how;
                    double km;
                    if (!in) in = gps_place(&a, &c, &how, &km);
                    if (in) known_field(h, t, "Parent", in->key, 0);
                }
                fprintf(stderr, "Known from now on: %s (%s)%s%s%s; arv tags show %s\n", t, label,
                        !strncmp(t, "person:", 7) || !strncmp(t, "family:", 7) ? ", private" : "", in ? ", in " : "",
                        in ? (in->label ? in->label : in->key) : "", t);
                free(label);
                free(basis);
            }
        if (made.n) archive_save(cat, h->rec_path);
        strlist_free(&made);
        free(who);
    }
    /* the title and description, made from the answers */
    const char *what = c.kind ? c.kind : "files";
    const char *event = tag_value(&tags, "event"), *project = tag_value(&tags, "project");
    char *when = date ? date_words(date) : NULL;
    sbuf t = { 0 }, d = { 0 };
    char *head = event ? pretty(event) : project ? pretty(project) : name_title(name);
    sb_puts(&t, head);
    if (when && !name_date(head)) sb_printf(&t, ", %s", when);
    char *kinds_text = NULL;
    {
        sbuf k = { 0 };
        for (size_t i = 0; i < c.kinds.n && i < 3; i++) {
            char *one = counted(c.kind_n[i], c.kinds.v[i]);
            sb_printf(&k, "%s%s", i ? (i + 1 == c.kinds.n || i == 2 ? " and " : ", ") : "", one);
            free(one);
        }
        kinds_text = k.s ? k.s : xstrdup("no files");
    }
    char *upper = xstrdup(kinds_text);
    sb_puts(&d, upper);
    free(upper);
    for (size_t i = 0; i < c.titles.n && i < 3; i++)   /* the PDFs' own titles say what they are */
        sb_printf(&d, "%s%s", i ? ", " : " (", c.titles.v[i]);
    if (c.titles.n) sb_printf(&d, "%s)", c.titles.n > 3 ? ", ..." : "");
    strlist places = { 0 }, people = { 0 };
    for (size_t i = 0; i < tags.n; i++) {
        if (!strncmp(tags.v[i], "place:", 6)) strlist_add(&places, tags.v[i] + 6);
        if (!strncmp(tags.v[i], "person:", 7)) strlist_add(&people, tags.v[i] + 7);
    }
    if (c.camera && !strcmp(what, "photos")) sb_printf(&d, ", from a %s", c.camera);
    for (size_t i = 0; i < places.n; i++) {     /* by their labels when known: "Ōsaka", "Fushimi Inari" */
        char *key = xprintf("place:%s", places.v[i]);
        const known_entry *e = known_find(a.kv, a.kn, key);
        char *p = e && e->label ? xstrdup(e->label) : known_label_of(key);
        sb_printf(&d, "%s%s", i ? " and " : ", in ", p);
        free(p);
        free(key);
    }
    for (size_t i = 0; i < people.n; i++) {
        char *key = xprintf("person:%s", people.v[i]);
        const known_entry *e = known_find(a.kv, a.kn, key);
        char *p = e && e->label ? xstrdup(e->label) : pretty(people.v[i]);
        free(key);
        sb_printf(&d, "%s%s", i ? (i + 1 == people.n ? " and " : ", ") : ", with ", p);
        free(p);
    }
    if (event) {
        char *e = pretty(event);
        sb_printf(&d, ", of the %s", e);
        free(e);
    }
    if (when) sb_printf(&d, ", %s", when);
    sb_puts(&d, ".");
    out->title = field(&a, "Title", t.s);
    out->description = field(&a, "Description", d.s);
    fprintf(stderr, "\n%d question%s asked.\n", a.asked, a.asked == 1 ? "" : "s");
    free(t.s);
    free(d.s);
    free(head);
    free(when);
    free(date);
    free(clue_date);
    free(kinds_text);
    strlist_free(&places);
    strlist_free(&people);
    strlist_free(&folders);
    strlist_free(&tags);
    clues_free(&c);
    word_stats_free(ws);
    known_free(kv, a.kn);
}
