/*
 * Words and tags, counted (research/plan.md, "Describing without a model"): which words of names are
 * rare in this archive, and which go with which tags, so that arv describe --ask can mark the tags a
 * folder's words point to, with the counts that say why ("'kicad' in 9 of 10 folders tagged
 * electronics"). No model: counting, over what the record holds and what the person answered.
 *
 * - From the record (cache/words.tsv, rebuilt when the catalogue changes): every folder of every
 *   disc's listing, for how rare each word is; every path given tags (tags.tsv, objects' Tags),
 *   with the words of its names and its files' names, for which words go with which tags.
 * - From the answers (config/word-examples.tsv, home only): what --ask was told about a folder,
 *   with the words of its names and of its text files and PDFs, which the record does not keep.
 */
#include "assist.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define UNIT_FILES 300          /* the files of a tagged path whose names are counted */
#define CONTENT_WORDS 30        /* the words kept of a folder's text */
#define TEXT_FILES 20           /* the text files and PDFs read, spread over the folder */
#define TEXT_BYTES 65536

/* ------------------------------------------------------------------ a small map: text -> count */

typedef struct {
    char **keys;
    size_t *vals;
    size_t cap, n;
} counts;

static size_t hash_of(const char *s)
{
    size_t h = 1469598103934665603ULL;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ULL;
    return h;
}

static size_t *slot_of(counts *m, const char *key, int add)
{
    if (add && (m->n + 1) * 2 > m->cap) {   /* grow, and place every key again */
        size_t cap = m->cap ? m->cap * 2 : 1024;
        char **keys = xmalloc(cap * sizeof *keys);
        size_t *vals = xmalloc(cap * sizeof *vals);
        memset(keys, 0, cap * sizeof *keys);
        for (size_t i = 0; i < m->cap; i++)
            if (m->keys[i]) {
                size_t j = hash_of(m->keys[i]) & (cap - 1);
                while (keys[j]) j = (j + 1) & (cap - 1);
                keys[j] = m->keys[i];
                vals[j] = m->vals[i];
            }
        free(m->keys);
        free(m->vals);
        m->keys = keys;
        m->vals = vals;
        m->cap = cap;
    }
    if (!m->cap) return NULL;
    size_t j = hash_of(key) & (m->cap - 1);
    while (m->keys[j] && strcmp(m->keys[j], key)) j = (j + 1) & (m->cap - 1);
    if (!m->keys[j]) {
        if (!add) return NULL;
        m->keys[j] = xstrdup(key);
        m->vals[j] = 0;
        m->n++;
    }
    return &m->vals[j];
}

static void bump(counts *m, const char *key, size_t by)
{
    *slot_of(m, key, 1) += by;
}

static size_t count_of(const counts *m, const char *key)
{
    size_t *v = slot_of((counts *)m, key, 0);
    return v ? *v : 0;
}

