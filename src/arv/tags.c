/*
 * arv tags and arv keywords (cli.cmd_tags, cli.cmd_keywords): the folder tags in the catalogue,
 * grouped by namespace, and one disc's set paths and folder tags as hierarchical keywords (XMP
 * lr:hierarchicalSubject, as Lightroom and digiKam write them).
 */
#define _XOPEN_SOURCE 700
#include "arv.h"
#include "data.h"
#include "edtf.h"

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
} path_tags;

typedef struct {
    path_tags *v;
    size_t n;
} tag_file;

/* a disc's tags now (its tags.tsv as burned, and the changes since): path <TAB> comma-separated
 * tags [<TAB> caption]; a path listed twice keeps its last line, in the place of its first */
static void read_tags(const archive *cat, const char *disc_id, const char *path, tag_file *out)
{
    memset(out, 0, sizeof *out);
    FILE *fp = disc_tags_open(cat, disc_id, path);
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
        path_tags *ft = NULL;
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
    strlist ns_names, ns_desc, ns_one;       /* declared namespaces; ns_one holds the one-valued */
    strlist ns_file_date;                   /* and those whose values may come from a file's date */
    char *path;
} tag_vocab;

static int load_tag_vocab(tag_vocab *tv, const arv_home *h)
{
    memset(tv, 0, sizeof *tv);
    tv->path = join(h->config_dir, "tags.rec");
    if (access(tv->path, F_OK) && !mkdirs(h->config_dir)) write_text(tv->path, DATA_DEFAULT_TAGS);
    rec_file f;
    int bad = 0;
    if (rec_read(tv->path, &f, &bad) && rec_parse(DATA_DEFAULT_TAGS, &f, &bad)) return -1;
    for (size_t i = 0; i < f.nrecords; i++) {
        const rec_record *r = &f.records[i];
        if (r->descriptor || !rec_get(r, "Name")) continue;
        if (r->type && !strcmp(r->type, "Namespace")) {
            const char *ns = rec_get(r, "Name"), *d = rec_get(r, "Description"), *v = rec_get(r, "Values");
            if (strlist_has(&tv->ns_names, ns)) continue;
            strlist_add(&tv->ns_names, ns);
            strlist_add(&tv->ns_desc, d ? d : "");
            if (v && !strcmp(v, "one")) strlist_add(&tv->ns_one, ns);
            for (size_t j = 0; j < r->nfields; j++)
                if (!strcmp(r->fields[j].name, "From") && !strcmp(r->fields[j].value, "file-date"))
                    strlist_add(&tv->ns_file_date, ns);
            continue;
        }
        if (r->type && strcmp(r->type, "Tag")) continue;
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

/* the vocabulary's words, for questions about what something is (arv describe --ask): its tags,
 * their aliases (from -> to), its namespaces (with what each holds, and the one-valued), and each
 * tag's Match patterns (rule_tag, rule_glob); -1 when there is no vocabulary */
int tag_words(const arv_home *h, tag_words_t *out)
{
    memset(out, 0, sizeof *out);
    tag_vocab tv;
    if (load_tag_vocab(&tv, h)) return -1;
    out->names = tv.names;
    out->alias_from = tv.alias_from;
    out->alias_to = tv.alias_to;
    out->ns_names = tv.ns_names;
    out->ns_desc = tv.ns_desc;
    out->ns_one = tv.ns_one;
    rec_file f;
    int bad = 0;
    if (!rec_read(tv.path, &f, &bad)) {
        for (size_t i = 0; i < f.nrecords; i++) {
            const rec_record *r = &f.records[i];
            if (r->descriptor || !rec_get(r, "Name") || (r->type && strcmp(r->type, "Tag"))) continue;
            char *name = tag_normalise(rec_get(r, "Name"));
            for (size_t j = 0; j < r->nfields; j++)
                if (!strcmp(r->fields[j].name, "Match") && *r->fields[j].value) {
                    strlist_add(&out->rule_tag, name);
                    strlist_add(&out->rule_glob, r->fields[j].value);
                }
            free(name);
        }
        rec_free(&f);
    }
    free(tv.path);
    return 0;
}

/* the home's declared namespaces that take one value a file (into one), and whether date's values
 * may come from a file's own date (From: file-date); -1 when there is no vocabulary */
int tag_namespaces(const arv_home *h, strlist *one, strlist *declared, int *date_from_file)
{
    tag_vocab tv;
    if (load_tag_vocab(&tv, h)) return -1;
    for (size_t i = 0; i < tv.ns_one.n; i++) strlist_add(one, tv.ns_one.v[i]);
    if (declared)
        for (size_t i = 0; i < tv.ns_names.n; i++) strlist_add(declared, tv.ns_names.v[i]);
    if (date_from_file) *date_from_file = strlist_has(&tv.ns_file_date, "date");
    return 0;
}

/* the vocabulary's names for tags, without repeats or blanks (tagger.TagVocab.canonical_list); the
 * tags as they are when the home's vocabulary has no tags */
void tags_canonical(const arv_home *h, strlist *tags)
{
    tag_vocab tv;
    if (load_tag_vocab(&tv, h)) return;
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

/* printf("%-*s") counts bytes; this width is in UTF-8 characters */
static void print_padded(const char *s, size_t width)
{
    size_t chars = 0;
    for (const char *p = s; *p; p++) chars += ((unsigned char)*p & 0xC0) != 0x80;
    fputs(s, stdout);
    for (; chars < width; chars++) putchar(' ');
}

typedef struct {
    char *tag;
    strlist folders, files, discs;  /* "disc\tpath" pairs, for folders and for files; disc ids */
} usage;

static int by_tag(const void *a, const void *b)
{
    return strcmp((*(usage *const *)a)->tag, (*(usage *const *)b)->tag);
}

/* a disc's tags against the declared namespaces: a path given two values of a one-valued namespace
 * (its own value replaces its folder's, so only its own count), and a date that is not EDTF */
static void check_namespaces(const tag_vocab *tv, const char *id, const tag_file *tf, const char *only,
                             strlist *out)
{
    for (size_t k = 0; k < tf->n; k++) {
        const char *path = tf->v[k].folder;
        for (size_t s = 0; s < tv->ns_one.n; s++) {
            const char *ns = tv->ns_one.v[s];
            size_t l = strlen(ns);
            if (only && strcmp(only, ns)) continue;
            sbuf vals = { 0 };
            size_t n = 0;
            for (size_t t = 0; t < tf->v[k].tags.n; t++) {
                const char *tag = tf->v[k].tags.v[t];
                if (ns_len(tag) != l || strncmp(tag, ns, l)) continue;
                const char *v = tag + l + 1;
                while (*v == ' ') v++;
                sb_printf(&vals, "%s%s", n++ ? ", " : "", v);
            }
            if (n > 1) {
                char *line = xprintf("  TWO VALUES  %s  %s  %s: %s", id, path, ns, vals.s);
                strlist_add(out, line);
                free(line);
            }
            free(vals.s);
        }
        if (only && strcmp(only, "date")) continue;
        for (size_t t = 0; t < tf->v[k].tags.n; t++) {
            const char *tag = tf->v[k].tags.v[t];
            if (ns_len(tag) != 4 || strncmp(tag, "date", 4)) continue;
            const char *v = tag + 5;
            while (*v == ' ') v++;
            long a, b;
            if (edtf_span(v, &a, &b) == 1) continue;
            char *line = xprintf("  BAD DATE    %s  %s  %s", id, path, tag);
            strlist_add(out, line);
            free(line);
        }
    }
}

/* arv tags [--namespace NS] */
int cmd_tags(int argc, char **argv)
{
    const char *given = NULL, *only = NULL;
    static const char *const SUBS[] = { "show", "add", "label", "alias", "merge", "split", "retire", NULL };
    for (int i = 0; i < argc; i++) {             /* known names: arv tags show|add|label|alias|merge|split|retire KEY ... */
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) {
            given = argv[++i];
            continue;
        }
        for (int k = 0; SUBS[k]; k++)
            if (!strcmp(argv[i], SUBS[k])) {
                char **rest = xmalloc(((size_t)argc + 1) * sizeof *rest);
                int n = 0;
                for (int j = i + 1; j < argc; j++) {
                    if (j + 1 < argc && (!strcmp(argv[j], "-C") || !strcmp(argv[j], "--home"))) given = argv[++j];
                    else rest[n++] = argv[j];
                }
                rest[n] = NULL;
                int rc = known_cmd(SUBS[k], n, rest, given);
                free(rest);
                return rc;
            }
        break;
    }
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--namespace")) only = argv[++i];
        else return 2;
    }
    arv_home h;
    archive cat;
    open_home(given, &h, &cat);
    tag_vocab tv;
    int have_vocab = !load_tag_vocab(&tv, &h);
    usage *u = NULL;
    size_t nu = 0;
    strlist problems = { 0 };
    for (size_t i = 0; i < cat.discs.n; i++) {
        const char *id = rec_get(cat.discs.v[i], "Id");
        if (!id) continue;
        char *path = home_volume_file(&h, id, "tags.tsv"), *lst = home_volume_file(&h, id, "listing.tsv");
        tag_file tf;
        read_tags(&cat, id, path, &tf);
        if (have_vocab) check_namespaces(&tv, id, &tf, only, &problems);
        for (size_t k = 0; k < tf.n; k++) {
            int file = strcmp(tf.v[k].folder, ".") && listing_has_file(lst, tf.v[k].folder);
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
                strlist *into = file ? &x->files : &x->folders;
                if (!strlist_has(into, pair)) strlist_add(into, pair);
                free(pair);
                if (!strlist_has(&x->discs, id)) strlist_add(&x->discs, id);
            }
        }
        tag_file_free(&tf);
        free(path);
        free(lst);
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
    known_entry *kv;                               /* the known names: their labels, beside their keys */
    size_t kn = known_load(&h, &kv);
    for (size_t s = 0; s < spaces.n; s++) {
        const char *ns = spaces.v[s];
        if (!*ns) puts("(no namespace)");
        else if (!have_vocab) printf("%s:\n", ns);
        else {
            size_t d = 0;
            while (d < tv.ns_names.n && strcmp(tv.ns_names.v[d], ns)) d++;
            if (d == tv.ns_names.n) printf("%s:   (not declared in %s)\n", ns, tv.path);
            else {
                printf("%s:   %s", ns, strlist_has(&tv.ns_one, ns) ? "one value" : "many values");
                if (*tv.ns_desc.v[d]) printf(": %s", tv.ns_desc.v[d]);
                putchar('\n');
            }
        }
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
            if (x->folders.n) printf(" %zu folder%s", x->folders.n, x->folders.n == 1 ? "" : "s");
            if (x->files.n) printf("%s %zu file%s", x->folders.n ? "," : "", x->files.n, x->files.n == 1 ? "" : "s");
            printf(" on %zu disc%s", x->discs.n, x->discs.n == 1 ? "" : "s");
            for (size_t e = 0; *ns && e < kn; e++)
                if (!strcmp(kv[e].key, x->tag))
                    printf("   %s%s%s%s%s%s", kv[e].label ? kv[e].label : "", kv[e].qualifier ? " (" : "", kv[e].qualifier ? kv[e].qualifier : "",
                           kv[e].qualifier ? ")" : "", kv[e].private_ ? ", private" : "", kv[e].retired ? ", RETIRED" : "");
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
    known_free(kv, kn);
    if (problems.n) {
        printf("\n%zu to correct (a one-valued namespace has one value a file, and a date is EDTF: 2019-07, 1995/2008):\n",
               problems.n);
        for (size_t i = 0; i < problems.n; i++) puts(problems.v[i]);
        puts("Change the tags with arv-assist describe --apply or tag --apply; arv tags then checks again.");
    }
    return 0;
}

/* arv keywords DISC-ID */
int cmd_keywords(int argc, char **argv)
{
    const char *given = NULL, *disc_id = NULL;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!disc_id && argv[i][0] != '-') disc_id = argv[i];
        else return 2;
    }
    if (!disc_id) return 2;
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
    read_tags(&cat, disc_id, path, &tf);
    for (size_t k = 0; k < tf.n; k++) {
        path_tags *ft = NULL;
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
    printf("# exiftool argument file for disc %s: from the root of a restored copy, run\n", rec_get(d, "Id"));
    puts("#   exiftool -@ this-file");
    puts("# Each section adds the keywords to one file, or to every file under one folder (-r). Removing each value");
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
    tag_file_free(&kw);
    return 0;
}
