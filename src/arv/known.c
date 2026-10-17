/*
 * Known names: authority control for one person's archive (research/plan.md, "Known names, as
 * authority control"). An entry is a namespaced Tag record of the vocabulary (config/tags.rec):
 * its key (Name, made once and kept), its preferred form now (Label), the other forms it is found
 * as (Alias), what tells two of one name apart (Qualifier), entries it relates to (Related), the
 * same entity elsewhere (Same), why it was made (Basis), whether it stays home (Private) and
 * whether it is withdrawn (Retired). Changes to entries are events about name:KEY; a change to
 * where an entry is used is an ordinary tag change, an event with Sets and Replaces.
 *
 *   arv tags show KEY
 *   arv tags add KEY [--label L] [--alias A]... [--qualifier Q] [--same ID]... [--private|--public]
 *   arv tags label KEY LABEL          (the old label becomes an alias)
 *   arv tags alias KEY FORM
 *   arv tags merge KEY INTO           (every use of KEY becomes INTO; KEY is retired, an alias of INTO)
 *   arv tags split KEY [--new KEY2] [--qualifier Q] [--new-qualifier Q2]   (each use asked about)
 *   arv tags retire KEY [--why TEXT]
 */
#define _XOPEN_SOURCE 700
#include "arv.h"
#include "data.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *vocab_file(const arv_home *h)
{
    char *p = join(h->config_dir, "tags.rec");
    if (access(p, F_OK) && !mkdirs(h->config_dir)) write_text(p, DATA_DEFAULT_TAGS);
    return p;
}

/* the gazetteer: places, entries of their own file, copied from Natural Earth's on first use */
static char *places_file(const arv_home *h)
{
    char *p = join(h->config_dir, "places.rec");
    if (access(p, F_OK) && !mkdirs(h->config_dir)) write_text(p, DATA_DEFAULT_PLACES);
    return p;
}

/* the file an entry is in, or goes in: a place's in the gazetteer, unless tags.rec has it already */
static char *file_for(const arv_home *h, const char *key)
{
    char *tags = vocab_file(h);
    if (strncmp(key, "place:", 6)) return tags;
    char *text = read_text(tags), *line = xprintf("Name: %s\n", key), *at = text ? strstr(text, line) : NULL;
    int here = at && (at == text || at[-1] == '\n');
    free(text);
    free(line);
    if (here) return tags;
    free(tags);
    return places_file(h);
}

/* the namespaces whose entries stay home by default: people's names are personal data */
static int private_by_default(const char *key)
{
    return !strncmp(key, "person:", 7) || !strncmp(key, "family:", 7);
}

/* ISO 6709 in decimal degrees (+35.03+135.75/), or "35.03,135.75" as a map shows it */
int iso6709_parse(const char *s, double *lat, double *lon)
{
    char *end;
    while (*s == ' ') s++;
    double a = strtod(s, &end);
    if (end == s) return -1;
    const char *p = end;
    while (*p == ',' || *p == ' ') p++;
    if (*p != '+' && *p != '-' && !isdigit((unsigned char)*p)) return -1;
    if (p == end && *p != '+' && *p != '-') return -1;
    double b = strtod(p, &end);
    if (end == p) return -1;
    while (*end == ' ' || *end == '/') end++;
    if (*end || a < -90 || a > 90 || b < -180 || b > 180) return -1;
    *lat = a;
    *lon = b;
    return 0;
}

/* two corners, ISO 6709 each ("+34.96+135.76/+34.98+135.78/"), or "lat,lon lat,lon"; any two opposite */
int extent_parse(const char *s, double *south, double *west, double *north, double *east)
{
    const char *mid = strchr(s, '/');
    if (!mid || !mid[1]) {                  /* "34.96,135.76 34.98,135.78" */
        mid = strchr(s, ' ');
        while (mid && mid[1] == ' ') mid++;
        if (!mid) return -1;
    }
    char *a = xprintf("%.*s", (int)(mid - s), s);
    double la1, lo1, la2, lo2;
    int bad = iso6709_parse(a, &la1, &lo1) || iso6709_parse(mid + 1, &la2, &lo2);
    free(a);
    if (bad || la1 == la2 || lo1 == lo2) return -1;
    *south = la1 < la2 ? la1 : la2, *north = la1 < la2 ? la2 : la1;
    *west = lo1 < lo2 ? lo1 : lo2, *east = lo1 < lo2 ? lo2 : lo1;
    return 0;
}

double reach_parse(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    while (*end == ' ') end++;
    if (end == s || v <= 0) return -1;
    if (!strcmp(end, "km") || !*end) return v;
    if (!strcmp(end, "m")) return v / 1000;
    return -1;
}

/* ISO 6709: degrees padded (+05.00, -007.50), to four decimals (about 11 m), trailing zeros dropped to two */
static void iso_part(sbuf *b, double v, int width)
{
    char t[32];
    snprintf(t, sizeof t, "%+0*.4f", width + 6, v);
    size_t n = strlen(t);
    while (n > 0 && t[n - 1] == '0' && t[n - 3] != '.') t[--n] = 0;
    sb_puts(b, t);
}

char *iso6709(double lat, double lon)
{
    sbuf b = { 0 };
    iso_part(&b, lat, 2);
    iso_part(&b, lon, 3);
    sb_puts(&b, "/");
    return b.s;
}

static void known_free_one(known_entry *e);
static char *key_slug(const char *s);

static int is_entry(const rec_record *r, const char *type)
{
    const char *name = rec_get(r, "Name");
    return !r->descriptor && name && strchr(name, ':') && (!r->type || !strcmp(r->type, type));
}

static void load_file(const char *p, const char *type, known_entry **out, size_t *n)
{
    rec_file f;
    int bad = 0;
    if (p ? rec_read(p, &f, &bad) : rec_parse(DATA_DEFAULT_PLACES, &f, &bad)) return;   /* NULL: the built-in gazetteer */
    for (size_t i = 0; i < f.nrecords; i++) {
        const rec_record *r = &f.records[i];
        if (!is_entry(r, type)) continue;
        *out = xrealloc(*out, (*n + 1) * sizeof **out);
        known_entry *e = &(*out)[(*n)++];
        memset(e, 0, sizeof *e);
        e->key = xstrdup(rec_get(r, "Name"));
        for (size_t j = 0; j < r->nfields; j++) {
            const char *fn = r->fields[j].name, *v = r->fields[j].value;
            if (!strcmp(fn, "Label") && !e->label) e->label = xstrdup(v);
            else if (!strcmp(fn, "Alias")) strlist_add(&e->aliases, v);
            else if (!strcmp(fn, "Qualifier") && !e->qualifier) e->qualifier = xstrdup(v);
            else if (!strcmp(fn, "Related")) strlist_add(&e->related, v);
            else if (!strcmp(fn, "Same")) strlist_add(&e->same, v);
            else if (!strcmp(fn, "Basis") && !e->basis) e->basis = xstrdup(v);
            else if (!strcmp(fn, "Private")) e->private_ = !strcmp(v, "yes");
            else if (!strcmp(fn, "Retired")) e->retired = xstrdup(v);
            else if (!strcmp(fn, "Kind") && !e->kind) e->kind = xstrdup(v);
            else if (!strcmp(fn, "Parent") && !e->parent) e->parent = xstrdup(v);
            else if (!strcmp(fn, "Country") && !e->country) e->country = xstrdup(v);
            else if (!strcmp(fn, "Region")) strlist_add(&e->regions, v);
            else if (!strcmp(fn, "Reach")) e->reach_km = reach_parse(v) > 0 ? reach_parse(v) : 0;
            else if (!strcmp(fn, "Extent"))
                e->has_extent = !extent_parse(v, &e->south, &e->west, &e->north, &e->east);
            else if (!strcmp(fn, "At") && !e->at) {
                e->at = xstrdup(v);
                e->has_at = !iso6709_parse(v, &e->lat, &e->lon);
            }
        }
        if (!e->has_at && e->has_extent) {     /* a box alone: it is where its middle is */
            e->lat = (e->south + e->north) / 2;
            e->lon = (e->west + e->east) / 2;
            e->has_at = 1;
        }
    }
    rec_free(&f);
}

/* ------------------------------------------------------------------ keys, indexed */

typedef struct {
    char **key;
    size_t *at, cap, n;
} key_index;

