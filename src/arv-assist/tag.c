/*
 * arv tag (tagger.py): consistent folder tags from your own tag vocabulary.
 *
 * - Every tag in <home>/config/tags.rec has a Description; Match globs tag a folder without any
 *   model; Alias words mean the same tag.
 * - Each folder gets a short summary (its path, sample file names, README text, image captions).
 *   A small embedding model turns tag descriptions and summaries into vectors; the tags whose
 *   descriptions are closest to a folder's summary are suggested.
 * - Tags you accept are remembered (<home>/config/tag-examples.jsonl): a new folder that looks
 *   like one you tagged before gets those tags suggested too, so suggestions follow your habits.
 * The model is llama.cpp's llama-embedding with a pinned GGUF file (see models.c), or any
 * OpenAI-compatible /v1/embeddings server (--embed-url).
 */
#define _XOPEN_SOURCE 700
#include "assist.h"
#include "../arvc/data.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXAMPLE_MIN 0.75    /* tuned on bge-small: similar trip folders ~0.78-0.80, unrelated 0.4-0.71 */
#define EXAMPLE_SHIFT 0.2
#define SEPARATOR "<#archive-sep#>"

/* ------------------------------------------------------------------ the vocabulary */

typedef struct {
    strlist names, descriptions;            /* in order */
    strlist alias_from, alias_to;
    strlist rule_tag, rule_glob;            /* Match rules */
} tag_vocab;

static int load_vocab(const arv_home *h, const char *given, tag_vocab *v)
{
    memset(v, 0, sizeof *v);
    char *path = given ? xstrdup(given) : join(h->config_dir, "tags.rec");
    if (!given && access(path, F_OK) && !mkdirs(h->config_dir)) write_text(path, DATA_DEFAULT_TAGS);
    rec_file f;
    int bad = 0;
    if (rec_read(path, &f, &bad)) die("cannot read the tag vocabulary %s", path);
    for (size_t i = 0; i < f.nrecords; i++) {
        const rec_record *r = &f.records[i];
        if (r->descriptor || !rec_get(r, "Name")) continue;
        char *name = tag_normalise(rec_get(r, "Name"));
        const char *desc = rec_get(r, "Description");
        strlist_add(&v->names, name);
        strlist_add(&v->descriptions, desc && *desc ? desc : name);
        for (size_t j = 0; j < r->nfields; j++) {
            if (!strcmp(r->fields[j].name, "Alias")) {
                char *a = tag_normalise(r->fields[j].value);
                strlist_add(&v->alias_from, a);
                strlist_add(&v->alias_to, name);
                free(a);
            } else if (!strcmp(r->fields[j].name, "Match")) {
                char *m = xstrdup(r->fields[j].value), *s = m, *e = m + strlen(m);
                while (isspace((unsigned char)*s)) s++;
                while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
                if (*s) {
                    strlist_add(&v->rule_tag, name);
                    strlist_add(&v->rule_glob, s);
                }
                free(m);
            }
        }
        free(name);
    }
    rec_free(&f);
    if (!v->names.n) die("no tags in %s", path);
    free(path);
    return 0;
}

/* the vocabulary's name for a tag or one of its aliases ("holiday" -> "travel") */
static char *canonical(const tag_vocab *v, const char *tag)
{
    char *t = tag_normalise(tag);
    for (size_t i = 0; v && i < v->alias_from.n; i++)
        if (!strcmp(v->alias_from.v[i], t)) {
            free(t);
            return xstrdup(v->alias_to.v[i]);
        }
    return t;
}

static void canonical_list(const tag_vocab *v, const strlist *in, strlist *out)
{
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < in->n; i++) {
        char *t = canonical(v, in->v[i]);
        if (*t && !strlist_has(out, t)) strlist_add(out, t);
        free(t);
    }
}

/* ------------------------------------------------------------------ folder summaries */

static void folder_key(const char *path, char **out)
{
    const char *slash = strrchr(path, '/');
    char *parent = slash ? xprintf("%.*s", (int)(slash - path), path) : xstrdup(".");
    const char *third = NULL;
    int k = 0;
    for (const char *q = parent; *q && !third; q++)
        if (*q == '/' && ++k == 3) third = q;
    *out = third ? xprintf("%.*s", (int)(third - parent), parent) : xstrdup(parent);
    free(parent);
}

static const char *const KIND_NAMES[] = { "photos", "videos", "audio recordings", "PDF documents", "text documents",
                                          "spreadsheets", "source code files", "archives" };
static const char *const KIND_EXTS[] = {
    ".jpg .jpeg .png .heic .tif .tiff .raw .cr2 .nef .arw .dng .webp .gif .bmp",
    ".mp4 .mov .mkv .avi .m4v .mts .webm .wmv",
    ".mp3 .flac .wav .ogg .m4a .aac",
    ".pdf",
    ".doc .docx .odt .txt .md .rtf",
    ".xls .xlsx .ods .csv",
    ".c .cpp .h .py .js .ts .rs .go .java .ino .sh .ini .toml",
    ".zip .tar .gz .7z .rar .iso .img .bundle",
};

