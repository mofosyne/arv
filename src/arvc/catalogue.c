/* Reading a catalogue: find, list, id. */
#define _XOPEN_SOURCE 700
#include "arvc.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ the catalogue */


int is_file(const char *dir, const char *name)
{
    struct stat st;
    char *p = join(dir, name);
    int r = !stat(p, &st) && S_ISREG(st.st_mode);
    free(p);
    return r;
}

/* the catalog folder in `path` (a catalog/ folder, a disc root or a home), or NULL */
char *catalogue_in(const char *path)
{
    if (is_file(path, "archive.rec")) return xstrdup(path);
    char *c = join(path, "catalog");
    if (is_file(c, "archive.rec")) return c;
    free(c);
    return NULL;
}

/* Finds and reads the catalogue; without `required`, returns -1 (an empty catalogue) when there
 * is none instead of stopping. */
int find_catalogue(const char *given, catalogue *c, int required)
{
    int bad = 0;
    arv_home h;
    home_find(&h, given, NULL);
    c->dir = h.catalog_dir;
    char *path = join(c->dir, "archive.rec");
    if (!is_file(c->dir, "archive.rec")) {      /* an empty home, as the Python arv reads it */
        memset(&c->rec, 0, sizeof c->rec);
        free(path);
        (void)required;
        return -1;
    }
    if (rec_read(path, &c->rec, &bad)) {
        fprintf(stderr, "arvc: cannot read %s%s\n", path, errno == EINVAL ? " (a line is not a field)" : "");
        exit(2);
    }
    free(path);
    return 0;
}

void open_catalogue(const char *given, catalogue *c)
{
    find_catalogue(given, c, 1);
}

int is_type(const rec_record *r, const char *type)
{
    return !r->descriptor && r->type && !strcmp(r->type, type);
}

const rec_record *location(const catalogue *c, const char *code)
{
    if (!code) return NULL;
    char *want = upper_trim(code);
    const rec_record *found = NULL;
    for (size_t i = 0; !found && i < c->rec.nrecords; i++)
        if (is_type(&c->rec.records[i], "Location")) {
            const char *k = rec_get(&c->rec.records[i], "Code");
            if (k && !strcmp(k, want)) found = &c->rec.records[i];
        }
    free(want);
    return found;
}

/* "Home / Study / Box 3" for a Location code; free text as it is. Appends to out. */
void location_path(const catalogue *c, const char *code, char **out, size_t *len)
{
    const rec_record *chain[32];
    size_t n = 0;
    for (const rec_record *l = location(c, code); l && n < 32; l = location(c, rec_get(l, "Parent"))) {
        size_t k;
        for (k = 0; k < n && chain[k] != l; k++) {}
        if (k < n) break;                       /* a cycle */
        chain[n++] = l;
    }
    if (!n) {
        size_t add = strlen(code);
        *out = xrealloc(*out, *len + add + 1);
        memcpy(*out + *len, code, add + 1);
        *len += add;
        return;
    }
    while (n--) {
        const char *name = rec_get(chain[n], "Name");
        if (!name) name = rec_get(chain[n], "Code");
        size_t add = strlen(name) + 3;
        *out = xrealloc(*out, *len + add + 1);
        sprintf(*out + *len, "%s%s", name, n ? " / " : "");
        *len += strlen(*out + *len);
    }
}

/* every place a disc's copies are kept, "; " between them ("" when not recorded) */
char *where(const catalogue *c, const rec_record *d)
{
    char *out = xstrdup("");
    size_t len = 0;
    for (size_t i = 0; i < d->nfields; i++) {
        if (strcmp(d->fields[i].name, "Location")) continue;
        if (len) {
            out = xrealloc(out, len + 3);
            strcpy(out + len, "; ");
            len += 2;
        }
        location_path(c, d->fields[i].value, &out, &len);
    }
    return out;
}

char *lower(const char *s)
{
    char *p = xstrdup(s);
    for (char *q = p; *q; q++) *q = (char)tolower((unsigned char)*q);
    return p;
}

/* substring, or a glob when the pattern has * ? [; ASCII letters match either case */
int matches(const char *pattern_lower, int glob, const char *text)
{
    char *t = lower(text);
    int r = glob ? !fnmatch(pattern_lower, t, 0) : strstr(t, pattern_lower) != NULL;
    free(t);
    return r;
}