static size_t key_hash(const char *s)
{
    size_t h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

static void index_free(key_index *x)
{
    free(x->key);
    free(x->at);
    memset(x, 0, sizeof *x);
}

static void index_put(key_index *x, char *key, size_t at)
{
    if ((x->n + 1) * 2 > x->cap) {          /* grow, and put them all again */
        key_index y = { 0 };
        y.cap = x->cap ? x->cap * 2 : 1024;
        y.key = xmalloc(y.cap * sizeof *y.key);
        y.at = xmalloc(y.cap * sizeof *y.at);
        memset(y.key, 0, y.cap * sizeof *y.key);
        for (size_t i = 0; i < x->cap; i++)
            if (x->key[i]) index_put(&y, x->key[i], x->at[i]);
        index_free(x);
        *x = y;
    }
    size_t i = key_hash(key) & (x->cap - 1);
    while (x->key[i] && strcmp(x->key[i], key)) i = (i + 1) & (x->cap - 1);
    if (!x->key[i]) x->n++;
    x->key[i] = key;
    x->at[i] = at;
}

static long index_get(const key_index *x, const char *key)
{
    if (!x->cap) return -1;
    size_t i = key_hash(key) & (x->cap - 1);
    while (x->key[i]) {
        if (!strcmp(x->key[i], key)) return (long)x->at[i];
        i = (i + 1) & (x->cap - 1);
    }
    return -1;
}

/* ------------------------------------------------------------------ extra place data */

/* U+00C0 to U+017F (Latin-1 and Latin Extended-A) as ASCII: keys made from names are plain
 * ("Île-de-France" -> ile-de-france), as tags are typed */
static const char *const FOLD[] = {
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "y",
    "a", "a", "a", "a", "a", "a", "c", "c", "c", "c", "c", "c", "c", "c", "d", "d",
    "d", "d", "e", "e", "e", "e", "e", "e", "e", "e", "e", "e", "g", "g", "g", "g",
    "g", "g", "g", "g", "h", "h", "h", "h", "i", "i", "i", "i", "i", "i", "i", "i",
    "i", "i", "ij", "ij", "j", "j", "k", "k", "k", "l", "l", "l", "l", "l", "l", "l",
    "l", "l", "l", "n", "n", "n", "n", "n", "n", "n", "n", "n", "o", "o", "o", "o",
    "o", "o", "oe", "oe", "r", "r", "r", "r", "r", "r", "s", "s", "s", "s", "s", "s",
    "s", "s", "t", "t", "t", "t", "t", "t", "u", "u", "u", "u", "u", "u", "u", "u",
    "u", "u", "u", "u", "w", "w", "y", "y", "y", "z", "z", "z", "z", "z", "z", "s",
};

/* "Kyoto Prefecture" -> "kyoto-prefecture": lower case, Latin accents folded, other letters kept */
static char *key_slug(const char *s)
{
    sbuf b = { 0 };
    int dash = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const char *plain = NULL;
        size_t len = 1;
        if (p[0] >= 0xC3 && p[0] <= 0xC5 && (p[1] & 0xC0) == 0x80) {   /* U+00C0..U+017F */
            unsigned cp = ((unsigned)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            if (cp >= 0xC0 && cp < 0x180) plain = FOLD[cp - 0xC0], len = 2;
        }
        if (plain && !*plain) {             /* × ÷: not letters */
            dash = 1;
            p += len - 1;
            continue;
        }
        if (plain || isalnum(*p) || *p >= 0x80) {
            if (dash && b.len) sb_puts(&b, "-");
            if (plain) sb_puts(&b, plain);
            else {
                char c = (char)tolower(*p);
                sb_add(&b, &c, 1);
            }
            dash = 0;
            p += len - 1;
        } else {
            dash = 1;
        }
    }
    return b.s ? b.s : xstrdup("");
}

static const char *jstr_get(const jv *o, const char *key)
{
    const jv *v = json_get(o, key);
    return v && v->kind == 's' ? v->str : NULL;
}

/* ISO 3166-2 subdivisions from iso-codes, when installed: regions in their countries (and in a
 * broader subdivision when it has one), named as people write them; no positions */
static void load_iso_regions(const char *path, known_entry **out, size_t *n, key_index *keys)
{
    char *text = read_text(path);
    jv *doc = text ? json_parse(text) : NULL;
    const jv *list = doc ? json_get(doc, "3166-2") : NULL;
    free(text);
    if (!list || list->kind != 'a') {
        if (doc) jfree(doc);
        return;
    }
    key_index country = { 0 }, by_code = { 0 }, known_code = { 0 };
    for (size_t i = 0; i < *n; i++) {
        if ((*out)[i].kind && !strcmp((*out)[i].kind, "country") && (*out)[i].country) index_put(&country, (*out)[i].country, i);
        for (size_t k = 0; k < (*out)[i].same.n; k++)   /* a region copied home before: the same region, kept as it is */
            if (!strncmp((*out)[i].same.v[k], "iso3166-2:", 10)) index_put(&known_code, (*out)[i].same.v[k] + 10, i);
    }
    size_t first = *n;
    for (size_t i = 0; i < list->n; i++) {
        const jv *r = list->vals[i];
        const char *code = jstr_get(r, "code"), *name = jstr_get(r, "name"), *type = jstr_get(r, "type");
        if (!code || !name || strlen(code) < 4 || code[2] != '-' || index_get(&known_code, code) >= 0) continue;
        char cc[3] = { code[0], code[1], 0 };
        long c = index_get(&country, cc);
        char *k1 = key_slug(name), *k2 = type ? xprintf("%s %s", name, type) : xstrdup(name), *k2s = key_slug(k2);
        char *key = xprintf("place:%s", k1);
        if (index_get(keys, key) >= 0) {        /* Kyoto the city has place:kyoto: place:kyoto-prefecture */
            free(key);
            key = xprintf("place:%s", k2s);
        }
        if (index_get(keys, key) >= 0) {        /* Georgia the state and Georgia the country */
            char *more = xprintf("%s-%c%c", key, tolower((unsigned char)cc[0]), tolower((unsigned char)cc[1]));
            free(key);
            key = more;
        }
        free(k1);
        free(k2);
        free(k2s);
        if (index_get(keys, key) >= 0) {
            free(key);
            continue;
        }
        *out = xrealloc(*out, (*n + 1) * sizeof **out);
        known_entry *e = &(*out)[(*n)++];
        memset(e, 0, sizeof *e);
        e->key = key;
        e->label = xstrdup(name);
        e->qualifier = type ? xstrdup(type) : NULL;
        e->kind = xstrdup("region");
        e->country = xstrdup(code);     /* for now its code: its Parent comes from it, below */
        if (c >= 0) e->parent = xstrdup((*out)[c].key);
        e->source = "iso-codes (ISO 3166-2)";
        char *same = xprintf("iso3166-2:%s", code);
        strlist_add(&e->same, same);
        free(same);
        index_put(keys, e->key, *n - 1);
        index_put(&by_code, e->country, *n - 1);
    }
    for (size_t i = 0, k = first; k < *n; i++) {   /* a subdivision of a subdivision: Babək in Nakhchivan */
        const jv *r = list->vals[i];
        const char *code = jstr_get(r, "code"), *up = jstr_get(r, "parent");
        if (!code || strcmp(code, (*out)[k].country)) continue;
        if (up) {
            char *full = strchr(up, '-') ? xstrdup(up) : xprintf("%.2s-%s", code, up);
            long p = index_get(&by_code, full);
            if (p >= 0) {
                free((*out)[k].parent);
                (*out)[k].parent = xstrdup((*out)[p].key);
            }
            free(full);
        }
        k++;
    }
    for (size_t k = first; k < *n; k++) {
        free((*out)[k].country);
        (*out)[k].country = NULL;
    }
    index_free(&country);
    index_free(&by_code);
    index_free(&known_code);
    jfree(doc);
}

/* a name as names are compared: lower case, words one space apart; "Kyoto Prefecture" -> "kyoto" */
static char *region_form(const char *s, const char *type)
{
    char *f = key_slug(s);
    if (type) {                             /* the region's own name without its type: Hokkaido, not Hokkaido Prefecture */
        char *t = key_slug(type);
        size_t lf = strlen(f), lt = strlen(t);
        if (lf > lt + 1 && !strcmp(f + lf - lt, t) && f[lf - lt - 1] == '-') f[lf - lt - 1] = 0;
        free(t);
    }
    return f;
}

static int is_code(const char *s)          /* "JP-26", "US-CA": an ISO 3166-2 code's shape */
{
    return isupper((unsigned char)s[0]) && isupper((unsigned char)s[1]) && s[2] == '-' && s[3] && !strchr(s, ' ');
}

/* a place's Region (a name, or an ISO 3166-2 code), resolved: the place is put in that region,
 * when a region of that name or code is known in the country it is in (Uji, in Kyoto Prefecture) */
static void place_regions(known_entry *v, size_t n)
{
    key_index by_code = { 0 }, by_name = { 0 };
    strlist owned = { 0 };                  /* the index's keys: "country|name" */
    for (size_t i = 0; i < n; i++) {
        known_entry *r = &v[i];
        if (!r->kind || strcmp(r->kind, "region")) continue;
        for (size_t k = 0; k < r->same.n; k++)
            if (!strncmp(r->same.v[k], "iso3166-2:", 10)) index_put(&by_code, r->same.v[k] + 10, i);
        const known_entry *c = r;            /* its country: the top of its chain */
        for (int d = 0; c && c->parent && d < 16; d++) c = known_find(v, n, c->parent);
        if (!c) continue;
        char *f = region_form(r->label ? r->label : r->key + 6, NULL), *g = region_form(r->label ? r->label : r->key + 6, r->qualifier);
        char *k1 = xprintf("%s|%s", c->key, f), *k2 = xprintf("%s|%s", c->key, g);
        strlist_add(&owned, k1);
        index_put(&by_name, owned.v[owned.n - 1], i);
        if (strcmp(k1, k2)) {
            strlist_add(&owned, k2);
            index_put(&by_name, owned.v[owned.n - 1], i);
        }
        free(f), free(g), free(k1), free(k2);
    }
    for (size_t i = 0; by_code.n + by_name.n && i < n; i++) {
        known_entry *e = &v[i];
        if (!e->regions.n || (e->kind && (!strcmp(e->kind, "region") || !strcmp(e->kind, "country")))) continue;
        const known_entry *up = e->parent ? known_find(v, n, e->parent) : NULL;
        if (!up || !up->kind || strcmp(up->kind, "country")) continue;   /* in a region already, or nowhere known */
        long hit = -1;
        int names = 0;                      /* a name decides; a code (JP-26) only when no name is given: */
        for (size_t k = 0; k < e->regions.n; k++) names += !is_code(e->regions.v[k]);   /* GeoNames' are not all ISO's */
        for (size_t k = 0; k < e->regions.n && hit < 0; k++) {
            if (is_code(e->regions.v[k])) {
                if (!names) hit = index_get(&by_code, e->regions.v[k]);
            } else {
                char *f = region_form(e->regions.v[k], NULL), *key = xprintf("%s|%s", up->key, f);
                hit = index_get(&by_name, key);
                free(f);
                free(key);
            }
            if (hit >= 0) {                 /* a code names its country: JP-26 only for a place in Japan */
                const known_entry *c = &v[hit];
                for (int d = 0; c && c->parent && d < 16; d++) c = known_find(v, n, c->parent);
                if (!c || strcmp(c->key, up->key)) hit = -1;
            }
        }
        if (hit >= 0) {
            free(e->parent);
            e->parent = xstrdup(v[hit].key);
        }
    }
    index_free(&by_code);
    index_free(&by_name);
    strlist_free(&owned);
}

/* where extra data is looked for: $XDG_DATA_HOME (~/.local/share), then $XDG_DATA_DIRS
 * (/usr/local/share:/usr/share), each with the path given after it */
static void data_dirs(strlist *out)
{
    memset(out, 0, sizeof *out);
    const char *home = getenv("XDG_DATA_HOME"), *dirs = getenv("XDG_DATA_DIRS");
    if (home && *home) strlist_add(out, home);
    else if (getenv("HOME")) {
        char *d = xprintf("%s/.local/share", getenv("HOME"));
        strlist_add(out, d);
        free(d);
    }
    char *copy = xstrdup(dirs && *dirs ? dirs : "/usr/local/share:/usr/share");
    for (char *d = strtok(copy, ":"); d; d = strtok(NULL, ":"))
        if (*d && !strlist_has(out, d)) strlist_add(out, d);
    free(copy);
}

/* the extra place data here: iso-codes' subdivisions (the first found), and each places/NAME.rec */
void known_extra_files(char **iso, strlist *files)
{
    strlist dirs;
    data_dirs(&dirs);
    *iso = NULL;
    memset(files, 0, sizeof *files);
    for (size_t d = 0; d < dirs.n; d++) {
        char *p = xprintf("%s/iso-codes/json/iso_3166-2.json", dirs.v[d]), *dir = xprintf("%s/arv/places", dirs.v[d]);
        if (!*iso && !access(p, R_OK)) *iso = p;
        else free(p);
        DIR *dp = opendir(dir);
        for (struct dirent *de; dp && (de = readdir(dp));) {
            size_t l = strlen(de->d_name);
            if (l > 4 && !strcmp(de->d_name + l - 4, ".rec")) {
                char *f = xprintf("%s/%s", dir, de->d_name);
                strlist_add(files, f);
                free(f);
            }
        }
        if (dp) closedir(dp);
        free(dir);
    }
    strlist_free(&dirs);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* the extra data: DIR/arv/places/NAME.rec in each data folder (GeoNames, a region's own list), then
 * iso-codes' subdivisions; an entry whose key is known already is left out (the home's wins) */
static void load_extra(known_entry **out, size_t *n, key_index *keys)
{
    if (getenv("ARV_NO_EXTRA_PLACES") && *getenv("ARV_NO_EXTRA_PLACES")) return;   /* the tests: the same places everywhere */
    strlist dirs;
    data_dirs(&dirs);
    for (size_t d = 0; d < dirs.n; d++) {
        char *dir = xprintf("%s/arv/places", dirs.v[d]);
        strlist files = { 0 };
        DIR *dp = opendir(dir);
        for (struct dirent *de; dp && (de = readdir(dp));) {
            size_t l = strlen(de->d_name);
            if (l > 4 && !strcmp(de->d_name + l - 4, ".rec")) strlist_add(&files, de->d_name);
        }
        if (dp) closedir(dp);
        if (files.n) qsort(files.v, files.n, sizeof *files.v, by_name);
        for (size_t f = 0; f < files.n; f++) {
            char *path = xprintf("%s/%s", dir, files.v[f]);
            size_t first = *n;
            load_file(path, "Place", out, n);
            static strlist sources;             /* the names live as long as the program */
            strlist_add(&sources, files.v[f]);
            size_t kept = first;
            for (size_t i = first; i < *n; i++) {
                if (index_get(keys, (*out)[i].key) >= 0) {
                    known_free_one(&(*out)[i]);
                    continue;
                }
                (*out)[kept] = (*out)[i];
                (*out)[kept].source = sources.v[sources.n - 1];
                index_put(keys, (*out)[kept].key, kept);
                kept++;
            }
            *n = kept;
            free(path);
        }
        strlist_free(&files);
        free(dir);
    }
    key_index country = { 0 };             /* Country: JP on a place without a Parent: in Japan */
    for (size_t i = 0; i < *n; i++)
        if ((*out)[i].kind && !strcmp((*out)[i].kind, "country") && (*out)[i].country) index_put(&country, (*out)[i].country, i);
    for (size_t i = 0; i < *n; i++) {
        known_entry *e = &(*out)[i];
        if (!e->source || e->parent || !e->country || (e->kind && !strcmp(e->kind, "country"))) continue;
        long c = index_get(&country, e->country);
        if (c >= 0) e->parent = xstrdup((*out)[c].key);
    }
    index_free(&country);
    for (size_t d = 0; d < dirs.n; d++) {
        char *iso = xprintf("%s/iso-codes/json/iso_3166-2.json", dirs.v[d]);
        int here = !access(iso, R_OK);
        if (here) load_iso_regions(iso, out, n, keys);
        free(iso);
        if (here) break;
    }
    strlist_free(&dirs);
}

size_t known_load(const arv_home *h, known_entry **out)
{
    *out = NULL;
    size_t n = 0;
    char *p = vocab_file(h), *g = places_file(h);
    load_file(p, "Tag", out, &n);
    size_t tags = n;
    load_file(g, "Place", out, &n);
    key_index keys = { 0 };
    for (size_t i = 0; i < tags; i++) index_put(&keys, (*out)[i].key, i);
    size_t kept = tags;
    for (size_t i = tags; i < n; i++) {         /* a place in tags.rec too: tags.rec's is the one */
        if (index_get(&keys, (*out)[i].key) >= 0) {
            known_free_one(&(*out)[i]);
            continue;
        }
        (*out)[kept] = (*out)[i];
        index_put(&keys, (*out)[kept].key, kept);
        kept++;
    }
    n = kept;
    load_extra(out, &n, &keys);
    index_free(&keys);
    place_regions(*out, n);
    free(p);
    free(g);
    return n;
}

/* every place a tags file (tags.tsv) names, kept as known_keep keeps one: for a disc about to be made */
void known_keep_tags(const arv_home *h, const char *tags_tsv)
{
    char *text = tags_tsv ? read_text(tags_tsv) : NULL;
    if (!text || !strstr(text, "place:")) {
        free(text);
        return;
    }
    known_entry *v;
    size_t n = known_load(h, &v);
    for (char *t = text; (t = strstr(t, "place:")); t++) {
        size_t l = strcspn(t, ",\t\n ");
        char *key = xprintf("%.*s", (int)l, t);
        const known_entry *e = known_find(v, n, key);
        if (e && e->source) known_keep(h, v, n, key);
        free(key);
    }
    known_free(v, n);
    free(text);
}

/* an entry from extra data, copied into the home's places.rec when a tag first uses it (and the
 * places it is in, likewise), so the record and its discs never depend on data installed here */
int known_keep(const arv_home *h, const known_entry *v, size_t n, const char *key)
{
    const known_entry *e = known_find(v, n, key);
    int copied = 0;
    for (int depth = 0; e && e->source && depth < 16; depth++, e = known_find(v, n, e->parent)) {
        char today[11], *basis;
        today_iso(today);
        basis = xprintf("from %s, copied when first used, %s", e->source, today);
        known_field(h, e->key, "Label", e->label ? e->label : e->key + 6, 0);
        for (size_t i = 0; i < e->aliases.n; i++) known_field(h, e->key, "Alias", e->aliases.v[i], 1);
        if (e->qualifier) known_field(h, e->key, "Qualifier", e->qualifier, 0);
        if (e->kind) known_field(h, e->key, "Kind", e->kind, 0);
        if (e->parent) known_field(h, e->key, "Parent", e->parent, 0);
        if (e->country) known_field(h, e->key, "Country", e->country, 0);
        if (e->at) known_field(h, e->key, "At", e->at, 0);
        for (size_t i = 0; i < e->same.n; i++) known_field(h, e->key, "Same", e->same.v[i], 1);
        for (size_t i = 0; i < e->regions.n; i++) known_field(h, e->key, "Region", e->regions.v[i], 1);
        known_field(h, e->key, "Basis", basis, 0);
        free(basis);
        copied = 1;
    }
    return copied;
}

static void load_extra(known_entry **out, size_t *n, key_index *keys);
static void place_regions(known_entry *v, size_t n);

/* the gazetteer a catalogue goes with, home or disc: config/places.rec beside or inside it, else
 * the built-in one */
size_t known_places_of(const char *catalog_dir, known_entry **out)
{
    *out = NULL;
    size_t n = 0;
    char *in = xprintf("%s/config/places.rec", catalog_dir), *beside = xprintf("%s/../config/places.rec", catalog_dir);
    int home = access(in, F_OK) && !access(beside, F_OK);
    load_file(!access(in, F_OK) ? in : home ? beside : NULL, "Place", out, &n);
    if (home) {                                 /* a home: the extra data this machine has, as known_load reads it */
        key_index keys = { 0 };
        for (size_t i = 0; i < n; i++) index_put(&keys, (*out)[i].key, i);
        load_extra(out, &n, &keys);
        index_free(&keys);
    }
    place_regions(*out, n);                     /* a disc's: the regions it carries */
    free(in);
    free(beside);
    return n;
}

/* whether a place is key or within it (its parents, and theirs) */
int known_within(const known_entry *v, size_t n, const char *place, const char *key)
{
    const known_entry *e = known_find(v, n, place);
    for (int depth = 0; depth < 16; depth++) {
        if (!strcmp(place, key)) return 1;
        if (!e || !e->parent) return 0;
        place = e->parent;
        e = known_find(v, n, place);
    }
    return 0;
}

static const known_entry *last;                 /* an index of the list asked about last (known_find) */
static size_t last_n;
static key_index last_index;

const known_entry *known_find(const known_entry *v, size_t n, const char *key)
{
    key_index *xp = &last_index;
#define x (*xp)
    if (!key || !n) return NULL;
    if (n < 64) {
        for (size_t i = 0; i < n; i++)
            if (!strcmp(v[i].key, key)) return &v[i];
        return NULL;
    }
    if (v != last || n != last_n || !x.cap) {
        index_free(&x);
        for (size_t i = n; i-- > 0;) index_put(&x, v[i].key, i);   /* the first of a key wins */
        last = v;
        last_n = n;
    }
    long i = index_get(&x, key);
#undef x
    return i >= 0 && (size_t)i < n && !strcmp(v[i].key, key) ? &v[i] : NULL;
}

/* where a place is: its own position (2), the nearest broader place's (1), or none (0) */
int known_position(const known_entry *v, size_t n, const known_entry *e, double *lat, double *lon)
{
    for (int depth = 0; e && depth < 16; depth++, e = known_find(v, n, e->parent))
        if (e->has_at) {
            *lat = e->lat;
            *lon = e->lon;
            return depth ? 1 : 2;
        }
    return 0;
}

/* a place with the places it is in: "Fushimi Inari, Kyoto, Japan" */
char *known_where(const known_entry *v, size_t n, const known_entry *e)
{
    sbuf b = { 0 };
    char *prev = NULL;
    sb_puts(&b, "");
    for (int depth = 0; e && depth < 16; depth++, e = known_find(v, n, e->parent)) {
        char *l = e->label ? xstrdup(e->label) : known_label_of(e->key), *f = key_slug(l);
        if (!prev || strcmp(f, prev)) sb_printf(&b, "%s%s", depth ? ", " : "", l);   /* Kyoto in Kyoto, Ōsaka in Osaka: once */
        free(prev);
        free(l);
        prev = f;
    }
    free(prev);
    return b.s;
}

static void known_free_one(known_entry *e)
{
    free(e->key);
    free(e->label);
    free(e->qualifier);
    free(e->basis);
    free(e->retired);
    strlist_free(&e->aliases);
    strlist_free(&e->related);
    strlist_free(&e->same);
    strlist_free(&e->regions);
    free(e->kind);
    free(e->parent);
    free(e->country);
    free(e->at);
}

void known_free(known_entry *v, size_t n)
{
    if (v == last) {                            /* its index goes with it */
        index_free(&last_index);
        last = NULL;
    }
    for (size_t i = 0; i < n; i++) known_free_one(&v[i]);
    free(v);
}

/* "anne-smith" -> "Anne Smith": a label for a key that has none yet */
char *known_label_of(const char *key)
{
    const char *v = strchr(key, ':') ? strchr(key, ':') + 1 : key;
    char *s = xstrdup(v);
    for (char *c = s; *c; c++) {
        if (*c == '-' || *c == '_') *c = ' ';
        if (c == s || c[-1] == ' ') *c = (char)toupper((unsigned char)*c);
    }
    return s;
}

/* ------------------------------------------------------------------ editing tags.rec, comments kept */

/* the record whose Name is key: its Name line at *start, its end (a blank line, %rec or the end) */
static int block_of(const char *text, const char *key, size_t *start, size_t *end)
{
    char *line = xprintf("Name: %s\n", key);
    const char *at = NULL;
    for (const char *s = text; (s = strstr(s, line)); s++)
        if (s == text || s[-1] == '\n') {
            at = s;
            break;
        }
    free(line);
    if (!at) return 0;
    *start = (size_t)(at - text);
    const char *e = at;
    while (*e) {
        const char *nl = strchr(e, '\n');
        if (!nl) {
            e += strlen(e);
            break;
        }
        e = nl + 1;
        if (*e == '\n' || !strncmp(e, "%rec:", 5)) break;
    }
    *end = (size_t)(e - text);
    return 1;
}

static int save_vocab(const char *path, const char *text)
{
    char *tmp = xprintf("%s.tmp", path);
    write_text(tmp, text);
    int rc = rename(tmp, path);
    free(tmp);
    return rc;
}

/* a field of an entry: set (the first such line replaced, or one added), or added (once); a new
 * entry, before the Namespace records, when there is none */
int known_field(const arv_home *h, const char *key, const char *field, const char *value, int add)
{
    char *p = file_for(h, key), *text = read_text(p);
    if (!text) {
        free(p);
        return -1;
    }
    size_t start, end;
    sbuf b = { 0 };
    char *line = xprintf("%s: %s\n", field, value);
    for (char *c = line; c[1]; c++)
        if (*c == '\n') *c = ' ';               /* one line a field */
    if (!block_of(text, key, &start, &end)) {
        char *ns = strstr(text, "\n%rec: Namespace");
        size_t cut = ns ? (size_t)(ns - text) + 1 : strlen(text);
        sb_add(&b, text, cut);
        if (b.len && b.s[b.len - 1] != '\n') sb_puts(&b, "\n");
        if (b.len < 2 || b.s[b.len - 2] != '\n') sb_puts(&b, "\n");
        sb_printf(&b, "Name: %s\n%s", key, line);
        if (ns) sb_puts(&b, "\n");
        sb_puts(&b, text + cut);
    } else {
        char *block = xprintf("%.*s", (int)(end - start), text + start), *head = xprintf("%s: ", field);
        char *same = strstr(block, line), *at = NULL;
        for (char *s = block; (s = strstr(s, head)); s++)
            if (s == block || s[-1] == '\n') {
                at = s;
                break;
            }
        sb_add(&b, text, start);
        if (add && same && (same == block || same[-1] == '\n')) {
            sb_puts(&b, block);             /* there already */
        } else if (!add && at) {
            char *nl = strchr(at, '\n');
            sb_add(&b, block, (size_t)(at - block));
            sb_puts(&b, line);
            sb_puts(&b, nl ? nl + 1 : "");
        } else {
            sb_puts(&b, block);
            if (b.len && b.s[b.len - 1] != '\n') sb_puts(&b, "\n");
            sb_puts(&b, line);
        }
        sb_puts(&b, text + end);
        free(block);
        free(head);
    }
    int rc = save_vocab(p, b.s);
    free(line);
    free(b.s);
    free(text);
    free(p);
    return rc;
}

/* the vocabulary's text without the entries kept home (Private: yes), for a disc anyone may read */
char *known_strip_private(const char *text)
{
    sbuf b = { 0 };
    sb_puts(&b, "");
    for (const char *s = text; *s;) {          /* record by record: blank-line separated */
        const char *e = strstr(s, "\n\n");
        size_t n = e ? (size_t)(e - s) + 2 : strlen(s);
        char *rec = xprintf("%.*s", (int)n, s);
        int keep = 1;
        for (const char *q = rec; (q = strstr(q, "Private: yes")); q++)
            if ((q == rec || q[-1] == '\n') && (q[12] == '\n' || !q[12])) keep = 0;
        if (keep) sb_puts(&b, rec);
        free(rec);
        s += n;
    }
    return b.s;
}

/* ------------------------------------------------------------------ where an entry is used */

typedef struct {
    strlist discs, paths, tags;     /* a disc's path, and its tags now */
    strlist objects, otags;         /* a data object with tags of its own, and them */
} uses;

static int tags_have(const char *tags, const char *key)
{
    size_t n = strlen(key);
    for (const char *t = tags; (t = strstr(t, key)); t++)
        if ((t == tags || t[-1] == ' ' || t[-1] == ',') && (t[n] == ',' || !t[n])) return 1;
    return 0;
}

static void uses_of(const arv_home *h, const archive *cat, const char *key, uses *u)
{
    memset(u, 0, sizeof *u);
    for (size_t i = 0; i < cat->discs.n; i++) {
        const char *id = rec_get(cat->discs.v[i], "Id");
        if (!id) continue;
        char *tf = home_volume_file(h, id, "tags.tsv"), *text = disc_tags_text(cat, id, access(tf, F_OK) ? NULL : tf);
        free(tf);
        for (char *line = text, *next; line && *line; line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            char *tab = line[0] != '#' ? strchr(line, '\t') : NULL;
            if (!tab) continue;
            *tab = 0;
            char *end = strchr(tab + 1, '\t');
            if (end) *end = 0;
            if (!tags_have(tab + 1, key)) continue;
            const rec_record *o = object_at(cat, id, line);
            if (o && rec_get(o, "Uuid")) {      /* a data object's own path: the object's tags (as describe --apply) */
                const char *uuid = rec_get(o, "Uuid"), *own = object_field(cat, uuid, "Tags");
                if (!strlist_has(&u->objects, uuid)) {
                    strlist_add(&u->objects, uuid);
                    strlist_add(&u->otags, own ? own : tab + 1);
                }
                continue;
            }
            strlist_add(&u->discs, id);
            strlist_add(&u->paths, line);
            strlist_add(&u->tags, tab + 1);
        }
        free(text);
    }
    for (size_t i = 0; i < cat->objects.n; i++) {
        const char *uuid = rec_get(cat->objects.v[i], "Uuid"), *t = uuid ? object_field(cat, uuid, "Tags") : NULL;
        if (!t || strlist_has(&u->objects, uuid) || !tags_have(t, key)) continue;
        strlist_add(&u->objects, uuid);
        strlist_add(&u->otags, t);
    }
}

static void uses_free(uses *u)
{
    strlist_free(&u->discs);
    strlist_free(&u->paths);
    strlist_free(&u->tags);
    strlist_free(&u->objects);
    strlist_free(&u->otags);
}

/* tags with one replaced by another (none: removed), each once */
static char *tags_with(const char *tags, const char *from, const char *to)
{
    strlist out = { 0 };
    char *copy = xstrdup(tags);
    for (char *t = strtok(copy, ","); t; t = strtok(NULL, ",")) {
        while (*t == ' ') t++;
        char *e = t + strlen(t);
        while (e > t && e[-1] == ' ') *--e = 0;
        const char *v = !strcmp(t, from) ? to : t;
        if (v && *v && !strlist_has(&out, v)) strlist_add(&out, v);
    }
    free(copy);
    sbuf b = { 0 };
    sb_puts(&b, "");
    for (size_t i = 0; i < out.n; i++) sb_printf(&b, "%s%s", i ? ", " : "", out.v[i]);
    strlist_free(&out);
    return b.s;
}

/* ------------------------------------------------------------------ events */

static void name_event(archive *cat, const char *key, const char *note, const char *field, const char *value)
{
    char *who = person(), *thing = xprintf("name:%s", key);
    rec_record *e = new_event(key, "metadata modification", "success", who, "human", note);
    free(e->fields[0].name);                /* Object: name:KEY, not Disc */
    free(e->fields[0].value);
    e->fields[0].name = xstrdup("Object");
    e->fields[0].value = xstrdup(thing);
    if (field) {
        rec_add(e, field, value);
        sets_field(cat, e, thing, field);
    }
    recs_add(&cat->events, e);
    free(who);
    free(thing);
}

/* one use moved from one entry to another: a tag change, as arv retag or describe --apply makes */
static void move_use(archive *cat, const uses *u, size_t i, int object, const char *from, const char *to, const char *note)
{
    char *who = person();
    if (object) {
        char *now = tags_with(u->otags.v[i], from, to), *thing = xprintf("object:%s", u->objects.v[i]);
        rec_record *e = new_event(u->objects.v[i], "metadata modification", "success", who, "human", note);
        free(e->fields[0].name);
        free(e->fields[0].value);
        e->fields[0].name = xstrdup("Object");
        e->fields[0].value = xstrdup(thing);
        rec_add(e, "Tags", now);
        sets_field(cat, e, thing, "Tags");
        recs_add(&cat->events, e);
        free(now);
        free(thing);
    } else {
        char *now = tags_with(u->tags.v[i], from, to);
        rec_record *e = new_event(u->discs.v[i], "metadata modification", "success", who, "human", note);
        tag_change(cat, e, u->discs.v[i], "Tags", u->paths.v[i], now);
        recs_add(&cat->events, e);
        free(now);
    }
    free(who);
}

/* an entry made from an answer (arv describe --ask): its label, basis and, for people and families,
 * Private; the event goes into cat, for the caller to save */
void known_made(const arv_home *h, archive *cat, const char *key, const char *label, const char *basis)
{
    known_field(h, key, "Label", label, 0);
    known_field(h, key, "Basis", basis, 0);
    if (private_by_default(key)) known_field(h, key, "Private", "yes", 0);
    name_event(cat, key, basis, "Label", label);
}

/* ------------------------------------------------------------------ the commands */

static const known_entry *entry_of(const known_entry *v, size_t n, const char *key)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(v[i].key, key)) return &v[i];
    return NULL;
}