static const char *kind_of(const char *ext)
{
    if (!*ext) return "files";
    for (size_t k = 0; k < sizeof KIND_NAMES / sizeof *KIND_NAMES; k++) {
        const char *s = KIND_EXTS[k];
        size_t n = strlen(ext);
        while ((s = strstr(s, ext))) {
            if (s[n] == ' ' || !s[n]) return KIND_NAMES[k];
            s += n;
        }
    }
    return "files";
}

/* camera and phone default names carry no meaning: DSC_4001.JPG, IMG_20211221_1830.jpg, GH010042.MP4 */
static int generic_name(const char *base)
{
    static const char *const words[] = { "dscn", "dsc", "img", "pxl", "mvi", "vid", "gopr", "dji", "sam", "photo", "image",
                                         "scan", NULL };
    for (int i = 0; words[i]; i++) {
        size_t n = strlen(words[i]);
        if (strncasecmp(base, words[i], n)) continue;
        const char *r = base + n;
        if (*r == '_' || *r == '-') r++;
        if (isdigit((unsigned char)*r)) return 1;
    }
    /* gh<digit>, gx<digit>, p<digit>, then [_-]? and a digit */
    const char *r = NULL;
    if ((!strncasecmp(base, "gh", 2) || !strncasecmp(base, "gx", 2)) && isdigit((unsigned char)base[2])) r = base + 3;
    else if ((base[0] == 'p' || base[0] == 'P') && isdigit((unsigned char)base[1])) r = base + 2;
    if (r) {
        if (*r == '_' || *r == '-') r++;
        if (isdigit((unsigned char)*r)) return 1;
    }
    return 0;
}

/* runs of _ - . become one space */
static char *words(const char *text, size_t n)
{
    sbuf b = { 0 };
    int gap = 0;
    for (size_t i = 0; i < n && text[i]; i++) {
        char c = text[i];
        if (c == '_' || c == '-' || c == '.') { gap = 1; continue; }
        if (gap && b.len) sb_puts(&b, " ");
        gap = 0;
        sb_add(&b, &c, 1);
    }
    char *s = b.s ? b.s : xstrdup("");
    char *e = s + strlen(s);
    while (e > s && e[-1] == ' ') *--e = 0;
    return s;
}

typedef struct {
    strlist folders, texts;
} summaries;