const char *access_of(const rec_record *d)
{
    const char *a = rec_get(d, "Access");
    static char buf[16];
    if (!a) return "private";
    size_t n = 0;
    while (*a == ' ') a++;
    for (; *a && *a != ' ' && n < sizeof buf - 1; a++) buf[n++] = (char)tolower((unsigned char)*a);
    buf[n] = 0;
    return !strcmp(buf, "public") || !strcmp(buf, "sealed") || !strcmp(buf, "private") ? buf : "private";
}

char *catalogue_volume(const catalogue *c, const char *id, const char *name)
{
    char *dir = join(c->dir, "volumes"), *vol = join(dir, id), *p = join(vol, name);
    free(dir);
    free(vol);
    return p;
}

static const char *const SEARCH_FIELDS[] = { "Id", "Title", "Description", "Subject", "Note", "Coverage",
                                            "Category", "Path", "Location", NULL };

int cmd_find(int argc, char **argv)
{
    const char *given = NULL, *pattern = NULL;
    long limit = 200;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-C") && i + 1 < argc) given = argv[++i];
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = atol(argv[++i]);
        else if (!pattern) pattern = argv[i];
        else return 2;
    }
    if (!pattern) return 2;
    catalogue c;
    open_catalogue(given, &c);
    char *pat = lower(pattern);
    int glob = strpbrk(pat, "*?[") != NULL, any = 0;
    long files = 0;

    for (size_t i = 0; i < c.rec.nrecords; i++) {          /* discs whose description matches */
        const rec_record *d = &c.rec.records[i];
        if (!is_type(d, "Disc")) continue;
        int hit = 0;
        for (size_t j = 0; j < d->nfields && !hit; j++)
            for (int k = 0; SEARCH_FIELDS[k] && !hit; k++)
                if (!strcmp(d->fields[j].name, SEARCH_FIELDS[k]) && matches(pat, glob, d->fields[j].value)) hit = 1;
        if (hit) {
            char *w = where(&c, d);
            printf("DISC  %s  %s  [%s]\n", rec_get(d, "Id"), rec_get(d, "Title") ? rec_get(d, "Title") : "None",
                   *w ? w : "location unknown");
            free(w);
            any = 1;
        }
    }
    for (size_t i = 0; i < c.rec.nrecords; i++) {          /* folder tags and captions */
        const rec_record *d = &c.rec.records[i];
        if (!is_type(d, "Disc") || !rec_get(d, "Id")) continue;
        char *path = catalogue_volume(&c, rec_get(d, "Id"), "tags.tsv"), *line = NULL;
        size_t cap = 0;
        ssize_t len;
        FILE *fp = fopen(path, "r");
        while (fp && (len = getline(&line, &cap, fp)) >= 0) {
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
            char *cols[3] = { line, NULL, NULL };
            if (line[0] == '#' || split_tabs(line, cols, 3) < 2) continue;
            const char *caption = cols[2] ? cols[2] : "";
            while (*caption == ' ') caption++;
            int cap_hit = *caption && matches(pat, glob, caption), hit = cap_hit;
            char *tags = xstrdup(cols[1]), shown[4096] = "";
            for (char *t = strtok(tags, ","); t; t = strtok(NULL, ",")) {
                while (*t == ' ') t++;
                char *e = t + strlen(t);
                while (e > t && e[-1] == ' ') *--e = 0;
                if (!*t) continue;
                if (matches(pat, glob, t)) hit = 1;
                if (*shown) strncat(shown, ", ", sizeof shown - strlen(shown) - 1);
                strncat(shown, t, sizeof shown - strlen(shown) - 1);
            }
            if (cap_hit) {
                if (*shown) strncat(shown, ", ", sizeof shown - strlen(shown) - 1);
                strncat(shown, caption, sizeof shown - strlen(shown) - 1);
            }
            free(tags);
            if (!hit) continue;
            char *w = where(&c, d);
            if (!strcmp(cols[0], ".")) printf("TAG   %s  [%s]  data/  (%s)\n", rec_get(d, "Id"), *w ? w : "?", shown);
            else printf("TAG   %s  [%s]  data/%s/  (%s)\n", rec_get(d, "Id"), *w ? w : "?", cols[0], shown);
            free(w);
            any = 1;
        }
        if (fp) fclose(fp);
        free(line);
        free(path);
    }
    for (size_t i = 0; i < c.rec.nrecords; i++) {          /* files, from each disc's manifest */
        const rec_record *d = &c.rec.records[i];
        if (!is_type(d, "Disc") || !rec_get(d, "Id")) continue;
        char *path = catalogue_volume(&c, rec_get(d, "Id"), "manifest.sha256"), *line = NULL;
        size_t cap = 0;
        ssize_t len;
        FILE *fp = fopen(path, "r");
        char *w = where(&c, d);
        while (fp && (len = getline(&line, &cap, fp)) >= 0) {
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
            char *rel = strstr(line, "  ");
            if (!rel || !matches(pat, glob, rel + 2)) continue;
            if (!limit || files < limit) printf("%s  [%s]  %s\n", rec_get(d, "Id"), *w ? w : "?", rel + 2);
            files++;
            any = 1;
        }
        free(w);
        if (fp) fclose(fp);
        free(line);
        free(path);
    }
    if (limit && files > limit) printf("... %ld more file matches (use --limit 0 for all)\n", files - limit);
    free(pat);
    return any ? 0 : 1;
}