static char *line_in(const char *prompt)
{
    fputs(prompt, stderr);
    fflush(stderr);
    char *line = NULL;
    size_t cap = 0;
    ssize_t r = getline(&line, &cap, stdin);
    if (r < 0) {
        free(line);
        return xstrdup("");
    }
    while (r > 0 && (line[r - 1] == '\n' || line[r - 1] == '\r' || line[r - 1] == ' ')) line[--r] = 0;
    return line;
}

static void show_entry(const archive *cat, const known_entry *v, size_t n, const known_entry *e, const uses *u)
{
    printf("%s  %s%s%s%s\n", e->key, e->label ? e->label : "(no label)", e->qualifier ? " (" : "", e->qualifier ? e->qualifier : "",
           e->qualifier ? ")" : "");
    for (size_t i = 0; i < e->aliases.n; i++) printf("  also: %s\n", e->aliases.v[i]);
    for (size_t i = 0; i < e->related.n; i++) printf("  related: %s\n", e->related.v[i]);
    for (size_t i = 0; i < e->same.n; i++) printf("  same as: %s\n", e->same.v[i]);
    if (e->kind) printf("  kind: %s\n", e->kind);
    if (e->parent) {
        const known_entry *up = known_find(v, n, e->parent);
        char *w = up ? known_where(v, n, up) : NULL;
        printf("  in: %s%s%s%s\n", w ? w : "", w ? " (" : "", e->parent, w ? ")" : ", not in the gazetteer");
        free(w);
    }
    if (e->country) printf("  country: %s\n", e->country);
    if (e->at) printf("  at: %s\n", e->at);
    if (e->has_extent) printf("  covers: %.4f to %.4f north, %.4f to %.4f east\n", e->south, e->north, e->west, e->east);
    if (e->reach_km > 0) printf("  reach: %g km (a photo this near is here)\n", e->reach_km);
    if (e->basis) printf("  basis: %s\n", e->basis);
    printf("  %s\n", e->private_ ? "private: stays home, never on a public disc" : "on discs with the vocabulary");
    if (e->retired) printf("  RETIRED: %s\n", e->retired);
    printf("  used on %zu path%s and %zu data object%s\n", u->paths.n, u->paths.n == 1 ? "" : "s", u->objects.n,
           u->objects.n == 1 ? "" : "s");
    for (size_t i = 0; i < u->paths.n && i < 20; i++) printf("    %s  data/%s\n", u->discs.v[i], u->paths.v[i]);
    for (size_t i = 0; i < u->objects.n && i < 20; i++)
        printf("    object %s\n", object_slug(cat, object_at_uuid(cat, u->objects.v[i])));
}