static void summarise(const items *l, const char *text_root, const ftags *captions, summaries *out)
{
    memset(out, 0, sizeof *out);
    /* items by path, grouped by folder in order of first appearance */
    size_t *idx = xmalloc((l->n + 1) * sizeof *idx);
    for (size_t i = 0; i < l->n; i++) idx[i] = i;
    for (size_t i = 1; i < l->n; i++)
        for (size_t j = i; j > 0 && strcmp(l->v[idx[j - 1]].path, l->v[idx[j]].path) > 0; j--) {
            size_t t = idx[j];
            idx[j] = idx[j - 1];
            idx[j - 1] = t;
        }
    char **keys = xmalloc((l->n + 1) * sizeof *keys);
    for (size_t i = 0; i < l->n; i++) {
        folder_key(l->v[idx[i]].path, &keys[i]);
        if (!strlist_has(&out->folders, keys[i])) strlist_add(&out->folders, keys[i]);
    }
    for (size_t f = 0; f < out->folders.n; f++) {
        const char *folder = out->folders.v[f];
        strlist kinds = { 0 }, names = { 0 };
        size_t *counts = NULL;
        for (size_t i = 0; i < l->n; i++) {
            if (strcmp(keys[i], folder)) continue;
            const char *p = l->v[idx[i]].path, *slash = strrchr(p, '/'), *base = slash ? slash + 1 : p, *dot = strrchr(base, '.');
            char ext[32] = "";
            if (dot && dot != base) {
                snprintf(ext, sizeof ext, "%s", dot);
                for (char *q = ext; *q; q++) *q = (char)tolower((unsigned char)*q);
            }
            const char *k = kind_of(ext);
            size_t m = 0;
            while (m < kinds.n && strcmp(kinds.v[m], k)) m++;
            if (m == kinds.n) {
                strlist_add(&kinds, k);
                counts = xrealloc(counts, kinds.n * sizeof *counts);
                counts[m] = 0;
            }
            counts[m]++;
            size_t blen = dot && dot != base ? (size_t)(dot - base) : strlen(base);
            char *b = xprintf("%.*s", (int)blen, base);
            if (!generic_name(b) && names.n < 8) {
                char *w = words(b, blen);
                strlist_add(&names, w);
                free(w);
            }
            free(b);
        }
        sbuf t = { 0 };
        char *fcopy = xstrdup(folder);
        int first = 1;
        for (char *part = fcopy, *next; part; part = next) {
            next = strchr(part, '/');
            if (next) *next++ = 0;
            char *w = words(part, strlen(part));
            sb_printf(&t, "%s%s", first ? "" : ", ", w);
            free(w);
            first = 0;
        }
        free(fcopy);
        sb_puts(&t, ": ");
        for (int shown = 0; shown < 3; shown++) {
            size_t best = kinds.n;
            for (size_t m = 0; m < kinds.n; m++)
                if (counts[m] && (best == kinds.n || counts[m] > counts[best])) best = m;
            if (best == kinds.n) break;
            sb_printf(&t, "%s%zu %s", shown ? ", " : "", counts[best], kinds.v[best]);
            counts[best] = 0;
        }
        if (names.n) {
            sb_puts(&t, ". Named: ");
            for (size_t k = 0; k < names.n; k++) sb_printf(&t, "%s%s", k ? "; " : "", names.v[k]);
        }
        for (size_t i = 0; text_root && i < l->n; i++) {       /* the first README-like file */
            if (strcmp(keys[i], folder)) continue;
            const item *it = &l->v[idx[i]];
            const char *slash = strrchr(it->path, '/'), *base = slash ? slash + 1 : it->path;
            char *stem = lower(base), *dot = strchr(stem, '.');
            if (dot) *dot = 0;
            int readme = !strcmp(stem, "readme") || !strcmp(stem, "notes") || !strcmp(stem, "about") ||
                         !strcmp(stem, "description") || !strcmp(stem, "info");
            free(stem);
            if (!readme || it->size >= 256 * 1024) continue;
            char *full = join(text_root, it->path), *text = read_text(full);
            free(full);
            if (text) {
                text[strlen(text) > 400 ? 400 : strlen(text)] = 0;
                sbuf w = { 0 };
                int sp = 0;
                for (char *c = text; *c; c++) {
                    if (*c == '#' || isspace((unsigned char)*c)) { sp = w.len > 0; continue; }
                    if (sp) sb_puts(&w, " ");
                    sp = 0;
                    sb_add(&w, c, 1);
                }
                sb_printf(&t, ". %s", w.s ? w.s : "");
                free(w.s);
                free(text);
            }
            break;
        }
        for (size_t c = 0; captions && c < captions->n; c++)
            if (!strcmp(captions->folder[c], folder) && captions->caption[c]) sb_printf(&t, ". Images show: %s", captions->caption[c]);
        strlist_add(&out->texts, t.s);
        free(t.s);
        strlist_free(&kinds);
        strlist_free(&names);
        free(counts);
    }
    for (size_t i = 0; i < l->n; i++) free(keys[i]);
    free(keys);
    free(idx);
}

/* ------------------------------------------------------------------ embeddings */

typedef struct {
    char *binary, *model_path, *model_name, *prefix;
    llm_client *server;             /* --embed-url: an /embeddings server instead */
} embedder;

static double *parse_vectors(const jv *arr, size_t want, size_t *dim, char **err)
{
    if (!arr || arr->kind != 'a' || arr->n != want) {
        *err = xprintf("%zu vectors came back for %zu texts", arr && arr->kind == 'a' ? arr->n : 0, want);
        return NULL;
    }
    *dim = arr->n ? arr->vals[0]->n : 0;
    double *out = xmalloc((want * *dim + 1) * sizeof *out);
    for (size_t i = 0; i < want; i++) {
        const jv *v = arr->vals[i];
        if (v->kind != 'a' || v->n != *dim) {
            *err = xstrdup("vectors of different lengths came back");
            free(out);
            return NULL;
        }
        for (size_t k = 0; k < *dim; k++) out[i * *dim + k] = strtod(v->vals[k]->str, NULL);
    }
    return out;
}

static void normalise(double *v, size_t dim)
{
    double n = 0;
    for (size_t k = 0; k < dim; k++) n += v[k] * v[k];
    n = sqrt(n);
    if (n == 0) n = 1;
    for (size_t k = 0; k < dim; k++) v[k] /= n;
}

