/*
 * Checks arvc's C code against tests/fixtures/ (the language-neutral contract the Python
 * implementation passes too): recfile/, coverage.tsv, covers.tsv.
 *
 *   fixtures DIR        prints each failing case; exit 0 when all pass
 */
#define _XOPEN_SOURCE 700
#include "../src/arvc/discid.h"
#include "../src/arvc/edtf.h"
#include "../src/arvc/rec.h"
#include "../src/arvc/sha512.h"
#include "../src/arvc/vocab.h"
#include "../src/arvc/arvc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, cases;

/* TSV escapes: \t \n \\ */
static void unescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *o++ = *s == 't' ? '\t' : *s == 'n' ? '\n' : *s;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

/* Calls fn for each case line, split on tabs and unescaped. */
static void each_case(const char *dir, const char *name, void (*fn)(char **cols, int n))
{
    char path[4096], *line = NULL;
    size_t cap = 0;
    ssize_t len;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("FAIL %s: cannot open\n", path);
        failures++;
        return;
    }
    while ((len = getline(&line, &cap, fp)) >= 0) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (line[0] == '#' || !len) continue;
        char *cols[16];
        int n = 0;
        for (char *p = line; n < 16;) {
            cols[n++] = p;
            char *t = strchr(p, '\t');
            if (!t) break;
            *t = 0;
            p = t + 1;
        }
        for (int i = 0; i < n; i++) unescape(cols[i]);
        cases++;
        fn(cols, n);
    }
    free(line);
    fclose(fp);
}

static void day(long d, char out[32])
{
    snprintf(out, 32, "%04ld-%02ld-%02ld", d / 10000, d / 100 % 100, d % 100);
}

static void expect(const char *what, const char *input, const char *got, const char *want)
{
    if (strcmp(got, want)) {
        printf("FAIL %s [%s]: got [%s], want [%s]\n", what, input, got, want);
        failures++;
    }
}

/* input, edtf, compact, first day, last day */
static void coverage_case(char **c, int n)
{
    long a = 0, b = 0;
    char fa[32] = "", fb[32] = "", e[64], k[64];
    if (n < 5) return;
    expect("to_edtf", c[0], discid_to_edtf(c[0], e, sizeof e) ? "ERROR" : e, c[1]);
    expect("compact", c[0], discid_compact(c[0], k, sizeof k) ? "ERROR" : k, c[2]);
    int r = edtf_span(c[0], &a, &b);
    if (r > 0) {
        day(a, fa);
        day(b, fb);
    } else if (r < 0) {
        strcpy(fa, "ERROR");
        strcpy(fb, "ERROR");
    }
    if (strcmp(fa, c[3]) || strcmp(fb, c[4])) {
        printf("FAIL coverage %s: got %s / %s, want %s / %s\n", c[0], fa, fb, c[3], c[4]);
        failures++;
    }
}

static void covers_case(char **c, int n)
{
    if (n < 3) return;
    int r = edtf_covers(c[0], c[1]);
    const char *got = r > 0 ? "yes" : r == 0 ? "no" : "ERROR";
    if (strcmp(got, c[2])) {
        printf("FAIL covers %s %s: got %s, want %s\n", c[0], c[1], got, c[2]);
        failures++;
    }
}

static void check_char_case(char **c, int n)
{
    char got[2] = { 0, 0 };
    if (n < 2) return;
    got[0] = discid_check(c[0]);
    expect("check character", c[0], got, c[1]);
}

/* set, sequence, coverage, id */
static void compose_case(char **c, int n)
{
    char id[64];
    if (n < 4) return;
    expect("compose", c[0], discid_compose(c[0], atol(c[1]), c[2], id, sizeof id) ? "ERROR" : id, c[3]);
}

/* text, scheme, set, sequence, coverage, check, valid */
static void parse_case(char **c, int n)
{
    discid_parts p;
    char got[256], want[256];
    if (n < 7) return;
    if (discid_parse(c[0], &p))
        snprintf(got, sizeof got, "%s|%s|%ld|%s|%c|%s", p.scheme, p.set, p.sequence, p.coverage,
                 p.check ? p.check : '-', p.valid ? "yes" : "no");
    else
        snprintf(got, sizeof got, "none|||||no");
    if (!strcmp(c[1], "none")) snprintf(want, sizeof want, "none|||||no");
    else snprintf(want, sizeof want, "%s|%s|%s|%s|%c|%s", c[1], c[2], c[3], c[4], c[5][0] ? c[5][0] : '-', c[6]);
    expect("parse", c[0], got, want);
}

static vocab fixture_vocab;

/* code, paths (space separated) */
static void vocab_paths_case(char **c, int n)
{
    strlist p = { 0 };
    char got[1024] = "";
    vocab_paths(&fixture_vocab, c[0], &p);
    for (size_t i = 0; i < p.n; i++) {
        if (i) strcat(got, " ");
        strcat(got, p.v[i]);
    }
    strlist_free(&p);
    expect("vocab paths", c[0], got, n > 1 ? c[1] : "");
}

/* word, resolve, guess */
static void vocab_words_case(char **c, int n)
{
    const char *r = vocab_resolve(&fixture_vocab, c[0]), *g = vocab_guess(&fixture_vocab, c[0]);
    expect("vocab resolve", c[0], r ? r : "", n > 1 ? c[1] : "");
    expect("vocab guess", c[0], g ? g : "", n > 2 ? c[2] : "");
}