static int known_ok(const char *key)
{
    const char *c = strchr(key, ':');
    if (!c || c == key || !c[1]) return 0;
    for (const char *s = key; *s; s++)
        if (isspace((unsigned char)*s) || *s == ',') return 0;
    return 1;
}

/* ------------------------------------------------------------------ likely the same */

/* a form as forms are compared: lower case, words one space apart ("A. Smith" -> "a smith") */
static char *plain_form(const char *s)
{
    sbuf b = { 0 };
    int gap = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p >= 0x80) {
            if (gap && b.len) sb_puts(&b, " ");
            char ch = (char)tolower(*p);
            sb_add(&b, &ch, 1);
            gap = 0;
        } else {
            gap = 1;
        }
    }
    return b.s ? b.s : xstrdup("");
}

static size_t edits(const char *a, const char *b)   /* Levenshtein, small strings */
{
    size_t n = strlen(a), m = strlen(b), *row = xmalloc((m + 1) * sizeof *row);
    for (size_t j = 0; j <= m; j++) row[j] = j;
    for (size_t i = 1; i <= n; i++) {
        size_t prev = row[0];
        row[0] = i;
        for (size_t j = 1; j <= m; j++) {
            size_t here = row[j], best = prev + (a[i - 1] != b[j - 1]);
            if (row[j] + 1 < best) best = row[j] + 1;
            if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
            row[j] = best;
            prev = here;
        }
    }
    size_t d = row[m];
    free(row);
    return d;
}

