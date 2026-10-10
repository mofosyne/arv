/* Reading a catalogue: find, list, id. */
#define _XOPEN_SOURCE 700
#include "arv.h"

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
    if (required) home_find(&h, given, NULL);
    else if (home_try(&h, given, NULL)) {       /* no archive: an empty catalogue */
        memset(&c->rec, 0, sizeof c->rec);
        c->dir = NULL;
        return -1;
    }
    c->dir = h.catalog_dir;
    char *path = join(c->dir, "archive.rec");
    if (!is_file(c->dir, "archive.rec")) {      /* an empty home, as the Python arv reads it */
        memset(&c->rec, 0, sizeof c->rec);
        free(path);
        (void)required;
        return -1;
    }
    if (rec_read(path, &c->rec, &bad)) {
        fprintf(stderr, "Error: cannot read %s%s\n", path, errno == EINVAL ? " (a line is not a field)" : "");
        exit(1);
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

/* where a found disc is, or that it was retired (a retired disc has left every place) */
static char *found_at(const catalogue *c, const rec_record *d)
{
    return rec_get(d, "Retired") ? xprintf("retired %s", rec_get(d, "Retired")) : where(c, d);
}

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
            char *w = found_at(&c, d);
            printf("DISC  %s  %s  [%s]\n", rec_get(d, "Id"), rec_get(d, "Title") ? rec_get(d, "Title") : "None",
                   *w ? w : "location unknown");
            free(w);
            any = 1;
        }
    }
    for (size_t i = 0; i < c.rec.nrecords; i++) {          /* data objects, by name or path */
        const rec_record *o = &c.rec.records[i];
        if (!is_type(o, "Object") || !rec_get(o, "Disc")) continue;
        const char *name = rec_get(o, "Name") ? rec_get(o, "Name") : "", *path = rec_get(o, "Path") ? rec_get(o, "Path") : "";
        if (!matches(pat, glob, name) && !matches(pat, glob, path)) continue;
        const rec_record *d = find_disc(&c, rec_get(o, "Disc"));
        char *w = d ? found_at(&c, d) : xstrdup("");
        int folder = strcmp(rec_get(o, "Kind") ? rec_get(o, "Kind") : "", "file");
        printf("OBJECT  %s%s  version %s  %s  data/%s%s  [%s]\n", name, folder ? "/" : "", rec_get(o, "Version") ? rec_get(o, "Version") : "?",
               rec_get(o, "Disc"), strcmp(path, ".") ? path : "", folder && strcmp(path, ".") ? "/" : "", *w ? w : "?");
        free(w);
        any = 1;
    }
    size_t hexlen = strspn(pat, "0123456789abcdef");
    if (hexlen == strlen(pat) && hexlen >= 7 && hexlen <= 40)  /* a git commit: the discs holding it */
        for (size_t i = 0; i < c.rec.nrecords; i++) {
            const rec_record *d = &c.rec.records[i];
            if (!is_type(d, "Disc") || !rec_get(d, "Id")) continue;
            char *path = catalogue_volume(&c, rec_get(d, "Id"), "git.tsv"), *line = NULL;
            size_t cap = 0;
            ssize_t len;
            FILE *fp = fopen(path, "r");
            strlist repos = { 0 }, shown = { 0 };     /* per repository: the commit, and the refs at it */
            while (fp && (len = getline(&line, &cap, fp)) >= 0) {
                while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
                char *cols[3] = { line, NULL, NULL };
                if (line[0] == '#' || split_tabs(line, cols, 3) < 3) continue;
                char *sp = strchr(cols[2], ' ');
                const char *v = sp ? sp + 1 : cols[2];
                if (strncmp(v, pat, hexlen) || (strcmp(cols[1], "commit") && strcmp(cols[1], "head"))) continue;
                size_t k = 0;
                while (k < repos.n && strcmp(repos.v[k], cols[0])) k++;
                if (k == repos.n) {
                    strlist_add(&repos, cols[0]);
                    char *first = xprintf("%.12s", v);
                    strlist_add(&shown, first);
                    free(first);
                }
                if (!strcmp(cols[1], "head")) {
                    if (sp) *sp = 0;
                    char *more = xprintf("%s%s%s", shown.v[k], strchr(shown.v[k], '(') ? ", " : " (", cols[2]);
                    free(shown.v[k]);
                    shown.v[k] = more;
                }
            }
            for (size_t k = 0; k < repos.n; k++) {
                char *w = found_at(&c, d);
                printf("GIT   %s  %s  commit %s%s  [%s]\n", rec_get(d, "Id"), repos.v[k], shown.v[k],
                       strchr(shown.v[k], '(') ? ")" : "", *w ? w : "location unknown");
                free(w);
                any = 1;
            }
            strlist_free(&repos);
            strlist_free(&shown);
            if (fp) fclose(fp);
            free(line);
            free(path);
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
            char *w = found_at(&c, d);
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
        char *w = found_at(&c, d);
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
    for (size_t i = 0; i < c.rec.nrecords; i++) {          /* files lost: retired with no other copy */
        const rec_record *r = &c.rec.records[i];
        if (is_type(r, "Disc")) {                           /* a disc retired on its own (arv retire DISC-ID) */
            for (size_t f = 0; f < r->nfields; f++) {
                const char *rel = strcmp(r->fields[f].name, "Lost") ? NULL : strstr(r->fields[f].value, "  ");
                if (!rel || !matches(pat, glob, rel + 2)) continue;
                if (!limit || files < limit) printf("LOST  %s  %s  (retired with no other copy)\n", rec_get(r, "Id") ? rec_get(r, "Id") : "?", rel + 2);
                files++;
                any = 1;
            }
            continue;
        }
        if (!is_type(r, "Revision")) continue;
        for (size_t f = 0; f < r->nfields; f++) {
            const char *rel = strcmp(r->fields[f].name, "Lost") ? NULL : strstr(r->fields[f].value, "  ");
            if (!rel || !matches(pat, glob, rel + 2)) continue;
            const char *code = "?";
            for (size_t k = 0; k < c.rec.nrecords; k++)
                if (is_type(&c.rec.records[k], "Collection") && rec_get(r, "Collection") && rec_get(&c.rec.records[k], "Uuid")
                    && !strcmp(rec_get(&c.rec.records[k], "Uuid"), rec_get(r, "Collection")) && rec_get(&c.rec.records[k], "Code"))
                    code = rec_get(&c.rec.records[k], "Code");
            if (!limit || files < limit)
                printf("LOST  %s/%s  %s  (retired with no other copy)\n", code, rec_get(r, "Edition") ? rec_get(r, "Edition") : "?", rel + 2);
            files++;
            any = 1;
        }
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

/* The date `ago` (5y, 18m, 90d) before today, or a date given as such (2021, 2021-06, 2021-06-30),
   as YYYY-MM-DD; NULL when it is neither */
static char *cutoff_date(const char *ago)
{
    char *end;
    long n = strtol(ago, &end, 10);
    if (n > 0 && end != ago && end[0] && !end[1] && strchr("ymd", end[0])) {
        struct tm tm;
        today_tm(&tm);
        if (end[0] == 'y') tm.tm_year -= (int)n;
        else if (end[0] == 'm') tm.tm_mon -= (int)n;
        else tm.tm_mday -= (int)n;
        tm.tm_hour = 12;
        tm.tm_isdst = -1;
        mktime(&tm);
        return xprintf("%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    }
    static const char *const shapes[] = { "dddd", "dddd-dd", "dddd-dd-dd", NULL };   /* d: a digit */
    for (int k = 0; shapes[k]; k++) {
        size_t len = strlen(shapes[k]), j = 0;
        if (strlen(ago) != len) continue;
        while (j < len && (shapes[k][j] == 'd' ? isdigit((unsigned char)ago[j]) : ago[j] == shapes[k][j])) j++;
        if (j == len) return xprintf("%s%s", ago, k == 0 ? "-01-01" : k == 1 ? "-01" : "");
    }
    return NULL;
}

/* The date of the disc's last successful check (outcome success or warning), not counting the
   test of the image when it was made: "" when there is none */
static const char *last_checked(const catalogue *c, const char *id)
{
    const char *last = "";
    for (size_t i = 0; i < c->rec.nrecords; i++) {
        const rec_record *e = &c->rec.records[i];
        const char *disc = rec_get(e, "Disc"), *type = rec_get(e, "Type"), *out = rec_get(e, "Outcome"), *date = rec_get(e, "Date"),
                   *note = rec_get(e, "Note");
        if (!is_type(e, "Event") || !disc || !type || !out || !date || strcmp(disc, id) || strcmp(type, "fixity check")) continue;
        if (strcmp(out, "success") && strcmp(out, "warning")) continue;
        if (note && !strncmp(note, "image test after creation", 25)) continue;
        if (strncmp(date, last, 10) > 0) last = date;
    }
    return last;
}

int cmd_list(int argc, char **argv)
{
    const char *given = NULL, *within = NULL, *at = NULL, *made = NULL, *acc = NULL, *covers = NULL, *unchecked = NULL;
    int one_place = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--one-place")) { one_place = 1; continue; }
        const char **slot = !strcmp(argv[i], "-C") ? &given : !strcmp(argv[i], "--in") ? &within
            : !strcmp(argv[i], "--at") ? &at : !strcmp(argv[i], "--made") ? &made
            : !strcmp(argv[i], "--access") ? &acc : !strcmp(argv[i], "--covers") ? &covers
            : !strcmp(argv[i], "--unchecked-since") ? &unchecked : NULL;
        if (!slot || i + 1 >= argc) return 2;
        *slot = argv[++i];
    }
    if (covers) {
        long a, b;
        if (edtf_span(covers, &a, &b) <= 0)
            die("--covers: %s is not a date or range (examples: 2019, 2015/2024, 2019-07/2019-08, 199X)", covers);
    }
    char *cutoff = NULL;
    if (unchecked && !(cutoff = cutoff_date(unchecked)))
        die("--unchecked-since: %s is neither an age (5y, 18m, 90d) nor a date (2021, 2021-06, 2021-06-30)", unchecked);
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
        const char *checked = cutoff ? last_checked(&c, rec_get(d, "Id") ? rec_get(d, "Id") : "") : NULL;
        if (cutoff && *checked && strncmp(checked, cutoff, 10) >= 0) continue;
        if (one_place) {
            size_t places = 0;
            for (size_t j = 0; j < d->nfields; j++) places += !strcmp(d->fields[j].name, "Location");
            if (places > 1) continue;
        }
        char *w = where(&c, d);
        const char *files = rec_get(d, "Files");
        printf("%s\t%s\t%s\t%s file%s\t%s\t%s", rec_get(d, "Id"), rec_get(d, "Date") ? rec_get(d, "Date") : "None",
               rec_get(d, "Title") ? rec_get(d, "Title") : "None", files && *files ? files : "?",
               files && !strcmp(files, "1") ? "" : "s", access_of(d), w);
        if (cutoff) printf("\tlast checked %.10s", *checked ? checked : "never");
        putchar('\n');
        free(w);
    }
    free(cutoff);
    free(code);
    free(place);
    return 0;
}