/* pattern, path, matches */
static void match_case(char **c, int n)
{
    if (n < 3) return;
    expect("match rule", c[0], vocab_path_matches(c[0], c[1]) ? "yes" : "no", c[2]);
}

/* file name, result (ok, warning, error: the worst issue) */
static void names_case(char **c, int n)
{
    name_issues x = { 0 };
    char *one[1] = { c[0] };
    if (n < 2) return;
    names_check(one, 1, &x);
    const char *got = "ok";
    for (size_t i = 0; i < x.n; i++) got = x.v[i].error ? "error" : !strcmp(got, "error") ? got : "warning";
    names_free(&x);
    expect("names", c[0], got, c[1]);
}

/* disc id, text, volume label */
static void label_case(char **c, int n)
{
    if (n < 3) return;
    char *got = volume_label(c[0], c[1]);
    expect("label", c[1], got, c[2]);
    free(got);
}

/* record, type, field, value: every field in file order */
static rec_file rec;
static size_t rec_index, field_index;
static const char *rec_name;

static void recfile_case(char **c, int n)
{
    if (n < 4) return;
    size_t want = (size_t)atol(c[0]);
    if (want != rec_index) { rec_index = want; field_index = 0; }
    if (rec_index >= rec.nrecords || field_index >= rec.records[rec_index].nfields) {
        printf("FAIL recfile %s: record %s has no field %s\n", rec_name, c[0], c[2]);
        failures++;
        return;
    }
    const rec_record *r = &rec.records[rec_index];
    const rec_field *f = &r->fields[field_index++];
    const char *type = r->type ? r->type : "";
    if (strcmp(type, c[1]) || strcmp(f->name, c[2]) || strcmp(f->value, c[3])) {
        printf("FAIL recfile %s record %s: got %s %s=[%s], want %s %s=[%s]\n", rec_name, c[0],
               type, f->name, f->value, c[1], c[2], c[3]);
        failures++;
    }
}

static void check_recfile(const char *dir, const char *name)
{
    char path[4096], expected[256];
    int bad = 0;
    snprintf(path, sizeof path, "%s/recfile/%s.rec", dir, name);
    if (rec_read(path, &rec, &bad)) {
        printf("FAIL recfile %s: cannot read (line %d)\n", name, bad);
        failures++;
        return;
    }
    rec_name = name;
    rec_index = 0;
    field_index = 0;
    snprintf(expected, sizeof expected, "recfile/%s.expected.tsv", name);
    each_case(dir, expected, recfile_case);
    rec_free(&rec);
}

/* fixtures --hash FILE...: "sha512  name" lines, as sha512sum prints them */
static int hash_files(int argc, char **argv)
{
    static unsigned char buf[65536];
    for (int i = 2; i < argc; i++) {
        FILE *fp = fopen(argv[i], "rb");
        sha512_ctx c;
        unsigned char d[64];
        char hex[129];
        size_t n;
        if (!fp) return 1;
        sha512_init(&c);
        while ((n = fread(buf, 1, sizeof buf, fp)) > 0) sha512_update(&c, buf, n);
        fclose(fp);
        sha512_final(&c, d);
        sha512_hex(d, hex);
        printf("%s  %s\n", hex, argv[i]);
    }
    return 0;
}

/* fixtures --roundtrip IN OUT: read a recfile and write it again (recfile.py writes the same bytes) */
static int roundtrip(const char *in, const char *out)
{
    rec_file f;
    int bad = 0;
    if (rec_read(in, &f, &bad)) return 1;
    rec_record **all = malloc((f.nrecords + 1) * sizeof *all);
    for (size_t i = 0; i < f.nrecords; i++) all[i] = &f.records[i];
    int rc = rec_write(out, all, f.nrecords);
    free(all);
    rec_free(&f);
    return rc ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--hash")) return hash_files(argc, argv);
    if (argc == 4 && !strcmp(argv[1], "--roundtrip")) return roundtrip(argv[2], argv[3]);
    if (argc != 2) {
        fputs("usage: fixtures tests/fixtures\n", stderr);
        return 2;
    }
    each_case(argv[1], "coverage.tsv", coverage_case);
    each_case(argv[1], "covers.tsv", covers_case);
    each_case(argv[1], "check-chars.tsv", check_char_case);
    each_case(argv[1], "disc-id-compose.tsv", compose_case);
    each_case(argv[1], "disc-id-parse.tsv", parse_case);
    char path[4096], err[256];
    snprintf(path, sizeof path, "%s/vocab.rec", argv[1]);
    if (vocab_load(&fixture_vocab, path, NULL, err, sizeof err)) {
        printf("FAIL %s\n", err);
        failures++;
    } else {
        each_case(argv[1], "vocab-paths.tsv", vocab_paths_case);
        each_case(argv[1], "vocab-words.tsv", vocab_words_case);
        vocab_free(&fixture_vocab);
    }
    each_case(argv[1], "match-rules.tsv", match_case);
    each_case(argv[1], "names.tsv", names_case);
    each_case(argv[1], "labels.tsv", label_case);
    check_recfile(argv[1], "multiline");
    check_recfile(argv[1], "types");
    printf("%d fixture cases, %d failed\n", cases, failures);
    return failures != 0;
}