/* "a smith" and "anne smith": as many words, each the same or one an initial of the other */
static int initials_of(const char *a, const char *b)
{
    strlist x = { 0 }, y = { 0 };
    char *ca = xstrdup(a), *cb = xstrdup(b);
    for (char *t = strtok(ca, " "); t; t = strtok(NULL, " ")) strlist_add(&x, t);
    for (char *t = strtok(cb, " "); t; t = strtok(NULL, " ")) strlist_add(&y, t);
    int same = x.n == y.n && x.n > 1, initial = 0;
    for (size_t i = 0; same && i < x.n; i++) {
        if (!strcmp(x.v[i], y.v[i])) continue;
        int one = (strlen(x.v[i]) == 1 && y.v[i][0] == x.v[i][0]) || (strlen(y.v[i]) == 1 && x.v[i][0] == y.v[i][0]);
        same = one;
        initial |= one;
    }
    strlist_free(&x);
    strlist_free(&y);
    free(ca);
    free(cb);
    return same && initial;
}

static void forms_of(const known_entry *e, strlist *out)
{
    memset(out, 0, sizeof *out);
    char *f;
    if (!e->label) {                        /* the key only when it has no label: person:anne-smith-2 is a key, not a name */
        f = plain_form(strchr(e->key, ':') + 1);
        strlist_add(out, f);
        free(f);
    } else {
        f = plain_form(e->label);
        if (!strlist_has(out, f)) strlist_add(out, f);
        free(f);
    }
    for (size_t i = 0; i < e->aliases.n; i++) {
        if (strchr(e->aliases.v[i], ':')) continue;
        f = plain_form(e->aliases.v[i]);
        if (*f && !strlist_has(out, f)) strlist_add(out, f);
        free(f);
    }
}