/* L2-normalised vectors for texts (so a dot product is the cosine similarity); NULL with *err */
static double *embed(embedder *e, const strlist *texts, size_t *dim, char **err)
{
    double *all = NULL;
    *dim = 0;
    for (size_t at = 0; at < texts->n; at += 64) {
        size_t n = texts->n - at < 64 ? texts->n - at : 64, d = 0;
        double *part = NULL;
        if (e->server) {
            jv *body = jobj(), *input = jarr();
            for (size_t i = 0; i < n; i++) jpush(input, jstr(texts->v[at + i]));
            jput(body, "model", jstr(e->model_name));
            jput(body, "input", input);
            jv *r = llm_call(e->server, "/embeddings", body, err);
            jfree(body);
            if (!r) { free(all); return NULL; }
            const jv *data = json_get(r, "data");
            jv *arr = jarr();                   /* by index, as the server may answer in any order */
            for (size_t i = 0; i < n; i++)
                for (size_t k = 0; data && data->kind == 'a' && k < data->n; k++) {
                    const jv *idx = json_get(data->vals[k], "index");
                    size_t want = idx && idx->kind == 't' ? (size_t)strtoul(idx->str, NULL, 10) : k;
                    if (want == i) { jpush(arr, (jv *)json_get(data->vals[k], "embedding")); break; }
                }
            part = parse_vectors(arr, n, &d, err);
            free(arr->vals);
            free(arr);
            jfree(r);
            if (part)
                for (size_t i = 0; i < n; i++) normalise(part + i * d, d);
        } else {
            char tmpl[] = "/tmp/arv-embed-XXXXXX";
            int fd = mkstemp(tmpl);
            if (fd < 0) { *err = xstrdup("cannot make a temporary file"); free(all); return NULL; }
            sbuf b = { 0 };
            for (size_t i = 0; i < n; i++) {
                const char *t = texts->v[at + i];
                sbuf c = { 0 };
                for (const char *p = t; *p; p++) {         /* the separator and NULs cannot be in a text */
                    if (!strncmp(p, SEPARATOR, strlen(SEPARATOR))) { sb_puts(&c, " "); p += strlen(SEPARATOR) - 1; }
                    else sb_add(&c, p, 1);
                }
                char *s = c.s ? c.s : xstrdup(""), *e2 = s + strlen(s), *st = s;
                while (isspace((unsigned char)*st)) st++;
                while (e2 > st && isspace((unsigned char)e2[-1])) *--e2 = 0;
                sb_printf(&b, "%s%s", i ? SEPARATOR : "", *st ? st : "(empty)");
                free(s);
            }
            if (write(fd, b.s, b.len) != (ssize_t)b.len) { *err = xstrdup("cannot write a temporary file"); close(fd); free(b.s); free(all); return NULL; }
            close(fd);
            free(b.s);
            long cpus = sysconf(_SC_NPROCESSORS_ONLN);
            char threads[24];
            snprintf(threads, sizeof threads, "%ld", cpus > 0 ? cpus : 2);
            char *argv[] = { e->binary, "-m", e->model_path, "-f", tmpl, "--embd-separator", SEPARATOR, "--embd-output-format",
                             "array", "--embd-normalize", "2", "--pooling", "cls", "-t", threads, "-c", "512", "-b", "512",
                             "-ub", "512", NULL };
            char *out = NULL;
            int rc = run(argv, &out);
            unlink(tmpl);
            char *open = out ? strstr(out, "[[") : NULL, *close = out ? strstr(out, "]]") : NULL;
            for (char *c2 = close; c2 && (c2 = strstr(c2 + 1, "]]"));) close = c2;
            if (rc || !open || !close) {
                size_t len = out ? strlen(out) : 0;
                *err = xprintf("llama-embedding failed (%d): %s", rc, out ? out + (len > 600 ? len - 600 : 0) : "");
                free(out);
                free(all);
                return NULL;
            }
            close[2] = 0;
            jv *arr = json_parse(open);
            part = parse_vectors(arr, n, &d, err);
            jfree(arr);
            free(out);
        }
        if (!part) { free(all); return NULL; }
        *dim = d;
        all = xrealloc(all, (at + n) * d * sizeof *all);
        memcpy(all + at * d, part, n * d * sizeof *part);
        free(part);
    }
    return all;
}

static double dot(const double *a, const double *b, size_t dim)
{
    double s = 0;
    for (size_t k = 0; k < dim; k++) s += a[k] * b[k];
    return s;
}

/* ------------------------------------------------------------------ examples from reviews */

typedef struct {
    strlist tags;
    double *vec;
    size_t dim;
} example;

static char *examples_path(const arv_home *h)
{
    return join(h->config_dir, "tag-examples.jsonl");
}

static size_t read_examples(const arv_home *h, const char *model, example **out)
{
    *out = NULL;
    char *path = examples_path(h), *text = read_text(path);
    free(path);
    size_t n = 0;
    for (char *line = text ? strtok(text, "\n") : NULL; line; line = strtok(NULL, "\n")) {
        jv *ex = json_parse(line);
        const char *m = ex ? jstr_of(json_get(ex, "model")) : NULL;
        const jv *vec = ex ? json_get(ex, "vector") : NULL, *tags = ex ? json_get(ex, "tags") : NULL;
        if (m && !strcmp(m, model) && vec && vec->kind == 'a') {
            *out = xrealloc(*out, (n + 1) * sizeof **out);
            example *e = &(*out)[n++];
            memset(e, 0, sizeof *e);
            e->dim = vec->n;
            e->vec = xmalloc((vec->n + 1) * sizeof *e->vec);
            for (size_t k = 0; k < vec->n; k++) e->vec[k] = strtod(vec->vals[k]->str, NULL);
            for (size_t k = 0; tags && tags->kind == 'a' && k < tags->n; k++)
                if (tags->vals[k]->kind == 's') strlist_add(&e->tags, tags->vals[k]->str);
        }
        jfree(ex);
    }
    free(text);
    return n;
}

