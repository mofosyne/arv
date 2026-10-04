/*
 * arv tags and arv keywords (cli.cmd_tags, cli.cmd_keywords): the folder tags in the catalogue,
 * grouped by namespace, and one disc's set paths and folder tags as hierarchical keywords (XMP
 * lr:hierarchicalSubject, as Lightroom and digiKam write them).
 */
#define _XOPEN_SOURCE 700
#include "arvc.h"
#include "data.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void open_home(const char *given, arv_home *h, archive *cat)
{
    home_find(h, given, NULL);
    archive_load(cat, h->rec_path);
}

/* catalog.normalise_tag: 'Place : Kyoto ' -> 'place:kyoto' (letter case folded for ASCII only) */
char *tag_normalise(const char *tag)
{
    sbuf b = { 0 };
    int space = 0;
    for (const char *s = tag ? tag : ""; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == ',' || isspace(c)) {
            space = b.len > 0;
            continue;
        }
        if (space) sb_puts(&b, " ");
        space = 0;
        char one[2] = { (char)tolower(c), 0 };
        sb_puts(&b, one);
    }
    if (!b.s) return xstrdup("");
    char *colon = strchr(b.s, ':');
    if (colon) {
        char *ns = xstrdup(b.s), *value = colon + 1;
        ns[colon - b.s] = 0;
        char *a = ns, *e = ns + strlen(ns);
        while (*a == ' ') a++;
        while (e > a && e[-1] == ' ') *--e = 0;
        while (*value == ' ') value++;
        char *ve = value + strlen(value);
        while (ve > value && ve[-1] == ' ') ve--;
        if (*a && !strchr(a, ' ')) {
            char *out = xprintf("%s:%.*s", a, (int)(ve - value), value);
            free(ns);
            free(b.s);
            return out;
        }
        free(ns);
    }
    return b.s;
}

/* catalog.split_tag: the namespace's length ("place:kyoto" -> 5), 0 for a plain tag */
static size_t ns_len(const char *tag)
{
    const char *colon = strchr(tag, ':');
    if (!colon || colon == tag) return 0;
    for (const char *p = tag; p < colon; p++)
        if (*p == ' ') return 0;
    return (size_t)(colon - tag);
}

/* catalog.hierarchical: 'place:kyoto' -> 'place|kyoto' */
static char *hierarchical(const char *tag)
{
    size_t n = ns_len(tag);
    return n ? xprintf("%.*s|%s", (int)n, tag, tag + n + 1) : xstrdup(tag);
}

typedef struct {
    char *folder;
    strlist tags;
} folder_tags;

typedef struct {
    folder_tags *v;
    size_t n;
} tag_file;

/* catalog.read_tags: folder <TAB> comma-separated tags [<TAB> caption]; a folder listed twice
 * keeps its last line, in the place of its first */
static void read_tags(const char *path, tag_file *out)
{
    memset(out, 0, sizeof *out);
    FILE *fp = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while (fp && (len = getline(&line, &cap, fp)) >= 0) {
        if (len > 0 && line[len - 1] == '\n') line[--len] = 0;
        char *tab = strchr(line, '\t');
        if (!*line || *line == '#' || !tab) continue;
        *tab = 0;
        char *rest = tab + 1, *tab2 = strchr(rest, '\t');
        if (tab2) *tab2 = 0;
        folder_tags *ft = NULL;
        for (size_t i = 0; i < out->n && !ft; i++)
            if (!strcmp(out->v[i].folder, line)) ft = &out->v[i];
        if (ft) {
            strlist_free(&ft->tags);
        } else {
            out->v = xrealloc(out->v, (out->n + 1) * sizeof *out->v);
            ft = &out->v[out->n++];
            ft->folder = xstrdup(line);
        }
        memset(&ft->tags, 0, sizeof ft->tags);
        for (char *t = strtok(rest, ","); t; t = strtok(NULL, ",")) {
            while (*t && isspace((unsigned char)*t)) t++;
            char *e = t + strlen(t);
            while (e > t && isspace((unsigned char)e[-1])) *--e = 0;
            if (*t) strlist_add(&ft->tags, t);
        }
    }
    free(line);
    if (fp) fclose(fp);
}

static void tag_file_free(tag_file *f)
{
    for (size_t i = 0; i < f->n; i++) {
        free(f->v[i].folder);
        strlist_free(&f->v[i].tags);
    }
    free(f->v);
}

/* the tag vocabulary: names and their aliases (tagger.TagVocab) */
typedef struct {
    strlist names, alias_from, alias_to;
    char *path;
} tag_vocab;

