/* What describe and tag share: the files of a folder or disc, folder tags, JSON, the terminal. */
#define _XOPEN_SOURCE 700
#include "assist.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ files */

static void items_add(items *l, const char *path, uint64_t size, time_t mtime)
{
    l->v = xrealloc(l->v, (l->n + 1) * sizeof *l->v);
    l->v[l->n].path = xstrdup(path);
    l->v[l->n].size = size;
    l->v[l->n].mtime = mtime;
    l->n++;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* os.walk order: a folder's files, then its folders (hidden ones skipped), each sorted by name */
static void walk(const char *root, const char *rel, items *out)
{
    char *dir = *rel ? join(root, rel) : xstrdup(root);
    DIR *dp = opendir(dir);
    struct dirent *e;
    strlist names = { 0 };
    while (dp && (e = readdir(dp)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) strlist_add(&names, e->d_name);
    if (dp) closedir(dp);
    if (names.n) qsort(names.v, names.n, sizeof *names.v, by_name);
    strlist dirs = { 0 };
    for (size_t i = 0; i < names.n; i++) {
        char *full = join(dir, names.v[i]), *r = *rel ? join(rel, names.v[i]) : xstrdup(names.v[i]);
        struct stat st;
        if (!lstat(full, &st)) {
            if (S_ISDIR(st.st_mode)) {
                if (names.v[i][0] != '.') strlist_add(&dirs, r);
            } else if (S_ISREG(st.st_mode)) {
                items_add(out, r, (uint64_t)st.st_size, st.st_mtime);
            }
        }
        free(full);
        free(r);
    }
    for (size_t i = 0; i < dirs.n; i++) walk(root, dirs.v[i], out);
    strlist_free(&names);
    strlist_free(&dirs);
    free(dir);
}

void items_of_folder(const char *src, items *out)
{
    memset(out, 0, sizeof *out);
    walk(src, "", out);
}

/* the files in data/ from the disc's listing in the home (links only noted are not files) */
void items_of_disc(const arv_home *h, const char *disc_id, items *out)
{
    memset(out, 0, sizeof *out);
    char *path = home_volume_file(h, disc_id, "listing.tsv");
    listing l;
    if (read_listing(path, &l)) die2("no file listing for %s at %s", disc_id, path);
    for (size_t i = 0; i < l.n; i++)
        if (l.r[i].size >= 0) {
            long long t = parse_utc(l.r[i].modified);
            items_add(out, l.r[i].path, (uint64_t)l.r[i].size, t < 0 ? 0 : (time_t)t);
        }
    free_listing(&l);
    free(path);
}

void items_free(items *l)
{
    for (size_t i = 0; i < l->n; i++) free(l->v[i].path);
    free(l->v);
    memset(l, 0, sizeof *l);
}

/* ------------------------------------------------------------------ folder tags */

strlist *ftags_get(ftags *f, const char *folder, int create)
{
    for (size_t i = 0; i < f->n; i++)
        if (!strcmp(f->folder[i], folder)) return &f->tags[i];
    if (!create) return NULL;
    f->folder = xrealloc(f->folder, (f->n + 1) * sizeof *f->folder);
    f->tags = xrealloc(f->tags, (f->n + 1) * sizeof *f->tags);
    f->caption = xrealloc(f->caption, (f->n + 1) * sizeof *f->caption);
    f->folder[f->n] = xstrdup(folder);
    memset(&f->tags[f->n], 0, sizeof *f->tags);
    f->caption[f->n] = NULL;
    return &f->tags[f->n++];
}

void ftags_set_caption(ftags *f, const char *folder, const char *caption)
{
    ftags_get(f, folder, 1);
    for (size_t i = 0; i < f->n; i++)
        if (!strcmp(f->folder[i], folder)) {
            free(f->caption[i]);
            f->caption[i] = caption && *caption ? xstrdup(caption) : NULL;
        }
}

void ftags_free(ftags *f)
{
    for (size_t i = 0; i < f->n; i++) {
        free(f->folder[i]);
        strlist_free(&f->tags[i]);
        free(f->caption[i]);
    }
    free(f->folder);
    free(f->tags);
    free(f->caption);
    memset(f, 0, sizeof *f);
}

/* catalog.write_tags: folder <TAB> comma-separated tags [<TAB> caption], folders sorted */
void write_tags_file(const char *path, const ftags *f)
{
    strlist keys = { 0 };
    for (size_t i = 0; i < f->n; i++)
        if ((f->tags[i].n || f->caption[i]) && !strlist_has(&keys, f->folder[i])) strlist_add(&keys, f->folder[i]);
    if (keys.n) qsort(keys.v, keys.n, sizeof *keys.v, by_name);
    sbuf b = { 0 };
    sb_puts(&b, "# folder (relative to data/)\ttags\tcaption (what sampled images show, if analysed)\n");
    for (size_t k = 0; k < keys.n; k++)
        for (size_t i = 0; i < f->n; i++) {
            if (strcmp(f->folder[i], keys.v[k])) continue;
            sb_printf(&b, "%s\t", f->folder[i]);
            for (size_t t = 0; t < f->tags[i].n; t++) sb_printf(&b, "%s%s", t ? ", " : "", f->tags[i].v[t]);
            if (f->caption[i]) {
                sb_puts(&b, "\t");
                for (const char *c = f->caption[i]; *c; c++) sb_add(&b, *c == '\t' || *c == '\n' ? " " : c, 1);
            }
            sb_puts(&b, "\n");
            break;
        }
    char *slash = strrchr(path, '/');
    if (slash) {
        char *dir = xprintf("%.*s", (int)(slash - path), path);
        if (mkdirs(dir)) die("cannot create %s", dir);
        free(dir);
    }
    write_text(path, b.s);
    free(b.s);
    strlist_free(&keys);
}

/* catalog.read_tag_info */
void read_tags_file(const char *path, ftags *out)
{
    memset(out, 0, sizeof *out);
    char *text = read_text(path);
    if (!text) return;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *tab = strchr(line, '\t');
        if (*line == '#' || !tab) continue;
        *tab = 0;
        char *rest = tab + 1, *cap = strchr(rest, '\t');
        if (cap) *cap++ = 0;
        strlist *tags = ftags_get(out, line, 1);
        strlist_free(tags);
        char *copy = xstrdup(rest);
        for (char *t = copy, *next; t; t = next) {
            next = strchr(t, ',');
            if (next) *next++ = 0;
            while (*t == ' ') t++;
            char *e = t + strlen(t);
            while (e > t && (e[-1] == ' ' || e[-1] == '\r')) *--e = 0;
            if (*t) strlist_add(tags, t);
        }
        free(copy);
        if (cap) {
            while (*cap == ' ') cap++;
            char *e = cap + strlen(cap);
            while (e > cap && (e[-1] == ' ' || e[-1] == '\r')) *--e = 0;
        }
        ftags_set_caption(out, line, cap);
    }
    free(text);
}

/* ------------------------------------------------------------------ json */

static void quote(sbuf *b, const char *s)      /* json.dumps(ensure_ascii=False) */
{
    sb_puts(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"') sb_puts(b, "\\\"");
        else if (*p == '\\') sb_puts(b, "\\\\");
        else if (*p == '\n') sb_puts(b, "\\n");
        else if (*p == '\r') sb_puts(b, "\\r");
        else if (*p == '\t') sb_puts(b, "\\t");
        else if (*p < 0x20) sb_printf(b, "\\u%04x", *p);
        else sb_add(b, (const char *)p, 1);
    }
    sb_puts(b, "\"");
}