const rec_record *find_disc(const catalogue *c, const char *id)
{
    for (size_t i = 0; i < c->rec.nrecords; i++)
        if (is_type(&c->rec.records[i], "Disc") && rec_get(&c->rec.records[i], "Id")
            && !strcmp(rec_get(&c->rec.records[i], "Id"), id))
            return &c->rec.records[i];
    return NULL;
}

/* Explain a disc id: its parts, whether the check character is right, and what it names. */
int cmd_id(int argc, char **argv)
{
    const char *given = NULL, *text = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-C") && i + 1 < argc) given = argv[++i];
        else if (!text) text = argv[i];
        else return 2;
    }
    if (!text) return 2;
    discid_parts p;
    if (!discid_parse(text, &p)) {
        printf("%s: not a disc id of a known scheme\n", text);
        return 1;
    }
    printf("scheme:    %s\nset:       %s\nsequence:  %ld\ncoverage:  %s\n", p.scheme, p.set, p.sequence, p.coverage);
    if (p.check) printf("check:     %c (%s)\n", p.check, p.valid ? "correct" : "WRONG: probably a typo");
    catalogue c;
    find_catalogue(given, &c, 0);
    const char *t;
    size_t len;
    for (t = text; isspace((unsigned char)*t); t++) {}
    len = strlen(t);
    while (len && isspace((unsigned char)t[len - 1])) len--;
    char *exact = xmalloc(len + 1), *up = xmalloc(len + 1);
    memcpy(exact, t, len);
    exact[len] = 0;
    for (size_t i = 0; i <= len; i++) up[i] = (char)toupper((unsigned char)exact[i]);
    const rec_record *d = find_disc(&c, up);
    if (!d) d = find_disc(&c, exact);
    if (d) {
        char *w = where(&c, d);
        printf("disc:      %s [%s]\n", rec_get(d, "Title") ? rec_get(d, "Title") : "None", *w ? w : "location not recorded");
        free(w);
        const char *scheme = rec_get(d, "IdScheme"), *seq = rec_get(d, "Sequence");
        char again[64];
        if (scheme && !strcmp(scheme, DISCID_SCHEME) && seq
            && !discid_compose(rec_get(d, "Set"), atol(seq), rec_get(d, "Coverage"), again, sizeof again)) {
            if (!strcmp(again, rec_get(d, "Id"))) printf("fields:    regenerate this id\n");
            else printf("fields:    regenerate %s (the record and the id disagree)\n", again);
        }
    } else {
        /* known ids of the same length within two characters, closest first */
        const char *best[64];
        int diffs[64], n = 0;
        for (size_t i = 0; i < c.rec.nrecords && n < 64; i++) {
            const rec_record *r = &c.rec.records[i];
            const char *k = is_type(r, "Disc") ? rec_get(r, "Id") : NULL;
            if (!k || strlen(k) != len) continue;
            int diff = 0;
            for (size_t j = 0; j < len; j++) diff += toupper((unsigned char)k[j]) != (unsigned char)up[j];
            if (diff > 2) continue;
            int at = n++;
            while (at > 0 && (diffs[at - 1] > diff || (diffs[at - 1] == diff && strcmp(best[at - 1], k) > 0))) {
                best[at] = best[at - 1];
                diffs[at] = diffs[at - 1];
                at--;
            }
            best[at] = k;
            diffs[at] = diff;
        }
        printf("disc:      not in the catalogue");
        for (int i = 0; i < n; i++) printf("%s%s", i ? ", " : " - did you mean ", best[i]);
        printf("%s\n", n ? "?" : "");
    }
    free(exact);
    free(up);
    return p.valid ? 0 : 1;
}