static void counts_free(counts *m)
{
    for (size_t i = 0; i < m->cap; i++) free(m->keys[i]);
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

struct word_stats {
    size_t folders;             /* folders counted for rarity */
    size_t text_folders;        /* folders whose text's words were kept (the answers) */
    counts text_df;             /* word -> those whose text had it: the archive's own common words */
    counts keep;                /* the vocabulary's words: never common, whatever the lists say */
    counts tag_units;           /* tag -> tagged paths (and answers) with it */
    counts df;                  /* word -> folders whose names have it */
    counts units;               /* word -> tagged paths (and answers) whose words have it */
    counts pairs;               /* "word\ttag" -> those of them with the tag */
    strlist tags_of_word_key;   /* "word": the tags it was seen with, "tag,tag" (in tags_of_word_val) */
    strlist tags_of_word_val;
    counts tag_index;           /* word -> its index in tags_of_word_* + 1 */
};

/* ------------------------------------------------------------------ words of a name, and of a text */

static const char *const STOP[] = {
    /* names */
    "img", "dsc", "dcim", "pxl", "vid", "mvi", "jpg", "jpeg", "png", "pdf", "doc", "docx", "txt", "copy", "final",
    "new", "old", "misc", "other", "files", "file", "folder", "data", "backup", "untitled", "tmp", "temp", "export", "pdfs",
    "docs",
    /* text, English: the commonest words, which rarity alone would let through a short text */
    "the", "and", "for", "are", "but", "not", "you", "all", "any", "can", "had", "her", "was", "one", "our", "out",
    "has", "his", "how", "its", "may", "who", "did", "get", "him", "let", "say", "she", "too", "use", "that", "with",
    "have", "this", "will", "your", "from", "they", "been", "were", "what", "when", "there", "their", "which",
    "would", "about", "could", "other", "into", "than", "then", "them", "these", "some", "only", "also", "more",
    "such", "very", "should", "each", "where", "after", "before", "here", "just", "over", "upon", "shall", "being",
    "those", "while", "same", "does", "made", "make", "most", "must", "many", "much", "well", "page", "date", "dear",
    "yours", "please", "thank", "thanks", "regards", "sincerely", NULL
};

extern const char COMMON_WORDS[];          /* data/common-words.txt, built in (dev-tools/common-words.sh) */
static counts common;                       /* its words, read once */

static void common_load(void)
{
    if (common.n) return;
    for (const char *s = COMMON_WORDS, *e; *s; s = *e ? e + 1 : e) {
        e = strchr(s, '\n');
        if (!e) e = s + strlen(s);
        if (*s != '#' && e > s) {
            char *w = xprintf("%.*s", (int)(e - s), s);
            bump(&common, w, 1);
            free(w);
        }
    }
}

/* a word of a text that says nothing about what the folder holds: one of the language's commonest
 * (the list), or in most of the texts described before (the archive's own); never one of the
 * vocabulary's words */
static int text_common(const word_stats *ws, const char *w)
{
    if (ws && count_of(&ws->keep, w)) return 0;
    common_load();
    if (count_of(&common, w)) return 1;
    return ws && ws->text_folders >= 10 && count_of(&ws->text_df, w) * 2 > ws->text_folders;
}

void word_stats_vocab(word_stats *ws, const strlist *names, const strlist *aliases)
{
    const strlist *both[2] = { names, aliases };
    for (int k = 0; k < 2; k++)
        for (size_t i = 0; both[k] && i < both[k]->n; i++) {
            const char *w = strchr(both[k]->v[i], ':') ? strchr(both[k]->v[i], ':') + 1 : both[k]->v[i];
            bump(&ws->keep, w, 1);
        }
}

static int stop_word(const char *w)
{
    for (int i = 0; STOP[i]; i++)
        if (!strcmp(STOP[i], w)) return 1;
    return 0;
}

/* runs of letters, lower case, 3 to 30 of them; with pairs, each two in a row as "a b" too */
static void words_of(const char *text, size_t len, int pairs, strlist *out)
{
    sbuf w = { 0 };
    char *last = NULL;
    for (size_t i = 0;; i++) {
        unsigned char c = i < len ? (unsigned char)text[i] : 0;
        if (c && (isalpha(c) || c >= 0x80)) {
            char l = (char)tolower(c);
            if (w.len < 64) sb_add(&w, &l, 1);
            continue;
        }
        if (w.len >= 3 && w.len <= 30 && !stop_word(w.s)) {
            strlist_add(out, w.s);
            if (pairs && last) {
                char *p = xprintf("%s %s", last, w.s);
                strlist_add(out, p);
                free(p);
            }
            free(last);
            last = xstrdup(w.s);
        } else if (w.len) {
            free(last);
            last = NULL;
        }
        w.len = 0;
        if (w.s) w.s[0] = 0;
        if (i >= len) break;
    }
    free(last);
    free(w.s);
}

void name_word_list(const char *name, strlist *out)
{
    char *folded = text_fold(name);         /* as tags are: "Zürich" is "zurich" */
    words_of(folded, strlen(folded), 1, out);
    free(folded);
}

/* ------------------------------------------------------------------ counting the record */

static void unique(strlist *l)
{
    strlist u = { 0 };
    for (size_t i = 0; i < l->n; i++)
        if (!strlist_has(&u, l->v[i])) strlist_add(&u, l->v[i]);
    strlist_free(l);
    *l = u;
}

static void count_unit(word_stats *ws, strlist *words, const strlist *tags)
{
    unique(words);
    for (size_t t = 0; t < tags->n; t++) bump(&ws->tag_units, tags->v[t], 1);
    for (size_t i = 0; i < words->n; i++) {
        bump(&ws->units, words->v[i], 1);
        for (size_t t = 0; t < tags->n; t++) {
            char *k = xprintf("%s\t%s", words->v[i], tags->v[t]);
            bump(&ws->pairs, k, 1);
            free(k);
        }
    }
}

static void split_tags(const char *text, strlist *out)
{
    char *copy = xstrdup(text);
    for (char *t = strtok(copy, ","); t; t = strtok(NULL, ",")) {
        while (*t == ' ') t++;
        char *e = t + strlen(t);
        while (e > t && e[-1] == ' ') *--e = 0;
        if (*t && strncmp(t, "date:", 5) && !strlist_has(out, t)) strlist_add(out, t);   /* dates are not words' */
    }
    free(copy);
}

/* every disc's folders (rarity) and tagged paths (words with tags), and objects' own tags */
static void count_record(const arv_home *h, const archive *cat, word_stats *ws)
{
    counts tagged = { 0 };                  /* "disc\tpath" of each tagged path: an object there is counted with it */
    for (size_t d = 0; d < cat->discs.n; d++) {
        const char *id = rec_get(cat->discs.v[d], "Id");
        if (!id) continue;
        char *lf = home_volume_file(h, id, "listing.tsv");
        listing l = { 0 };
        int have = !read_listing(lf, &l);
        free(lf);
        strlist folder_names = { 0 }, folder_words = { 0 };
        counts where = { 0 };                   /* folder -> its index + 1 */
        for (size_t i = 0; have && i < l.n; i++) {     /* rarity: each folder once, its name and its files' */
            const char *p = l.r[i].path, *slash = strrchr(p, '/');
            char *folder = slash ? xprintf("%.*s", (int)(slash - p), p) : xstrdup(".");
            size_t k = count_of(&where, folder);
            if (k) k--;
            else {
                k = folder_names.n;
                bump(&where, folder, k + 1);
                strlist_add(&folder_names, folder);
                strlist_add(&folder_words, "");
                const char *last = strrchr(folder, '/') ? strrchr(folder, '/') + 1 : folder;
                strlist w = { 0 };
                if (strcmp(folder, ".")) name_word_list(last, &w);
                sbuf b = { 0 };
                sb_puts(&b, "");
                for (size_t j = 0; j < w.n; j++) sb_printf(&b, "%s\n", w.v[j]);
                free(folder_words.v[k]);
                folder_words.v[k] = b.s;
                strlist_free(&w);
            }
            strlist w = { 0 };
            name_word_list(slash ? slash + 1 : p, &w);
            sbuf b = { 0 };
            sb_puts(&b, folder_words.v[k]);
            for (size_t j = 0; j < w.n && b.len < 8192; j++) sb_printf(&b, "%s\n", w.v[j]);
            free(folder_words.v[k]);
            folder_words.v[k] = b.s;
            strlist_free(&w);
            free(folder);
        }
        for (size_t k = 0; k < folder_names.n; k++) {
            strlist w = { 0 };
            for (char *s = folder_words.v[k], *nl; s && *s; s = nl + 1) {
                nl = strchr(s, '\n');
                *nl = 0;
                strlist_add(&w, s);
            }
            unique(&w);
            for (size_t j = 0; j < w.n; j++) bump(&ws->df, w.v[j], 1);
            ws->folders++;
            strlist_free(&w);
        }
        strlist_free(&folder_names);
        strlist_free(&folder_words);
        counts_free(&where);
        char *tf = home_volume_file(h, id, "tags.tsv"), *text = disc_tags_text(cat, id, access(tf, F_OK) ? NULL : tf);
        free(tf);
        for (char *line = text, *next; line && *line; line = next) {   /* a tagged path: its words, its tags */
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            char *tab = line[0] != '#' ? strchr(line, '\t') : NULL;
            if (!tab) continue;
            *tab = 0;
            char *end = strchr(tab + 1, '\t');
            if (end) *end = 0;
            strlist tags = { 0 }, words = { 0 };
            split_tags(tab + 1, &tags);
            if (!tags.n) continue;
            const char *path = line;
            char *key = xprintf("%s\t%s", id, path);
            bump(&tagged, key, 1);
            free(key);
            if (strcmp(path, ".")) name_word_list(path, &words);
            size_t n = strlen(path), files = 0;
            for (size_t i = 0; have && i < l.n && files < UNIT_FILES; i++) {
                const char *p = l.r[i].path;
                if (strcmp(path, ".") && strcmp(p, path) && (strncmp(p, path, n) || p[n] != '/')) continue;
                const char *base = strrchr(p, '/') ? strrchr(p, '/') + 1 : p;
                name_word_list(base, &words);
                files++;
            }
            count_unit(ws, &words, &tags);
            strlist_free(&tags);
            strlist_free(&words);
        }
        free(text);
        if (have) free_listing(&l);
    }
    counts done = { 0 };
    for (size_t i = 0; i < cat->objects.n; i++) {      /* objects retagged: their names, their tags (once each) */
        const char *u = rec_get(cat->objects.v[i], "Uuid"), *set = u ? object_field(cat, u, "Tags") : NULL;
        if (!set || count_of(&done, u)) continue;
        bump(&done, u, 1);
        const char *od = rec_get(cat->objects.v[i], "Disc"), *op = rec_get(cat->objects.v[i], "Path");
        char *key = xprintf("%s\t%s", od ? od : "", op ? op : ".");
        size_t there = count_of(&tagged, key);
        free(key);
        if (there) continue;                /* its disc's tags for its path were counted */
        strlist tags = { 0 }, words = { 0 };
        split_tags(set, &tags);
        name_word_list(rec_get(cat->objects.v[i], "Name") ? rec_get(cat->objects.v[i], "Name") : "", &words);
        if (tags.n && words.n) count_unit(ws, &words, &tags);
        strlist_free(&tags);
        strlist_free(&words);
    }
    counts_free(&done);
    counts_free(&tagged);
}

static char *stamp_of(const arv_home *h)
{
    struct stat st;
    if (stat(h->rec_path, &st)) return xstrdup("none");
    return xprintf("%lld-%lld", (long long)st.st_size, (long long)st.st_mtime);
}

static char *cache_path(const arv_home *h)
{
    return join(h->cache_dir, "words.tsv");
}

static char *examples_file(const arv_home *h)
{
    return join(h->config_dir, "word-examples.tsv");
}

static void save_cache(const arv_home *h, const word_stats *ws, const char *stamp)
{
    sbuf b = { 0 };
    sb_printf(&b, "# arv words 2\t%s\t%zu\n", stamp, ws->folders);
    for (size_t i = 0; i < ws->df.cap; i++)
        if (ws->df.keys[i]) sb_printf(&b, "d\t%s\t%zu\n", ws->df.keys[i], ws->df.vals[i]);
    for (size_t i = 0; i < ws->units.cap; i++)
        if (ws->units.keys[i]) sb_printf(&b, "u\t%s\t%zu\n", ws->units.keys[i], ws->units.vals[i]);
    for (size_t i = 0; i < ws->pairs.cap; i++)
        if (ws->pairs.keys[i]) sb_printf(&b, "p\t%s\t%zu\n", ws->pairs.keys[i], ws->pairs.vals[i]);
    for (size_t i = 0; i < ws->tag_units.cap; i++)
        if (ws->tag_units.keys[i]) sb_printf(&b, "t\t%s\t%zu\n", ws->tag_units.keys[i], ws->tag_units.vals[i]);
    if (!mkdirs(h->cache_dir)) {
        char *p = cache_path(h), *tmp = xprintf("%s.tmp", p);
        write_text(tmp, b.s ? b.s : "");
        rename(tmp, p);
        free(tmp);
        free(p);
    }
    free(b.s);
}

static int load_cache(const arv_home *h, word_stats *ws, const char *stamp)
{
    char *p = cache_path(h), *text = read_text(p);
    free(p);
    if (!text) return -1;
    char *nl = strchr(text, '\n');
    char want[128];
    snprintf(want, sizeof want, "# arv words 2\t%s\t", stamp);
    if (!nl || strncmp(text, want, strlen(want))) {
        free(text);
        return -1;
    }
    ws->folders = (size_t)atol(text + strlen(want));
    for (char *line = nl + 1, *next; *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        else next = line + strlen(line);
        char *last = strrchr(line, '\t');
        if (!last || line[1] != '\t') continue;
        *last = 0;
        size_t v = (size_t)atol(last + 1);
        counts *m = line[0] == 'd' ? &ws->df : line[0] == 'u' ? &ws->units : line[0] == 'p' ? &ws->pairs
                  : line[0] == 't' ? &ws->tag_units : NULL;
        if (m) bump(m, line + 2, v);
    }
    free(text);
    return 0;
}

/* the answers given before: tags <TAB> the words of the names <TAB> the words of the text */
static void count_examples(const arv_home *h, word_stats *ws)
{
    char *p = examples_file(h), *text = read_text(p);
    free(p);
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (line[0] == '#') continue;
        char *c[4];
        if (split_tabs(line, c, 4) < 3) continue;
        strlist tags = { 0 }, words = { 0 };
        split_tags(c[1], &tags);
        int had_text = 0;
        for (int k = 2; k < 4; k++)
            for (char *w = c[k] ? strtok(c[k], ",") : NULL; w; w = strtok(NULL, ","))
                if (*w) {
                    strlist_add(&words, w);
                    if (k == 3) {
                        bump(&ws->text_df, w, 1);   /* a text's words: how common in the archive's texts */
                        had_text = 1;
                    }
                }
        ws->text_folders += had_text;
        if (tags.n) count_unit(ws, &words, &tags);
        strlist_free(&tags);
        strlist_free(&words);
    }
    free(text);
}