static void dump(sbuf *b, const jv *v, int indent, int depth)
{
    if (!v) { sb_puts(b, "null"); return; }
    if (v->kind == 's') { quote(b, v->str); return; }
    if (v->kind == 't') { sb_puts(b, v->str); return; }
    if (v->kind == 'n') { sb_puts(b, "null"); return; }
    int obj = v->kind == 'o';
    sb_puts(b, obj ? "{" : "[");
    for (size_t i = 0; i < v->n; i++) {
        if (i) sb_puts(b, indent < 0 ? ", " : ",");
        if (indent >= 0) sb_printf(b, "\n%*s", indent * (depth + 1), "");
        if (obj) {
            quote(b, v->keys[i]);
            sb_puts(b, ": ");
        }
        dump(b, v->vals[i], indent, depth + 1);
    }
    if (indent >= 0 && v->n) sb_printf(b, "\n%*s", indent * depth, "");
    sb_puts(b, obj ? "}" : "]");
}

void json_dump(sbuf *b, const jv *v, int indent)
{
    dump(b, v, indent, 0);
}

jv *jnum(const char *literal)
{
    jv *v = jstr(literal);
    v->kind = 't';
    return v;
}

jv *jstrings(const strlist *l)
{
    jv *a = jarr();
    for (size_t i = 0; l && i < l->n; i++) jpush(a, jstr(l->v[i]));
    return a;
}

char *jstr_of(const jv *v)
{
    return v && v->kind == 's' ? v->str : NULL;
}

void save_json(const char *path, const jv *doc)
{
    sbuf b = { 0 };
    json_dump(&b, doc, 2);
    sb_puts(&b, "\n");
    write_text(path, b.s);
    free(b.s);
}

/* ------------------------------------------------------------------ the terminal */

char *ask(const char *prompt)
{
    fputs(prompt, stderr);
    fflush(stderr);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, stdin);
    if (n < 0) {
        free(line);
        return xstrdup("");
    }
    char *s = line, *e = line + n;
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    while (*s == ' ' || *s == '\t') s++;
    char *out = xstrdup(s);
    free(line);
    return out;
}