static int load_tag_vocab(tag_vocab *tv, const arv_home *h, const char *given)
{
    memset(tv, 0, sizeof *tv);
    tv->path = given ? xstrdup(given) : join(h->config_dir, "tags.rec");
    if (!given && access(tv->path, F_OK) && !mkdirs(h->config_dir)) write_text(tv->path, DATA_DEFAULT_TAGS);
    rec_file f;
    int bad = 0;
    if (rec_read(tv->path, &f, &bad) && rec_parse(given ? "" : DATA_DEFAULT_TAGS, &f, &bad)) return -1;
    for (size_t i = 0; i < f.nrecords; i++) {
        const rec_record *r = &f.records[i];
        if (r->descriptor || !rec_get(r, "Name")) continue;
        char *name = tag_normalise(rec_get(r, "Name"));
        strlist_add(&tv->names, name);
        for (size_t j = 0; j < r->nfields; j++)
            if (!strcmp(r->fields[j].name, "Alias")) {
                char *a = tag_normalise(r->fields[j].value);
                strlist_add(&tv->alias_from, a);
                strlist_add(&tv->alias_to, name);
                free(a);
            }
        free(name);
    }
    rec_free(&f);
    return tv->names.n ? 0 : -1;
}

/* the vocabulary's names for tags, without repeats or blanks (tagger.TagVocab.canonical_list); the
 * tags as they are when the home's vocabulary has no tags */
void tags_canonical(const arv_home *h, strlist *tags)
{
    tag_vocab tv;
    if (load_tag_vocab(&tv, h, NULL)) return;
    strlist out = { 0 };
    for (size_t i = 0; i < tags->n; i++) {
        char *t = tag_normalise(tags->v[i]);
        const char *canon = t;
        for (size_t a = 0; a < tv.alias_from.n; a++)
            if (!strcmp(tv.alias_from.v[a], t)) { canon = tv.alias_to.v[a]; break; }
        if (*canon && !strlist_has(&out, canon)) strlist_add(&out, canon);
        free(t);
    }
    strlist_free(tags);
    *tags = out;
}

static int by_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* printf("%-*s") counting characters, not bytes, as Python does */
static void print_padded(const char *s, size_t width)
{
    size_t chars = 0;
    for (const char *p = s; *p; p++) chars += ((unsigned char)*p & 0xC0) != 0x80;
    fputs(s, stdout);
    for (; chars < width; chars++) putchar(' ');
}

typedef struct {
    char *tag;
    strlist folders, discs;     /* "disc\tfolder" pairs; disc ids */
} usage;

static int by_tag(const void *a, const void *b)
{
    return strcmp((*(usage *const *)a)->tag, (*(usage *const *)b)->tag);
}

/* arvc tags [--namespace NS] [--vocab FILE] */
int cmd_tags(int argc, char **argv)
{
    const char *given = NULL, *only = NULL, *vocab_file = NULL;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--namespace")) only = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--vocab")) vocab_file = argv[++i];
        else return 2;
    }
    arv_home h;
    archive cat;
    open_home(given, &h, &cat);
    tag_vocab tv;
    int have_vocab = !load_tag_vocab(&tv, &h, vocab_file);
    usage *u = NULL;
    size_t nu = 0;
    for (size_t i = 0; i < cat.discs.n; i++) {
        const char *id = rec_get(cat.discs.v[i], "Id");
        if (!id) continue;
        char *path = home_volume_file(&h, id, "tags.tsv");
        tag_file tf;
        read_tags(path, &tf);
        for (size_t k = 0; k < tf.n; k++)
            for (size_t t = 0; t < tf.v[k].tags.n; t++) {
                const char *tag = tf.v[k].tags.v[t];
                usage *x = NULL;
                for (size_t j = 0; j < nu && !x; j++)
                    if (!strcmp(u[j].tag, tag)) x = &u[j];
                if (!x) {
                    u = xrealloc(u, (nu + 1) * sizeof *u);
                    x = &u[nu++];
                    memset(x, 0, sizeof *x);
                    x->tag = xstrdup(tag);
                }
                char *pair = xprintf("%s\t%s", id, tf.v[k].folder);
                if (!strlist_has(&x->folders, pair)) strlist_add(&x->folders, pair);
                free(pair);
                if (!strlist_has(&x->discs, id)) strlist_add(&x->discs, id);
            }
        tag_file_free(&tf);
        free(path);
    }
    strlist spaces = { 0 };                        /* the namespaces in use, sorted */
    for (size_t j = 0; j < nu; j++) {
        char *ns = xprintf("%.*s", (int)ns_len(u[j].tag), u[j].tag);
        if (!strlist_has(&spaces, ns)) strlist_add(&spaces, ns);
        free(ns);
    }
    if (only) {
        strlist_free(&spaces);
        strlist_add(&spaces, only);
    }
    if (spaces.n) qsort(spaces.v, spaces.n, sizeof *spaces.v, by_str);
    for (size_t s = 0; s < spaces.n; s++) {
        const char *ns = spaces.v[s];
        if (*ns) printf("%s:\n", ns);
        else puts("(no namespace)");
        usage **mine = xmalloc((nu + 1) * sizeof *mine);
        size_t n = 0;
        for (size_t j = 0; j < nu; j++) {
            size_t l = ns_len(u[j].tag);
            if (l == strlen(ns) && !strncmp(u[j].tag, ns, l)) mine[n++] = &u[j];
        }
        if (n) qsort(mine, n, sizeof *mine, by_tag);
        for (size_t k = 0; k < n; k++) {
            const usage *x = mine[k];
            fputs("  ", stdout);
            print_padded(*ns ? x->tag + strlen(ns) + 1 : x->tag, 28);
            printf(" %zu folder%s on %zu disc%s", x->folders.n, x->folders.n == 1 ? "" : "s", x->discs.n,
                   x->discs.n == 1 ? "" : "s");
            if (have_vocab && !*ns && !strlist_has(&tv.names, x->tag)) {
                char *norm = tag_normalise(x->tag);
                const char *canon = norm;
                for (size_t a = 0; a < tv.alias_from.n; a++)
                    if (!strcmp(tv.alias_from.v[a], norm)) { canon = tv.alias_to.v[a]; break; }
                if (strcmp(canon, x->tag)) printf("   (alias of %s)", canon);
                else printf("   (not in %s)", tv.path);
                free(norm);
            }
            putchar('\n');
        }
        free(mine);
    }
    return 0;
}