static void index_tags(word_stats *ws)
{
    for (size_t i = 0; i < ws->pairs.cap; i++) {
        if (!ws->pairs.keys[i]) continue;
        char *k = ws->pairs.keys[i], *tab = strchr(k, '\t');
        if (!tab) continue;
        *tab = 0;
        size_t at = count_of(&ws->tag_index, k);
        if (!at) {
            strlist_add(&ws->tags_of_word_key, k);
            strlist_add(&ws->tags_of_word_val, "");
            at = ws->tags_of_word_key.n;
            bump(&ws->tag_index, k, at);
        }
        char *was = ws->tags_of_word_val.v[at - 1], *now = xprintf("%s%s%s", was, *was ? "," : "", tab + 1);
        free(was);
        ws->tags_of_word_val.v[at - 1] = now;
        *tab = '\t';
    }
}

word_stats *word_stats_load(const arv_home *h, const archive *cat)
{
    word_stats *ws = xmalloc(sizeof *ws);
    memset(ws, 0, sizeof *ws);
    if (!h) return ws;
    char *stamp = stamp_of(h);
    if (load_cache(h, ws, stamp)) {
        counts_free(&ws->df);
        counts_free(&ws->units);
        counts_free(&ws->pairs);
        counts_free(&ws->tag_units);
        ws->folders = 0;
        count_record(h, cat, ws);
        save_cache(h, ws, stamp);
    }
    free(stamp);
    count_examples(h, ws);
    index_tags(ws);
    return ws;
}