/* the reviewed tags, kept so future suggestions follow them */
static void remember(const arv_home *h, embedder *e, const summaries *s, const ftags *accepted, char **err)
{
    strlist texts = { 0 }, folders = { 0 };
    for (size_t i = 0; i < accepted->n; i++) {
        if (!accepted->tags[i].n) continue;
        for (size_t f = 0; f < s->folders.n; f++)
            if (!strcmp(s->folders.v[f], accepted->folder[i])) {
                strlist_add(&texts, s->texts.v[f]);
                strlist_add(&folders, accepted->folder[i]);
            }
    }
    if (!texts.n) return;
    size_t dim;
    double *vecs = embed(e, &texts, &dim, err);
    if (!vecs) return;
    mkdirs(h->config_dir);
    char *path = examples_path(h);
    FILE *fp = fopen(path, "a");
    for (size_t i = 0; fp && i < texts.n; i++) {
        jv *ex = jobj(), *vec = jarr();
        jput(ex, "model", jstr(e->model_name));
        jput(ex, "folder", jstr(folders.v[i]));
        jput(ex, "text", jstr(texts.v[i]));
        jput(ex, "tags", jstrings(ftags_get((ftags *)accepted, folders.v[i], 0)));
        for (size_t k = 0; k < dim; k++) {
            char num[32];
            snprintf(num, sizeof num, "%.5f", vecs[i * dim + k]);
            jpush(vec, jnum(num));
        }
        jput(ex, "vector", vec);
        sbuf b = { 0 };
        json_dump(&b, ex, -1);
        fprintf(fp, "%s\n", b.s);
        free(b.s);
        jfree(ex);
    }
    if (fp) fclose(fp);
    free(path);
    free(vecs);
    strlist_free(&texts);
    strlist_free(&folders);
}

/* ------------------------------------------------------------------ suggesting */

typedef struct {
    char *tag;
    double score;
    int rule;               /* from a Match rule (no score) */
    int order;              /* the vocabulary's order, for equal scores */
} ranked;

typedef struct {
    char **folder;
    ranked **v;
    size_t *n, count;
} suggestions;

static ranked *slot(suggestions *s, const char *folder, int create, size_t **n_out)
{
    for (size_t i = 0; i < s->count; i++)
        if (!strcmp(s->folder[i], folder)) { *n_out = &s->n[i]; return s->v[i]; }
    if (!create) return NULL;
    s->folder = xrealloc(s->folder, (s->count + 1) * sizeof *s->folder);
    s->v = xrealloc(s->v, (s->count + 1) * sizeof *s->v);
    s->n = xrealloc(s->n, (s->count + 1) * sizeof *s->n);
    s->folder[s->count] = xstrdup(folder);
    s->v[s->count] = NULL;
    s->n[s->count] = 0;
    *n_out = &s->n[s->count];
    return s->v[s->count++];
}

static void add_ranked(suggestions *s, const char *folder, const char *tag, double score, int rule)
{
    size_t *n;
    slot(s, folder, 1, &n);
    size_t i = 0;
    while (i < s->count && strcmp(s->folder[i], folder)) i++;
    for (size_t k = 0; k < *n; k++)
        if (!strcmp(s->v[i][k].tag, tag)) return;
    s->v[i] = xrealloc(s->v[i], (*n + 1) * sizeof **s->v);
    s->v[i][*n].tag = xstrdup(tag);
    s->v[i][*n].score = score;
    s->v[i][*n].rule = rule;
    s->v[i][*n].order = (int)*n;
    (*n)++;
}

/* Match rules: a tag applies to a folder when its patterns claim at least a tenth of its files */
static void rule_tags(const tag_vocab *v, const items *l, const summaries *sum, suggestions *out)
{
    if (!v->rule_tag.n) return;
    for (size_t f = 0; f < sum->folders.n; f++) {
        size_t files = 0;
        strlist names = { 0 };
        size_t *counts = NULL;
        for (size_t r = 0; r < v->rule_tag.n; r++)
            if (!strlist_has(&names, v->rule_tag.v[r])) strlist_add(&names, v->rule_tag.v[r]);
        counts = xmalloc((names.n + 1) * sizeof *counts);
        memset(counts, 0, (names.n + 1) * sizeof *counts);
        for (size_t i = 0; i < l->n; i++) {
            char *key;
            folder_key(l->v[i].path, &key);
            int here = !strcmp(key, sum->folders.v[f]);
            free(key);
            if (!here) continue;
            files++;
            for (size_t m = 0; m < names.n; m++) {
                int hit = 0;
                for (size_t r = 0; r < v->rule_tag.n && !hit; r++)
                    if (!strcmp(v->rule_tag.v[r], names.v[m])) hit = vocab_path_matches(v->rule_glob.v[r], l->v[i].path);
                counts[m] += hit;
            }
        }
        double need = 0.1 * (double)files;
        if (need < 1) need = 1;
        for (size_t m = 0; m < names.n; m++)
            if ((double)counts[m] >= need) add_ranked(out, sum->folders.v[f], names.v[m], 0, 1);
        strlist_free(&names);
        free(counts);
    }
}