/* arvc keywords [--format tsv|exiftool] DISC-ID */
int cmd_keywords(int argc, char **argv)
{
    const char *given = NULL, *disc_id = NULL, *format = "tsv";
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--format")) format = argv[++i];
        else if (!disc_id && argv[i][0] != '-') disc_id = argv[i];
        else return 2;
    }
    if (!disc_id || (strcmp(format, "tsv") && strcmp(format, "exiftool"))) return 2;
    arv_home h;
    archive cat;
    open_home(given, &h, &cat);
    const rec_record *d = archive_disc(&cat, disc_id);
    if (!d) {
        fprintf(stderr, "Error: no disc %s in %s\n", disc_id, h.rec_path);
        return 1;
    }
    /* {folder: keywords}, "." (the whole disc: its set paths, or its set) first */
    tag_file kw = { 0 };
    kw.v = xmalloc(sizeof *kw.v);
    kw.n = 1;
    kw.v[0].folder = xstrdup(".");
    memset(&kw.v[0].tags, 0, sizeof kw.v[0].tags);
    for (size_t j = 0; j < d->nfields; j++)
        if (!strcmp(d->fields[j].name, "Path")) {
            char *p = xstrdup(d->fields[j].value);
            for (char *c = p; *c; c++)
                if (*c == '/') *c = '|';
            strlist_add(&kw.v[0].tags, p);
            free(p);
        }
    if (!kw.v[0].tags.n) strlist_add(&kw.v[0].tags, rec_get(d, "Set") ? rec_get(d, "Set") : "None");
    char *path = home_volume_file(&h, disc_id, "tags.tsv");
    tag_file tf;
    read_tags(path, &tf);
    for (size_t k = 0; k < tf.n; k++) {
        folder_tags *ft = NULL;
        for (size_t i = 0; i < kw.n && !ft; i++)
            if (!strcmp(kw.v[i].folder, tf.v[k].folder)) ft = &kw.v[i];
        if (!ft) {
            kw.v = xrealloc(kw.v, (kw.n + 1) * sizeof *kw.v);
            ft = &kw.v[kw.n++];
            ft->folder = xstrdup(tf.v[k].folder);
            memset(&ft->tags, 0, sizeof ft->tags);
        }
        for (size_t t = 0; t < tf.v[k].tags.n; t++) {
            char *hw = hierarchical(tf.v[k].tags.v[t]);
            strlist_add(&ft->tags, hw);
            free(hw);
        }
    }
    tag_file_free(&tf);
    free(path);
    if (!strcmp(format, "tsv")) {
        puts("# folder (relative to data/)\thierarchical keywords (| between levels)");
        for (size_t i = 0; i < kw.n; i++) {
            printf("%s\t", kw.v[i].folder);
            for (size_t t = 0; t < kw.v[i].tags.n; t++) printf("%s%s", t ? ", " : "", kw.v[i].tags.v[t]);
            putchar('\n');
        }
    } else {
        printf("# exiftool argument file for disc %s: from the root of a restored copy, run\n", rec_get(d, "Id"));
        puts("#   exiftool -@ this-file");
        puts("# Each section adds the keywords to every file under one folder (-r). Removing each value");
        puts("# before adding it keeps a second run from adding it twice.");
        for (size_t i = 0; i < kw.n; i++) {
            puts("-r");
            puts("-overwrite_original");
            for (size_t t = 0; t < kw.v[i].tags.n; t++) {
                const char *w = kw.v[i].tags.v[t], *bar = strrchr(w, '|'), *leaf = bar ? bar + 1 : w;
                printf("-XMP-lr:HierarchicalSubject-=%s\n-XMP-lr:HierarchicalSubject+=%s\n", w, w);
                printf("-XMP-dc:Subject-=%s\n-XMP-dc:Subject+=%s\n", leaf, leaf);
            }
            if (!strcmp(kw.v[i].folder, ".")) puts("data");
            else printf("data/%s\n", kw.v[i].folder);
            puts("-execute");
        }
    }
    tag_file_free(&kw);
    return 0;
}