void word_stats_free(word_stats *ws)
{
    if (!ws) return;
    counts_free(&ws->df);
    counts_free(&ws->units);
    counts_free(&ws->pairs);
    counts_free(&ws->tag_index);
    counts_free(&ws->text_df);
    counts_free(&ws->keep);
    counts_free(&ws->tag_units);
    strlist_free(&ws->tags_of_word_key);
    strlist_free(&ws->tags_of_word_val);
    free(ws);
}

double word_rarity(const word_stats *ws, const char *w)
{
    size_t df = count_of(&ws->df, w);
    return log(((double)ws->folders + 1) / ((double)df + 1));
}

int word_common(const word_stats *ws, const char *w)
{
    return ws->folders >= 20 && count_of(&ws->df, w) * 5 > ws->folders;   /* in more than a fifth of all folders */
}

/* the tags words point to: a word seen in WORD_MIN tagged paths or more, at least WORD_SHARE of them
 * with the tag; each with why ("'kicad' in 9 of 10 folders tagged electronics"), strongest first */
void word_tags(const word_stats *ws, const strlist *names, const strlist *text, strlist *tags, strlist *why)
{
    double *score = NULL;
    for (int src = 0; src < 2; src++) {
        const strlist *l = src ? text : names;
        for (size_t i = 0; l && i < l->n; i++) {
            const char *w = l->v[i];
            size_t n = count_of(&ws->units, w), at = count_of(&ws->tag_index, w);
            if (n < WORD_MIN || !at) continue;
            char *list = xstrdup(ws->tags_of_word_val.v[at - 1]);
            for (char *t = strtok(list, ","); t; t = strtok(NULL, ",")) {
                char *k = xprintf("%s\t%s", w, t);
                size_t c = count_of(&ws->pairs, k);
                free(k);
                if (c * 100 < n * WORD_SHARE) continue;
                double s = (double)c / (double)n * log((double)n + 1) * (src ? 0.5 : 1.0);   /* text: incidental */
                size_t j = 0;
                while (j < tags->n && strcmp(tags->v[j], t)) j++;
                if (j == tags->n) {
                    strlist_add(tags, t);
                    char *y = xprintf("'%s' in %zu of %zu %s tagged %s%s", w, c, n, n == 1 ? "folder" : "folders", t,
                                      src ? ", from the files' text" : "");
                    strlist_add(why, y);
                    free(y);
                    score = xrealloc(score, (tags->n + 1) * sizeof *score);
                    score[j] = s;
                } else if (s > score[j]) {
                    char *y = xprintf("'%s' in %zu of %zu %s tagged %s%s", w, c, n, n == 1 ? "folder" : "folders", t,
                                      src ? ", from the files' text" : "");
                    free(why->v[j]);
                    why->v[j] = y;
                    score[j] = s;
                }
            }
            free(list);
        }
    }
    for (size_t i = 1; i < tags->n; i++)        /* strongest first */
        for (size_t j = i; j > 0 && score[j] > score[j - 1]; j--) {
            double s = score[j];
            score[j] = score[j - 1];
            score[j - 1] = s;
            char *t = tags->v[j];
            tags->v[j] = tags->v[j - 1];
            tags->v[j - 1] = t;
            char *y = why->v[j];
            why->v[j] = why->v[j - 1];
            why->v[j - 1] = y;
        }
    free(score);
}