static int by_score(const void *a, const void *b)
{
    const ranked *x = a, *y = b;
    if (x->score != y->score) return x->score > y->score ? -1 : 1;
    return x->order - y->order;
}

/* the model's suggestions, best first: at most top, within margin of the best */
static int model_tags(embedder *e, const arv_home *h, const tag_vocab *v, const summaries *sum, int top, suggestions *out, char **err)
{
    if (!sum->folders.n) return 0;
    strlist texts = { 0 };
    for (size_t i = 0; i < v->names.n; i++) {
        char *t = xprintf("%s%s", e->prefix, v->descriptions.v[i]);
        strlist_add(&texts, t);
        free(t);
    }
    for (size_t i = 0; i < sum->texts.n; i++) strlist_add(&texts, sum->texts.v[i]);
    size_t dim;
    double *vecs = embed(e, &texts, &dim, err);
    strlist_free(&texts);
    if (!vecs) return -1;
    example *ex;
    size_t nex = read_examples(h, e->model_name, &ex);
    for (size_t f = 0; f < sum->folders.n; f++) {
        const double *fv = vecs + (v->names.n + f) * dim;
        size_t n = v->names.n;
        ranked *r = xmalloc((n + 64) * sizeof *r);
        for (size_t t = 0; t < n; t++) {
            r[t].tag = v->names.v[t];
            r[t].score = dot(fv, vecs + t * dim, dim);
            r[t].rule = 0;
            r[t].order = (int)t;
        }
        for (size_t x = 0; x < nex; x++) {          /* folders very like ones tagged before */
            if (ex[x].dim != dim) continue;
            double sim = dot(fv, ex[x].vec, dim);
            if (sim < EXAMPLE_MIN) continue;
            for (size_t k = 0; k < ex[x].tags.n; k++) {
                size_t t = 0;
                while (t < n && strcmp(r[t].tag, ex[x].tags.v[k])) t++;
                if (t == n) {
                    r = xrealloc(r, (n + 1) * sizeof *r);
                    r[n].tag = ex[x].tags.v[k];
                    r[n].score = -1;
                    r[n].rule = 0;
                    r[n].order = (int)n;
                    n++;
                }
                if (sim - EXAMPLE_SHIFT > r[t].score) r[t].score = sim - EXAMPLE_SHIFT;
            }
        }
        qsort(r, n, sizeof *r, by_score);
        double best = n ? r[0].score : 0;
        for (size_t t = 0; t < n && (int)t < top; t++)
            if (r[t].score >= best - 0.05) add_ranked(out, sum->folders.v[f], r[t].tag, floor(r[t].score * 1000 + 0.5) / 1000, 0);
        free(r);
    }
    for (size_t x = 0; x < nex; x++) {
        free(ex[x].vec);
        strlist_free(&ex[x].tags);
    }
    free(ex);
    free(vecs);
    return 0;
}

/* ------------------------------------------------------------------ reviewing */

static void review(const suggestions *s, int interactive, const tag_vocab *v, ftags *accepted)
{
    memset(accepted, 0, sizeof *accepted);
    if (interactive)
        fputs("Suggested tags. [Enter] accept, type tags (comma separated) to replace, '-' for none, 'a' to accept all the rest.\n", stderr);
    int rest = 0;
    for (size_t f = 0; f < s->count; f++) {
        strlist tags = { 0 }, fixed;
        for (size_t k = 0; k < s->n[f]; k++) strlist_add(&tags, s->v[f][k].tag);
        if (interactive && !rest) {
            fprintf(stderr, "\n  %s/\n    ", s->folder[f]);
            for (size_t k = 0; k < s->n[f]; k++) {
                if (s->v[f][k].rule) fprintf(stderr, "%s%s (rule)", k ? ", " : "", s->v[f][k].tag);
                else fprintf(stderr, "%s%s (%.2f)", k ? ", " : "", s->v[f][k].tag, s->v[f][k].score);
            }
            fputs("\n", stderr);
            char *a = ask("  > ");
            if (!strcmp(a, "a")) rest = 1;
            else if (!strcmp(a, "-")) strlist_free(&tags);
            else if (*a) {
                strlist_free(&tags);
                char *copy = xstrdup(a);
                for (char *t = copy, *next; t; t = next) {
                    next = strchr(t, ',');
                    if (next) *next++ = 0;
                    strlist_add(&tags, t);
                }
                free(copy);
            }
            free(a);
        }
        canonical_list(v, &tags, &fixed);
        strlist *dst = ftags_get(accepted, s->folder[f], 1);
        *dst = fixed;
        strlist_free(&tags);
    }
}