/* why two entries are likely one (NULL: they are not): the same namespace, neither told apart on
 * purpose (a qualifier), and a form in common, one or two letters apart, or initials */
static char *likely_same(const known_entry *a, const known_entry *b)
{
    size_t na = (size_t)(strchr(a->key, ':') - a->key);
    if (strncmp(a->key, b->key, na + 1) || a->retired || b->retired) return NULL;
    if (a->qualifier || b->qualifier) return NULL;
    strlist fa, fb;
    forms_of(a, &fa);
    forms_of(b, &fb);
    char *why = NULL;
    for (size_t i = 0; i < fa.n && !why; i++)
        for (size_t j = 0; j < fb.n && !why; j++) {
            const char *x = fa.v[i], *y = fb.v[j];
            size_t lx = strlen(x), ly = strlen(y), d = lx < 4 || ly < 4 ? 0 : edits(x, y);
            if (!strcmp(x, y)) why = xprintf("both found as \"%s\"", x);
            else if (d && d <= (lx >= 8 && ly >= 8 ? 2u : 1u)) why = xprintf("\"%s\" and \"%s\": %zu letter%s apart", x, y, d, d == 1 ? "" : "s");
            else if (initials_of(x, y)) why = xprintf("\"%s\" and \"%s\": initials", x, y);
        }
    strlist_free(&fa);
    strlist_free(&fb);
    return why;
}

/* arv tags names [--json]: the entries made here (the vocabulary's, and places of your own: the
 * gazetteer's own, which no one made here, aside), with their uses */