/* how often a word was seen with a tag, and in how many tagged paths: for the offer of a rule */
size_t word_tag_uses(const word_stats *ws, const char *tag)
{
    return ws ? count_of(&ws->tag_units, tag) : 0;
}

void word_counts(const word_stats *ws, const char *w, const char *tag, size_t *with, size_t *all)
{
    char *k = xprintf("%s\t%s", w, tag);
    *with = count_of(&ws->pairs, k);
    *all = count_of(&ws->units, w);
    free(k);
}

/* ------------------------------------------------------------------ a folder's text */

static int text_ext(const char *ext)
{
    static const char *const T[] = { ".txt", ".md", ".markdown", ".csv", ".tsv", ".rst", ".org", ".tex", ".html", ".htm",
                                     ".eml", ".log", ".json", ".xml", NULL };
    for (int i = 0; T[i]; i++)
        if (!strcmp(T[i], ext)) return 1;
    return 0;
}

static void ext_of(const char *path, char ext[16])
{
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    ext[0] = 0;
    if (!dot || (slash && dot < slash)) return;
    size_t k = 0;
    for (; dot[k] && k < 15; k++) ext[k] = (char)tolower((unsigned char)dot[k]);
    ext[k] = 0;
}

/* the words of a folder's text files and PDFs (pdftotext, when it is there), sampled; the most
 * telling CONTENT_WORDS (frequent here, rare in the archive's names), and the PDFs' own titles */