/* every code a disc belongs to: Set, Category and each part of each Path */
int disc_in(const rec_record *d, const char *code)
{
    for (size_t i = 0; i < d->nfields; i++) {
        const char *n = d->fields[i].name, *v = d->fields[i].value;
        if (!strcmp(n, "Set") || !strcmp(n, "Category")) {
            if (!strcmp(v, code)) return 1;
        } else if (!strcmp(n, "Path")) {
            size_t k = strlen(code);
            for (const char *p = v; *p;) {
                const char *e = strchr(p, '/');
                size_t len = e ? (size_t)(e - p) : strlen(p);
                if (len == k && !strncmp(p, code, k)) return 1;
                p += len + (e != NULL);
            }
        }
    }
    return 0;
}

/* is `code` the place `at` or inside it? */
int place_under(const catalogue *c, const char *code, const char *at)
{
    if (!strcmp(code, at)) return 1;
    size_t n = 0;
    for (const rec_record *l = location(c, code); l && n < 32; l = location(c, rec_get(l, "Parent")), n++) {
        const char *k = rec_get(l, "Code");
        if (k && !strcmp(k, at)) return 1;
    }
    return 0;
}

char *upper_trim(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *p = xstrdup(s), *e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    for (char *q = p; *q; q++) *q = (char)toupper((unsigned char)*q);
    return p;
}

int cmd_list(int argc, char **argv)
{
    const char *given = NULL, *within = NULL, *at = NULL, *made = NULL, *acc = NULL, *covers = NULL;
    for (int i = 0; i < argc; i++) {
        const char **slot = !strcmp(argv[i], "-C") ? &given : !strcmp(argv[i], "--in") ? &within
            : !strcmp(argv[i], "--at") ? &at : !strcmp(argv[i], "--made") ? &made
            : !strcmp(argv[i], "--access") ? &acc : !strcmp(argv[i], "--covers") ? &covers : NULL;
        if (!slot || i + 1 >= argc) return 2;
        *slot = argv[++i];
    }
    if (covers) {
        long a, b;
        if (edtf_span(covers, &a, &b) <= 0)
            die("--covers: %s is not a date or range (examples: 2019, 2015/2024, 2019-07/2019-08, 199X)", covers);
    }
    catalogue c;
    open_catalogue(given, &c);
    char *code = within ? upper_trim(within) : NULL, *place = at ? upper_trim(at) : NULL;
    for (size_t i = 0; i < c.rec.nrecords; i++) {
        const rec_record *d = &c.rec.records[i];
        if (!is_type(d, "Disc")) continue;
        if (code && !disc_in(d, code)) continue;
        if (place) {
            int hit = 0;
            for (size_t j = 0; j < d->nfields && !hit; j++)
                if (!strcmp(d->fields[j].name, "Location")) {
                    char *l = upper_trim(d->fields[j].value);
                    hit = place_under(&c, l, place);
                    free(l);
                }
            if (!hit) continue;
        }
        if (acc && strcmp(access_of(d), acc)) continue;
        if (made && strncmp(rec_get(d, "Date") ? rec_get(d, "Date") : "", made, strlen(made))) continue;
        if (covers && edtf_covers(rec_get(d, "Coverage"), covers) != 1) continue;
        char *w = where(&c, d);
        const char *files = rec_get(d, "Files");
        printf("%s\t%s\t%s\t%s files\t%s\t%s\n", rec_get(d, "Id"), rec_get(d, "Date") ? rec_get(d, "Date") : "None",
               rec_get(d, "Title") ? rec_get(d, "Title") : "None", files && *files ? files : "?", access_of(d), w);
        free(w);
    }
    free(code);
    free(place);
    return 0;
}