static int same_tags(const ftags *a, const ftags *b)
{
    if (a->n != b->n) return 0;
    for (size_t i = 0; i < a->n; i++) {
        if (strcmp(a->folder[i], b->folder[i]) || a->tags[i].n != b->tags[i].n) return 0;
        for (size_t k = 0; k < a->tags[i].n; k++)
            if (strcmp(a->tags[i].v[k], b->tags[i].v[k])) return 0;
    }
    return 1;
}

static const char *weakest(const char *a, const char *b)       /* the least reviewed part decides */
{
    static const char *const order[] = { "suggested", "automatic", "accepted", "edited", "human", NULL };
    int ia = 0, ib = 0;
    for (int i = 0; order[i]; i++) {
        if (!strcmp(a, order[i])) ia = i;
        if (!strcmp(b, order[i])) ib = i;
    }
    return ia <= ib ? a : b;
}

static jv *tags_json(const ftags *f, int skip_empty)
{
    jv *o = jobj();
    for (size_t i = 0; i < f->n; i++)
        if (!skip_empty || f->tags[i].n) jput(o, f->folder[i], jstrings(&f->tags[i]));
    return o;
}

/* ------------------------------------------------------------------ arv tag */

int assist_tag(int argc, char **argv)
{
    const char *tgt = NULL, *vocab_file = NULL, *save = NULL, *disc_root = NULL, *binary = NULL, *model = "bge-small-en-v1.5";
    const char *model_path = NULL, *embed_url = NULL, *embed_model = NULL, *home = NULL;
    int top = 3, apply = 0, show = 0, rules_only = 0, allow_remote = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        const char **str = !strcmp(a, "--vocab") ? &vocab_file : !strcmp(a, "--save") ? &save : !strcmp(a, "--disc-root") ? &disc_root
            : !strcmp(a, "--llama-embedding") ? &binary : !strcmp(a, "--model") ? &model : !strcmp(a, "--model-file") ? &model_path
            : !strcmp(a, "--embed-url") ? &embed_url : !strcmp(a, "--embed-model") ? &embed_model : !strcmp(a, "--home") ? &home : NULL;
        if (str && v) { *str = v; i++; }
        else if (!strcmp(a, "--top") && v) { top = atoi(v); i++; }
        else if (!strcmp(a, "--apply")) apply = 1;
        else if (!strcmp(a, "--show-summaries")) show = 1;
        else if (!strcmp(a, "--rules-only")) rules_only = 1;
        else if (!strcmp(a, "--llm-allow-remote")) allow_remote = 1;
        else if (a[0] != '-' && !tgt) tgt = a;
        else return 2;
    }
    if (!tgt) return 2;
    arv_home h;
    archive cat;
    home_find(&h, home, NULL);
    archive_load(&cat, h.rec_path);
    rec_record *disc = archive_disc(&cat, tgt);
    items files;
    char *text_root = NULL;
    ftags captions = { 0 };
    struct stat st;
    if (disc) {
        items_of_disc(&h, rec_get(disc, "Id"), &files);
        if (disc_root) text_root = join(disc_root, "data");
        char *tf = home_volume_file(&h, rec_get(disc, "Id"), "tags.tsv");
        read_tags_file(tf, &captions);
        free(tf);
    } else if (!stat(tgt, &st) && S_ISDIR(st.st_mode)) {
        text_root = abs_path(tgt);
        items_of_folder(text_root, &files);
    } else {
        die("%s is neither a disc id in the catalogue nor a folder", tgt);
    }
    tag_vocab v;
    load_vocab(&h, vocab_file, &v);
    summaries sum;
    summarise(&files, text_root, &captions, &sum);
    if (show) {
        for (size_t f = 0; f < sum.folders.n; f++) printf("%s\n    %s\n", sum.folders.v[f], sum.texts.v[f]);
        return 0;
    }
    suggestions sug = { 0 };
    rule_tags(&v, &files, &sum, &sug);          /* deterministic, and need no model */
    embedder e = { 0 };
    char *err = NULL;
    llm_client server;
    if (!rules_only) {
        if (embed_url) {
            if (llm_open(&server, embed_url, embed_model, allow_remote, &err)) die("%s", err);
            const char *m = llm_model(&server, &err);
            if (!m) die("%s", err);
            e.server = &server;
            e.model_name = xstrdup(m);
            e.prefix = xstrdup(model_query_prefix(m));
        } else {
            e.binary = find_runtime(&h, binary);
            if (!e.binary)
                die("%s", "llama.cpp's llama-embedding was not found. Install llama.cpp (it is in Homebrew and many Linux "
                          "distributions), pass --llama-embedding PATH, run 'arv models build-runtime', or use an embeddings "
                          "server with --embed-url");
            if (model_path) {           /* a model of your own choosing */
                const char *slash = strrchr(model_path, '/');
                e.model_path = xstrdup(model_path);
                e.model_name = xstrdup(slash ? slash + 1 : model_path);
                e.prefix = xstrdup("");
            } else {
                e.model_path = model_file(&h, model);
                if (!e.model_path) die("unknown model %s (known: bge-small-en-v1.5; or --model-file PATH)", model);
                if (access(e.model_path, R_OK)) die("model %s is not downloaded yet: run 'arv models fetch'", model);
                e.model_name = xstrdup(model);
                e.prefix = xstrdup(model_query_prefix(model));
            }
        }
        if (model_tags(&e, &h, &v, &sum, top, &sug, &err)) die("%s", err);
    }
    /* in the summaries' folder order */
    suggestions ordered = { 0 };
    for (size_t f = 0; f < sum.folders.n; f++) {
        size_t *n;
        ranked *r = slot(&sug, sum.folders.v[f], 0, &n);
        for (size_t k = 0; r && k < *n; k++) add_ranked(&ordered, sum.folders.v[f], r[k].tag, r[k].score, r[k].rule);
    }
    int interactive = isatty(0);
    ftags accepted, offered;
    review(&ordered, interactive, &v, &accepted);
    review(&ordered, 0, &v, &offered);
    const char *how = !interactive ? "suggested" : same_tags(&accepted, &offered) ? "accepted" : "edited";
    if (interactive && e.model_name) remember(&h, &e, &sum, &accepted, &err);
    char *agent = e.model_name ? xprintf("%sembeddings:%s", v.rule_tag.n ? "match rules + " : "", e.model_name) : xstrdup("match rules");
    if (!e.model_name) {            /* rules alone are software, not a model */
        if (!interactive) how = "automatic";
        else if (strcmp(how, "accepted")) how = "human";
    }
    if (save) {
        jv *d = NULL;
        char *old = access(save, F_OK) ? NULL : read_text(save);
        if (old && (d = json_parse(old)) && d->kind == 'o') {     /* merged into a draft (from arv describe, say) */
            jv *ft = (jv *)json_get(d, "folder_tags");
            if (!ft || ft->kind != 'o') { ft = jobj(); jput(d, "folder_tags", ft); }
            for (size_t i = 0; i < accepted.n; i++)
                if (accepted.tags[i].n) jput(ft, accepted.folder[i], jstrings(&accepted.tags[i]));
            const char *oa = jstr_of(json_get(d, "agent")), *oh = jstr_of(json_get(d, "authorship"));
            char *na = xprintf("%s; %s", oa ? oa : "draft", agent);
            jput(d, "agent", jstr(na));
            jput(d, "authorship", jstr(weakest(oh ? oh : "suggested", how)));
            free(na);
        } else {
            jfree(d);
            d = jobj();
            jput(d, "folder_tags", tags_json(&accepted, 1));
            jput(d, "agent", jstr(agent));
            jput(d, "authorship", jstr(how));
        }
        free(old);
        save_json(save, d);
        jfree(d);
        fprintf(stderr, "Saved to %s (use: arv make --draft %s ...)\n", save, save);
    } else if (disc && (apply || interactive)) {
        const char *id = rec_get(disc, "Id");
        char *tf = home_volume_file(&h, id, "tags.tsv");
        ftags merged;
        read_tags_file(tf, &merged);
        size_t folders = 0;
        for (size_t i = 0; i < accepted.n; i++) {
            if (!accepted.tags[i].n) continue;
            folders++;
            strlist *t = ftags_get(&merged, accepted.folder[i], 1);
            strlist_free(t);
            for (size_t k = 0; k < accepted.tags[i].n; k++) strlist_add(t, accepted.tags[i].v[k]);
        }
        write_tags_file(tf, &merged);
        char *note = xprintf("folder tags for %zu folders from the tag vocabulary", folders);
        recs_add(&cat.events, reviewed_event(id, agent, how, note));
        archive_save(&cat, h.rec_path);
        fprintf(stderr, "Updated tags of %s.\n", id);
        free(note);
        free(tf);
    } else {
        jv *d = jobj();
        jput(d, "folder_tags", tags_json(&accepted, 0));
        jput(d, "agent", jstr(agent));
        jput(d, "authorship", jstr(how));
        sbuf b = { 0 };
        json_dump(&b, d, 2);
        printf("%s\n", b.s);
        free(b.s);
        jfree(d);
    }
    return 0;
}