/* a text's words, lower case, into words (each once a file in files); each two in a row with nothing
 * between them but a space or a hyphen, and neither common, into the same (as "a b"); and a word
 * written with a capital where no sentence starts, into caps: a name, likely */
static void scan_text(const word_stats *ws, const char *text, counts *tf, counts *files, counts *caps, counts *occur)
{
    counts here = { 0 };
    char *prev = NULL, *prev_cap = NULL;
    int sentence = 1, broken = 1, prev_plain = 0;
    sbuf w = { 0 };
    for (const char *p = text;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c && (isalpha(c) || c >= 0x80)) {
            if (w.len < 64) sb_add(&w, (const char *)&c, 1);
            continue;
        }
        if (w.len) {
            int capital = isupper((unsigned char)w.s[0]) && !(w.len > 1 && isupper((unsigned char)w.s[1]));   /* not ALL CAPS */
            for (size_t i = 0; i < w.len; i++) w.s[i] = (char)tolower((unsigned char)w.s[i]);
            int ok = w.len >= 3 && w.len <= 30 && !stop_word(w.s), plain = ok && text_common(ws, w.s);
            if (ok) {                       /* a common word: never a clue alone, but half of a phrase ("self assessment") */
                bump(occur, w.s, 1);        /* every time, capital or not: is it mostly written as a name? */
                if (!plain) {
                    bump(tf, w.s, 1);
                    if (!count_of(&here, w.s)) bump(&here, w.s, 1);
                }
                if (prev && !broken && !(plain && prev_plain)) {
                    char *pair = xprintf("%s %s", prev, w.s);
                    bump(tf, pair, 1);
                    if (!count_of(&here, pair)) bump(&here, pair, 1);
                    free(pair);
                }
                if (capital && !sentence) {     /* a name may be a common word too: Rose, Grace, Reading */
                    bump(caps, w.s, 1);
                    if (prev_cap && !broken) {
                        char *pair = xprintf("%s %s", prev_cap, w.s);
                        bump(caps, pair, 1);
                        free(pair);
                    }
                }
            }
            free(prev);
            free(prev_cap);
            prev = ok ? xstrdup(w.s) : NULL;
            prev_plain = plain;
            prev_cap = ok && capital && !sentence ? xstrdup(w.s) : NULL;
            sentence = 0;
            broken = 0;
            w.len = 0;
            w.s[0] = 0;
        }
        if (!c) break;
        if (c == '.' || c == '!' || c == '?' || c == ':' || c == '\n') sentence = 1;   /* a capital after these is no clue */
        if (c != ' ' && c != '-') broken = 1;
    }
    for (size_t i = 0; i < here.cap; i++)
        if (here.keys[i]) bump(files, here.keys[i], 1);
    counts_free(&here);
    free(prev);
    free(prev_cap);
    free(w.s);
}