static int known_names(const char *given, int as_json)
{
    arv_home h;
    archive cat;
    home_find(&h, given, NULL);
    archive_load(&cat, h.rec_path);
    known_entry *v;
    size_t n = known_load(&h, &v);
    sbuf b = { 0 };
    sb_puts(&b, as_json ? "{" JSON_FORMAT ", \"names\": [" : "");
    size_t shown = 0;
    for (size_t i = 0; i < n; i++) {
        const known_entry *e = &v[i];
        if (e->source || (!e->basis && e->kind && strcmp(e->kind, "site"))) continue;   /* the gazetteer's and extra data's own */
        uses u;
        uses_of(&h, &cat, e->key, &u);
        if (as_json) {
            sb_puts(&b, shown ? ",\n  {" : "\n  {");
            sb_puts(&b, "\"key\": "); json_str(&b, e->key);
            sb_puts(&b, ", \"label\": "); json_str(&b, e->label ? e->label : "");
            sb_puts(&b, ", \"qualifier\": "); json_str(&b, e->qualifier ? e->qualifier : "");
            sb_puts(&b, ", \"aliases\": [");
            for (size_t k = 0; k < e->aliases.n; k++) {
                if (k) sb_puts(&b, ", ");
                json_str(&b, e->aliases.v[k]);
            }
            sb_puts(&b, "], \"basis\": "); json_str(&b, e->basis ? e->basis : "");
            sb_puts(&b, ", \"retired\": "); json_str(&b, e->retired ? e->retired : "");
            sb_puts(&b, ", \"parent\": "); json_str(&b, e->parent ? e->parent : "");
            sb_printf(&b, ", \"private\": %s, \"paths\": %zu, \"objects\": %zu}", e->private_ ? "true" : "false", u.paths.n, u.objects.n);
        } else {
            sb_printf(&b, "%-28s %s%s%s%s  (%zu path%s, %zu object%s)%s%s\n", e->key, e->label ? e->label : "(no label)",
                      e->qualifier ? " (" : "", e->qualifier ? e->qualifier : "", e->qualifier ? ")" : "", u.paths.n,
                      u.paths.n == 1 ? "" : "s", u.objects.n, u.objects.n == 1 ? "" : "s", e->private_ ? " private" : "",
                      e->retired ? " RETIRED" : "");
        }
        shown++;
        uses_free(&u);
    }
    size_t pairs = 0;                       /* likely the same: offered for merging, never merged here */
    sb_puts(&b, as_json ? "\n], \"likelySame\": [" : "");
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++) {
            if ((!v[i].basis && v[i].kind) || (!v[j].basis && v[j].kind)) continue;   /* the gazetteer's own aside */
            char *why = likely_same(&v[i], &v[j]);
            if (!why) continue;
            if (as_json) {
                sb_puts(&b, pairs ? ",\n  {\"keys\": [" : "\n  {\"keys\": [");
                json_str(&b, v[i].key);
                sb_puts(&b, ", ");
                json_str(&b, v[j].key);
                sb_puts(&b, "], \"why\": ");
                json_str(&b, why);
                sb_puts(&b, "}");
            } else {
                if (!pairs) sb_puts(&b, "\nLikely the same (arv tags merge KEY INTO; two people of one name: arv tags add KEY --qualifier Q):\n");
                sb_printf(&b, "  %s and %s: %s\n", v[i].key, v[j].key, why);
            }
            pairs++;
            free(why);
        }
    if (as_json) sb_puts(&b, "\n]}\n");
    else if (!shown) sb_puts(&b, "No known names yet: arv describe --ask makes them as you confirm names (arv tags add KEY makes one).\n");
    fputs(b.s ? b.s : "", stdout);
    free(b.s);
    known_free(v, n);
    return 0;
}