void text_clues(const word_stats *ws, const char *root, const items *l, strlist *out, strlist *titles, strlist *names,
                size_t *pdfs_read, strlist *scans)
{
    memset(out, 0, sizeof *out);
    memset(scans, 0, sizeof *scans);
    *pdfs_read = 0;
    if (!root) return;
    int pdf = on_path("pdftotext"), info = on_path("pdfinfo");
    size_t readable = 0;
    for (size_t i = 0; i < l->n; i++) {
        char ext[16];
        ext_of(l->v[i].path, ext);
        readable += text_ext(ext) || (pdf && !strcmp(ext, ".pdf"));
    }
    if (!readable) return;
    size_t step = readable > TEXT_FILES ? readable / TEXT_FILES : 1, at = 0, done = 0;
    counts tf = { 0 }, files = { 0 }, caps = { 0 }, occur = { 0 };
    for (size_t i = 0; i < l->n && done < TEXT_FILES; i++) {
        char ext[16];
        ext_of(l->v[i].path, ext);
        int is_pdf = !strcmp(ext, ".pdf");
        if (!text_ext(ext) && !(pdf && is_pdf)) continue;
        if (at++ % step) continue;
        char *p = join(root, l->v[i].path), *text = NULL;
        if (is_pdf) {
            char *const argv[] = { "pdftotext", "-q", "-l", "3", "-enc", "UTF-8", p, "-", NULL };
            if (run(argv, &text) != 0) {
                free(text);
                text = NULL;
            }
            if (text) {                     /* no letter on its first pages: pictures of paper, a scan */
                int letters = 0;
                for (const unsigned char *q = (const unsigned char *)text; *q && !letters; q++) letters = isalnum(*q) || *q >= 0x80;
                (*pdfs_read)++;
                if (!letters) strlist_add(scans, l->v[i].path);
            }
            if (info && titles) {           /* its own Title, when it has one */
                char *meta = NULL;
                char *const ia[] = { "pdfinfo", p, NULL };
                if (!run(ia, &meta) && meta)
                    for (char *line = meta, *next; *line; line = next) {
                        next = strchr(line, '\n');
                        if (next) *next++ = 0;
                        else next = line + strlen(line);
                        if (!strncmp(line, "Title:", 6)) {
                            char *t = line + 6;
                            while (*t == ' ') t++;
                            if (*t && !strlist_has(titles, t)) strlist_add(titles, t);
                        }
                    }
                free(meta);
            }
        } else {
            FILE *fp = fopen(p, "rb");
            if (fp) {
                text = xmalloc(TEXT_BYTES + 1);
                size_t n = fread(text, 1, TEXT_BYTES, fp);
                text[n] = 0;
                fclose(fp);
            }
        }
        free(p);
        if (!text) continue;
        done++;
        scan_text(ws, text, &tf, &files, &caps, &occur);
        free(text);
    }
    /* telling: in two files or more (or three times in one), weighed by how rare in the archive's names;
     * a pair only as a phrase: twice or more, and mostly where its rarer word is ("self assessment"),
     * and then its words only where they are not mostly in it */
    size_t n = 0;
    char **keys = xmalloc((tf.n + 1) * sizeof *keys);
    double *score = xmalloc((tf.n + 1) * sizeof *score);
    counts covered = { 0 };
    for (size_t i = 0; i < tf.cap; i++) {
        if (!tf.keys[i]) continue;
        char *sp = strchr(tf.keys[i], ' ');
        if (!sp) continue;
        *sp = 0;
        size_t a = count_of(&tf, tf.keys[i]), b = count_of(&tf, sp + 1), pc = tf.vals[i];
        if (!a) a = pc;                     /* a common word is counted only in its pairs */
        if (!b) b = pc;
        size_t lo = a < b ? a : b;
        if (pc >= 2 && pc * 2 >= lo) {
            if (a <= pc * 2) bump(&covered, tf.keys[i], 1);
            if (b <= pc * 2) bump(&covered, sp + 1, 1);
            *sp = ' ';
        } else {
            *sp = ' ';
            tf.vals[i] = 0;                 /* two words side by side, no more */
        }
    }
    for (size_t i = 0; i < tf.cap; i++) {
        if (!tf.keys[i] || !tf.vals[i] || count_of(&covered, tf.keys[i])) continue;
        size_t f = count_of(&files, tf.keys[i]);
        if (f < 2 && tf.vals[i] < 3) continue;
        keys[n] = tf.keys[i];
        score[n++] = log(1.0 + (double)tf.vals[i]) * (1.0 + (double)f) * (ws ? word_rarity(ws, tf.keys[i]) + 1.0 : 1.0)
                     * (strchr(tf.keys[i], ' ') ? 1.5 : 1.0);
    }
    if (names) {                            /* names: written with a capital where no sentence starts, mostly */
        memset(names, 0, sizeof *names);
        for (size_t i = 0; i < caps.cap && names->n < 10; i++) {
            if (!caps.keys[i] || caps.vals[i] < 2) continue;
            size_t all = strchr(caps.keys[i], ' ') ? count_of(&tf, caps.keys[i]) : count_of(&occur, caps.keys[i]);
            if (!all) all = caps.vals[i];
            int in_pair = 0;                /* "anne" of "anne smith": the pair is the name */
            for (size_t j = 0; !strchr(caps.keys[i], ' ') && j < caps.cap && !in_pair; j++)
                if (caps.keys[j] && caps.vals[j] >= 2 && strchr(caps.keys[j], ' ')) {
                    size_t k = strlen(caps.keys[i]);
                    in_pair = (!strncmp(caps.keys[j], caps.keys[i], k) && caps.keys[j][k] == ' ')
                              || !strcmp(strchr(caps.keys[j], ' ') + 1, caps.keys[i]);
                }
            if (!in_pair && caps.vals[i] * 3 >= all * 2) strlist_add(names, caps.keys[i]);
        }
        for (size_t i = 1; i < names->n; i++)  /* the same every run: by name */
            for (size_t j = i; j > 0 && strcmp(names->v[j], names->v[j - 1]) < 0; j--) {
                char *t = names->v[j];
                names->v[j] = names->v[j - 1];
                names->v[j - 1] = t;
            }
    }
    counts_free(&covered);
    for (size_t k = 0; k < CONTENT_WORDS && k < n; k++) {
        size_t best = k;
        for (size_t j = k + 1; j < n; j++)
            if (score[j] > score[best] || (score[j] == score[best] && strcmp(keys[j], keys[best]) < 0)) best = j;
        char *t = keys[k];
        keys[k] = keys[best];
        keys[best] = t;
        double s = score[k];
        score[k] = score[best];
        score[best] = s;
        strlist_add(out, keys[k]);
    }
    free(keys);
    free(score);
    counts_free(&tf);
    counts_free(&files);
    counts_free(&caps);
    counts_free(&occur);
}