int known_cmd(const char *sub, int argc, char **argv, const char *given)
{
    const char *label = NULL, *qual = NULL, *new_key = NULL, *new_qual = NULL, *why = NULL;
    const char *parent = NULL, *pos = NULL, *kind = NULL, *country = NULL, *reach = NULL, *extent = NULL;   /* a place's */
    strlist words = { 0 }, aliases = { 0 }, same = { 0 };
    int priv = -1, as_json = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (v && !strcmp(a, "--label")) label = argv[++i];
        else if (v && !strcmp(a, "--alias")) strlist_add(&aliases, argv[++i]);
        else if (v && !strcmp(a, "--qualifier")) qual = argv[++i];
        else if (v && !strcmp(a, "--same")) strlist_add(&same, argv[++i]);
        else if (v && !strcmp(a, "--new")) new_key = argv[++i];
        else if (v && !strcmp(a, "--new-qualifier")) new_qual = argv[++i];
        else if (v && !strcmp(a, "--why")) why = argv[++i];
        else if (v && !strcmp(a, "--parent")) parent = argv[++i];
        else if (v && !strcmp(a, "--at")) pos = argv[++i];
        else if (v && !strcmp(a, "--kind")) kind = argv[++i];
        else if (v && !strcmp(a, "--country")) country = argv[++i];
        else if (v && !strcmp(a, "--reach")) reach = argv[++i];
        else if (v && !strcmp(a, "--extent")) extent = argv[++i];
        else if (!strcmp(a, "--json")) as_json = 1;
        else if (!strcmp(a, "--private")) priv = 1;
        else if (!strcmp(a, "--public")) priv = 0;
        else if (a[0] == '-') return 2;
        else strlist_add(&words, a);
    }
    if (!strcmp(sub, "names")) {
        if (words.n) return 2;
        int rc = known_names(given, as_json);
        strlist_free(&aliases);
        strlist_free(&same);
        return rc;
    }
    if (!words.n) return 2;
    char *key = tag_normalise(words.v[0]);
    if (!known_ok(key)) die("%s: an entry's key is a namespaced tag, NAMESPACE:value (person:anne-smith, software:kicad)", words.v[0]);
    int place = !strncmp(key, "place:", 6);
    if (!place && (parent || pos || kind || country || reach || extent))
        die("%s", "--parent, --at, --kind, --country, --reach and --extent are a place's (place:KEY)");
    char *box = NULL, *far = NULL;
    if (extent) {
        double s, w, no, ea;
        if (extent_parse(extent, &s, &w, &no, &ea))
            die("%s: an extent is two opposite corners, ISO 6709: +34.96+135.76/+34.98+135.78/ (or '34.96,135.76 34.98,135.78')", extent);
        char *sw = iso6709(s, w), *ne = iso6709(no, ea);
        box = xprintf("%s%s", sw, ne);
        free(sw);
        free(ne);
    }
    if (reach) {
        double r = reach_parse(reach);
        if (r <= 0) die("%s: a reach is a distance, 2km or 500m", reach);
        far = r < 1 ? xprintf("%gm", r * 1000) : xprintf("%gkm", r);
    }
    char *at = NULL, *in = parent ? tag_normalise(parent) : NULL;
    if (in && strncmp(in, "place:", 6)) {       /* --parent kyoto: a place's key */
        char *full = xprintf("place:%s", in);
        free(in);
        in = full;
    }
    if (pos) {
        double la, lo;
        if (iso6709_parse(pos, &la, &lo)) die("%s: a position is ISO 6709 in decimal degrees, +35.03+135.75/ (or 35.03,135.75)", pos);
        at = iso6709(la, lo);
    }
    arv_home h;
    archive cat;
    home_find(&h, given, NULL);
    archive_load(&cat, h.rec_path);
    known_entry *v;
    size_t n = known_load(&h, &v);
    const known_entry *e = entry_of(v, n, key);
    char today[11], *who = person();
    today_iso(today);
    uses u;
    uses_of(&h, &cat, key, &u);
    int rc = 0, saved = 0;
    if (!strcmp(sub, "show") && as_json) {   /* its uses in the order split asks about them */
        sbuf b = { 0 };
        sb_puts(&b, "{" JSON_FORMAT ", \"key\": ");
        json_str(&b, key);
        sb_puts(&b, ", \"entry\": ");
        sb_puts(&b, e ? "true" : "false");
        sb_puts(&b, ", \"label\": "); json_str(&b, e && e->label ? e->label : "");
        sb_puts(&b, ", \"qualifier\": "); json_str(&b, e && e->qualifier ? e->qualifier : "");
        sb_puts(&b, ", \"uses\": [");
        for (size_t i = 0; i < u.paths.n + u.objects.n; i++) {
            int object = i >= u.paths.n;
            size_t k = object ? i - u.paths.n : i;
            sb_puts(&b, i ? ",\n  {" : "\n  {");
            if (object) {
                sb_puts(&b, "\"object\": "); json_str(&b, u.objects.v[k]);
                sb_puts(&b, ", \"name\": "); json_str(&b, object_slug(&cat, object_at_uuid(&cat, u.objects.v[k])));
                sb_puts(&b, ", \"tags\": "); json_str(&b, u.otags.v[k]);
            } else {
                sb_puts(&b, "\"disc\": "); json_str(&b, u.discs.v[k]);
                sb_puts(&b, ", \"path\": "); json_str(&b, u.paths.v[k]);
                sb_puts(&b, ", \"tags\": "); json_str(&b, u.tags.v[k]);
            }
            sb_puts(&b, "}");
        }
        sb_puts(&b, "\n]}\n");
        fputs(b.s, stdout);
        free(b.s);
    } else if (!strcmp(sub, "show")) {
        if (!e && !u.paths.n && !u.objects.n) die("no entry %s, and no tag of it in use (arv tags lists them)", key);
        known_entry blank = { 0 };
        blank.key = key;
        show_entry(&cat, v, n, e ? e : &blank, &u);
        if (!e) printf("  not an entry yet: arv tags add %s --label \"...\"\n", key);
    } else if (!strcmp(sub, "add")) {
        if (words.n > 1) return 2;
        if (!e) {
            char *lb = label ? xstrdup(label) : known_label_of(key), *basis = xprintf("made by %s with arv tags add, %s", who, today);
            known_field(&h, key, "Label", lb, 0);
            known_field(&h, key, "Basis", basis, 0);
            int hide = priv == 1 || (priv < 0 && (private_by_default(key) || at || box));   /* a place to the metre: someone's home, perhaps */
            if (hide) known_field(&h, key, "Private", "yes", 0);
            if (place) known_field(&h, key, "Kind", kind ? kind : "site", 0);
            name_event(&cat, key, "an entry of the known names, made", "Label", lb);
            printf("%s: an entry now, %s%s\n", key, lb, hide ? " (private: stays home)" : "");
            free(lb);
            free(basis);
        } else if (label) {
            known_field(&h, key, "Label", label, 0);
            name_event(&cat, key, "its label", "Label", label);
        }
        if (e && priv >= 0) known_field(&h, key, "Private", priv ? "yes" : "no", 0);
        for (size_t i = 0; i < aliases.n; i++) known_field(&h, key, "Alias", aliases.v[i], 1);
        for (size_t i = 0; i < same.n; i++) known_field(&h, key, "Same", same.v[i], 1);
        if (qual) known_field(&h, key, "Qualifier", qual, 0);
        if (e && kind) known_field(&h, key, "Kind", kind, 0);
        if (in) {
            if (!entry_of(v, n, in)) fprintf(stderr, "(%s is not in the gazetteer yet: arv tags add %s)\n", in, in);
            else known_keep(&h, v, n, in);      /* a region from iso-codes, say: copied home */
            known_field(&h, key, "Parent", in, 0);
            name_event(&cat, key, "the place it is in", "Parent", in);
        }
        if (country) known_field(&h, key, "Country", country, 0);
        if (at) known_field(&h, key, "At", at, 0);   /* the gazetteer's, never an event: the record keeps no coordinates */
        if (box) known_field(&h, key, "Extent", box, 0);
        if (far) known_field(&h, key, "Reach", far, 0);
        if (e) printf("%s: updated\n", key);
        saved = 1;
    } else if (!strcmp(sub, "label")) {
        if (words.n != 2) return 2;
        if (e && e->label && strcmp(e->label, words.v[1])) known_field(&h, key, "Alias", e->label, 1);   /* the old form: a variant */
        known_field(&h, key, "Label", words.v[1], 0);
        name_event(&cat, key, e && e->label ? "its preferred form, the old one kept as an alias" : "its label", "Label", words.v[1]);
        if (e && e->label) printf("%s: %s (was %s, kept as an alias)\n", key, words.v[1], e->label);
        else printf("%s: %s\n", key, words.v[1]);
        saved = 1;
    } else if (!strcmp(sub, "alias")) {
        if (words.n != 2) return 2;
        known_field(&h, key, "Alias", words.v[1], 1);
        name_event(&cat, key, "another form it is found as", "Alias", words.v[1]);
        printf("%s: also found as %s\n", key, words.v[1]);
        saved = 1;
    } else if (!strcmp(sub, "retire")) {
        if (words.n != 1) return 2;
        if (!e) die2("no entry %s (arv tags show %s)", key, key);
        char *note = xprintf("retired: %s", why ? why : "by a person");
        known_field(&h, key, "Retired", why ? why : "yes", 0);
        name_event(&cat, key, note, "Retired", why ? why : "yes");
        printf("%s: retired, kept (no longer offered; its %zu uses keep it)\n", key, u.paths.n + u.objects.n);
        free(note);
        saved = 1;
    } else if (!strcmp(sub, "merge")) {
        if (words.n != 2) return 2;
        char *into = tag_normalise(words.v[1]);
        if (!known_ok(into) || !strcmp(into, key)) die("%s: merge an entry into another one (arv tags merge KEY INTO)", words.v[1]);
        const known_entry *t = entry_of(v, n, into);
        if (!t) {
            char *lb = known_label_of(into), *basis = xprintf("made by %s merging %s, %s", who, key, today);
            known_field(&h, into, "Label", lb, 0);
            known_field(&h, into, "Basis", basis, 0);
            if (private_by_default(into)) known_field(&h, into, "Private", "yes", 0);
            free(lb);
            free(basis);
        }
        char *note = xprintf("%s merged into %s", key, into);
        for (size_t i = 0; i < u.paths.n; i++) move_use(&cat, &u, i, 0, key, into, note);
        for (size_t i = 0; i < u.objects.n; i++) move_use(&cat, &u, i, 1, key, into, note);
        known_field(&h, into, "Alias", key, 1);   /* typed or found later, it means the entry merged into */
        if (e && e->label) known_field(&h, into, "Alias", e->label, 1);
        for (size_t i = 0; e && i < e->aliases.n; i++) known_field(&h, into, "Alias", e->aliases.v[i], 1);
        char *ret = xprintf("merged into %s", into);
        known_field(&h, key, "Retired", ret, 0);
        name_event(&cat, key, note, "Retired", ret);
        name_event(&cat, into, note, NULL, NULL);
        printf("%s merged into %s: %zu use%s moved (an event each); %s kept, retired, and an alias of %s\n", key, into,
               u.paths.n + u.objects.n, u.paths.n + u.objects.n == 1 ? "" : "s", key, into);
        free(ret);
        free(note);
        free(into);
        saved = 1;
    } else if (!strcmp(sub, "split")) {
        if (words.n != 1) return 2;
        if (!e && !u.paths.n && !u.objects.n) die("no entry %s, and no tag of it in use", key);
        char *two = new_key ? tag_normalise(new_key) : NULL;
        for (int k = 2; !two; k++) {        /* person:anne-smith-2, or the next free */
            char *c = xprintf("%s-%d", key, k);
            if (!entry_of(v, n, c)) two = c;
            else free(c);
        }
        if (!known_ok(two) || entry_of(v, n, two)) die("%s: the new entry's key must be a new namespaced tag", two);
        char *lb = e && e->label ? xstrdup(e->label) : known_label_of(key);
        char *q1 = qual ? xstrdup(qual) : NULL, *q2 = new_qual ? xstrdup(new_qual) : NULL;
        if (!q1) q1 = line_in("What tells this one apart (its Qualifier: dates, a relation, a place)? ");
        if (!q2) q2 = line_in("And the other one? ");
        if (!*q1 || !*q2) die("%s", "two of one name need a qualifier each, to tell them apart");
        char *basis = xprintf("split from %s by %s, %s", key, who, today);
        known_field(&h, key, "Qualifier", q1, 0);
        known_field(&h, two, "Label", lb, 0);
        known_field(&h, two, "Qualifier", q2, 0);
        known_field(&h, two, "Basis", basis, 0);
        if ((e && e->private_) || (!e && private_by_default(two))) known_field(&h, two, "Private", "yes", 0);
        char *note = xprintf("%s split: this use is %s (%s)", key, two, q2);
        size_t moved = 0, asked = 0, total = u.paths.n + u.objects.n;
        fprintf(stderr, "\n%zu use%s of %s (%s). For each: 1 %s (%s), 2 %s (%s); Enter keeps it 1, '.' stops (the rest stay 1)\n",
                total, total == 1 ? "" : "s", key, lb, key, q1, two, q2);
        int stop = 0;
        for (size_t i = 0; i < total && !stop; i++) {
            int object = i >= u.paths.n;
            size_t k = object ? i - u.paths.n : i;
            if (object) fprintf(stderr, "\nobject %s, tags %s\n", object_slug(&cat, object_at_uuid(&cat, u.objects.v[k])), u.otags.v[k]);
            else fprintf(stderr, "\n%s  data/%s, tags %s\n", u.discs.v[k], u.paths.v[k], u.tags.v[k]);
            char *a = line_in("> ");
            asked++;
            if (!strcmp(a, ".")) stop = 1;
            else if (!strcmp(a, "2")) {
                move_use(&cat, &u, k, object, key, two, note);
                moved++;
            }
            free(a);
        }
        name_event(&cat, key, note, "Qualifier", q1);
        name_event(&cat, two, basis, "Label", lb);
        printf("%s split: %s (%s) and %s (%s); %zu of %zu use%s moved to %s (an event each), %zu not asked about\n", key, key, q1,
               two, q2, moved, total, total == 1 ? "" : "s", two, total - asked);
        printf("Two entries share the form %s now: describe --ask asks which one, never takes it alone.\n", lb);
        free(note);
        free(basis);
        free(lb);
        free(q1);
        free(q2);
        free(two);
        saved = 1;
    } else {
        rc = 2;
    }
    if (saved) archive_save(&cat, h.rec_path);
    uses_free(&u);
    known_free(v, n);
    strlist_free(&words);
    strlist_free(&aliases);
    strlist_free(&same);
    free(who);
    free(key);
    free(at);
    free(in);
    free(box);
    free(far);
    return rc;
}