/* ------------------------------------------------------------------ what was answered, kept */

void word_example_add(const arv_home *h, const strlist *tags, const strlist *names, const strlist *text)
{
    if (!h || !tags->n) return;
    char *p = examples_file(h);
    int fresh = access(p, F_OK) != 0;
    FILE *fp = fopen(p, "a");
    if (fp) {
        if (fresh) fputs("# arv word-examples 1\tdate\ttags\twords of the names\twords of the text (what arv describe --ask "
                         "was told; home only)\n", fp);
        char today[11];
        today_iso(today);
        fprintf(fp, "%s\t", today);
        for (size_t i = 0, k = 0; i < tags->n; i++)
            if (strncmp(tags->v[i], "date:", 5)) fprintf(fp, "%s%s", k++ ? "," : "", tags->v[i]);
        fputc('\t', fp);
        for (size_t i = 0; i < names->n; i++) fprintf(fp, "%s%s", i ? "," : "", names->v[i]);
        fputc('\t', fp);
        for (size_t i = 0; text && i < text->n; i++) fprintf(fp, "%s%s", i ? "," : "", text->v[i]);
        fputc('\n', fp);
        fclose(fp);
    }
    free(p);
}

/* the words of a path's names that keep going with one of its tags (in WORD_MIN tagged paths or more,
 * nine in ten of them with it), not yet a rule for it, nor the tag's own word: rules to offer, at most
 * max. pending: the tags given now are not yet in the counts. */
size_t word_rules_offered(const word_stats *ws, const tag_words_t *tw, const strlist *tags, const strlist *terms, int pending,
                          word_rule *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; ws && i < tags->n && n < max; i++) {
        if (!strncmp(tags->v[i], "date:", 5)) continue;
        for (size_t w = 0; w < terms->n && n < max; w++) {
            const char *word = terms->v[w];
            size_t with, all;
            word_counts(ws, word, tags->v[i], &with, &all);
            if (pending) with++, all++;     /* this answer too */
            if (all < WORD_MIN || with * 100 < all * 90) continue;
            const char *value = strchr(tags->v[i], ':') ? strchr(tags->v[i], ':') + 1 : tags->v[i];
            if (strstr(value, word)) continue;          /* the tag's own word: no rule needed */
            char *glob = xprintf("*%s*", word);
            for (char *g = glob; *g; g++)
                if (*g == ' ') *g = '*';
            int have = 0;
            for (size_t r = 0; r < tw->rule_tag.n && !have; r++)
                have = !strcmp(tw->rule_tag.v[r], tags->v[i]) && !strcmp(tw->rule_glob.v[r], glob);
            if (have) {
                free(glob);
                continue;
            }
            out[n++] = (word_rule){ xstrdup(tags->v[i]), xstrdup(word), glob, with, all };
        }
    }
    return n;
}

void word_rules_free(word_rule *r, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(r[i].tag);
        free(r[i].word);
        free(r[i].glob);
    }
}

/* a rule in the vocabulary: Match: *WORD* for TAG, in config/tags.rec, after its Name line (a new
 * record when the vocabulary has no such tag); the file kept as it is otherwise, comments too */
int word_rule_add(const arv_home *h, const char *tag, const char *word)
{
    char *p = join(h->config_dir, "tags.rec"), *text = read_text(p);
    if (!text) {
        free(p);
        return -1;
    }
    char *glob = xstrdup(word);
    for (char *c = glob; *c; c++)
        if (*c == ' ') *c = '*';            /* a pair of words: both, in that order */
    char *line = xprintf("Name: %s\n", tag), *at = NULL;
    for (char *s = text; (s = strstr(s, line)); s++)
        if (s == text || s[-1] == '\n') {
            at = s;
            break;
        }
    sbuf b = { 0 };
    if (at) {
        size_t cut = (size_t)(at - text) + strlen(line);
        sb_add(&b, text, cut);
        sb_printf(&b, "Match: *%s*\n", glob);
        sb_puts(&b, text + cut);
    } else {                                /* a new Tag record: before the Namespace records, if any */
        char *ns = strstr(text, "\n%rec: Namespace");
        size_t cut = ns ? (size_t)(ns - text) + 1 : strlen(text);
        sb_add(&b, text, cut);
        if (b.len && b.s[b.len - 1] != '\n') sb_puts(&b, "\n");
        sb_printf(&b, "%sName: %s\nDescription: learned: folders with '%s' in their names\nMatch: *%s*\n%s", ns ? "" : "\n", tag,
                  word, glob, ns ? "\n" : "");
        sb_puts(&b, text + cut);
    }
    char *tmp = xprintf("%s.tmp", p);
    write_text(tmp, b.s);
    int rc = rename(tmp, p);
    free(tmp);
    free(b.s);
    free(line);
    free(glob);
    free(text);
    free(p);
    return rc;
}
