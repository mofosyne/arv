/* index.html on every disc (src/arv/html.py): no JavaScript, no network, works from file://.
 * A generated view; the recfiles and manifests are the source of truth. */
#define _XOPEN_SOURCE 700
#include "arvc.h"
#include "data.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void human_size(uint64_t n, char out[32])
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)n;
    for (int u = 0; u < 5; u++) {
        if (v < 1024 || u == 4) {
            if (u == 0) snprintf(out, 32, "%llu B", (unsigned long long)n);
            else snprintf(out, 32, "%.1f %s", v, units[u]);
            return;
        }
        v /= 1024;
    }
}

static void esc(sbuf *b, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': sb_puts(b, "&amp;"); break;
        case '<': sb_puts(b, "&lt;"); break;
        case '>': sb_puts(b, "&gt;"); break;
        case '"': sb_puts(b, "&quot;"); break;
        case '\'': sb_puts(b, "&#x27;"); break;
        default: sb_add(b, s, 1);
        }
    }
}

static void quote(sbuf *b, const char *s)     /* urllib.parse.quote: / and unreserved characters stay */
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) && c < 128) sb_add(b, s, 1);
        else if (strchr("/_.-~", c)) sb_add(b, s, 1);
        else sb_printf(b, "%%%02X", c);
    }
}

/* the tree of files: folders first, then files, each by lower-case name */
typedef struct tnode {
    char *name;                 /* "name/" for a folder */
    const entry *file;
    struct tnode **kids;
    size_t nkids;
} tnode;

static tnode *child(tnode *n, const char *name, size_t len, int folder)
{
    for (size_t i = 0; i < n->nkids; i++) {
        tnode *k = n->kids[i];
        if (strlen(k->name) == len + (size_t)folder && !strncmp(k->name, name, len) && (!folder || k->name[len] == '/'))
            return k;
    }
    tnode *k = calloc(1, sizeof *k);
    if (!k) die("%s", "out of memory");
    k->name = xmalloc(len + 2);
    memcpy(k->name, name, len);
    if (folder) k->name[len++] = '/';
    k->name[len] = 0;
    n->kids = xrealloc(n->kids, (n->nkids + 1) * sizeof *n->kids);
    n->kids[n->nkids++] = k;
    return k;
}

static uint64_t tree_size(const tnode *n)
{
    if (n->file) return n->file->size;
    uint64_t t = 0;
    for (size_t i = 0; i < n->nkids; i++) t += tree_size(n->kids[i]);
    return t;
}

static int lower_cmp(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y) return x - y;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int by_tree_order(const void *a, const void *b)
{
    const tnode *x = *(tnode *const *)a, *y = *(tnode *const *)b;
    int fx = x->file != NULL, fy = y->file != NULL;
    if (fx != fy) return fx - fy;
    return lower_cmp(x->name, y->name);
}

static void render_tree(tnode *n, sbuf *out, int depth)
{
    char size[32];
    sb_puts(out, depth == 0 ? "<ul class=\"tree\">\n" : "<ul>\n");
    if (n->nkids) qsort(n->kids, n->nkids, sizeof *n->kids, by_tree_order);
    for (size_t i = 0; i < n->nkids; i++) {
        tnode *k = n->kids[i];
        human_size(tree_size(k), size);
        if (!k->file) {
            sb_printf(out, "<li><details%s><summary>", depth == 0 ? " open" : "");
            esc(out, k->name);
            sb_printf(out, "<span class=\"size\">%s</span></summary>\n", size);
            render_tree(k, out, depth + 1);
            sb_puts(out, "</details></li>\n");
        } else {
            sb_puts(out, "<li><a href=\"");
            quote(out, "data/");
            quote(out, k->file->path);
            sb_puts(out, "\">");
            esc(out, k->name);
            sb_printf(out, "</a><span class=\"size\">%s</span></li>\n", size);
        }
    }
    sb_puts(out, "</ul>\n");
}

static void field_rows(sbuf *out, const rec_record *r, const char *const *names)
{
    for (int i = 0; names[i]; i++) {
        int any = 0;
        for (size_t f = 0; f < r->nfields; f++) {
            if (strcmp(r->fields[f].name, names[i])) continue;
            if (!any) {
                if (out->len && out->s[out->len - 1] != '\n') sb_puts(out, "\n");
                sb_puts(out, "<tr><th>");
                esc(out, names[i]);
                sb_puts(out, "</th><td>");
            } else {
                sb_puts(out, "<br>");
            }
            esc(out, r->fields[f].value);
            any = 1;
        }
        if (any) sb_puts(out, "</td></tr>");
    }
}

/* lines joined with "\n", as the Python list is */
static void line(sbuf *out, const char *s)
{
    if (out->len) sb_puts(out, "\n");
    sb_puts(out, s);
}

char *render_index(const rec_record *disc, const rec_record *binding, const entries *files,
                   const archive *snapshot, char *(*where_fn)(const archive *, const rec_record *))
{
    sbuf out = { 0 }, t = { 0 };
    const char *title = rec_get(disc, "Title") ? rec_get(disc, "Title") : rec_get(disc, "Id");
    line(&out, "<!doctype html>");
    line(&out, "<html lang=\"en\"><head><meta charset=\"utf-8\">");
    line(&out, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
    esc(&t, title);
    sb_printf(&out, "\n<title>%s</title>", t.s);
    sb_printf(&out, "\n<style>%s</style></head><body><main>", DATA_INDEX_CSS);
    sb_printf(&out, "\n<h1>%s</h1>", t.s);
    t.len = 0;
    esc(&t, rec_get(disc, "Id"));
    sb_printf(&out, "\n<div class=\"id\">%s</div>", t.s);
    for (size_t f = 0; f < disc->nfields; f++)
        if (!strcmp(disc->fields[f].name, "Description")) {
            sb_puts(&out, "\n<p>");
            esc(&out, disc->fields[f].value);
            sb_puts(&out, "</p>");
        }
    line(&out, "<p>To search this disc and the rest of the archive, see SEARCH in "
               "<a href=\"README.txt\">README.txt</a>.</p>");
    line(&out, "<h2>About this disc</h2><table>");
    /* the disc's record with its Binding's fields after it */
    rec_record view = { 0 };
    view.type = "Disc";
    rec_copy(&view, disc);
    for (size_t f = 0; binding && f < binding->nfields; f++)
        if (strcmp(binding->fields[f].name, "Volume") && !rec_get(&view, binding->fields[f].name))
            rec_add(&view, binding->fields[f].name, binding->fields[f].value);
    static const char *const shown[] = { "Id", "Label", "Part", "Set", "Category", "Path", "Sequence", "Coverage",
                                        "Date", "Creator", "Subject", "Location", "Access", "Rights", "Media",
                                        "Container", "Filesystem", "Protection", "Ecc", "Software", NULL };
    sbuf rows = { 0 };
    field_rows(&rows, &view, shown);
    sb_puts(&out, "\n");
    if (rows.s) sb_puts(&out, rows.s);
    free(rows.s);
    rec_clear(&view);
    char size[32];
    human_size(strtoull(rec_get(disc, "Bytes") ? rec_get(disc, "Bytes") : "0", NULL, 10), size);
    t.len = 0;
    esc(&t, rec_get(disc, "Files") ? rec_get(disc, "Files") : "None");
    sb_printf(&out, "\n<tr><th>Contents</th><td>%s files, %s</td></tr>", t.s, size);
    line(&out, "</table>");
    int notes = 0;
    for (size_t f = 0; f < disc->nfields; f++)
        if (!strcmp(disc->fields[f].name, "Note")) {
            if (!notes++) line(&out, "<h2>Notes</h2>");
            sb_puts(&out, "\n<div class=\"note\">");
            esc(&out, disc->fields[f].value);
            sb_puts(&out, "</div>");
        }
    line(&out, "<h2>Files</h2>");
    if (files->n) {
        tnode root = { 0 };
        for (size_t i = 0; i < files->n; i++) {
            tnode *n = &root;
            const char *p = files->v[i].path, *slash;
            while ((slash = strchr(p, '/'))) {
                n = child(n, p, (size_t)(slash - p), 1);
                p = slash + 1;
            }
            child(n, p, strlen(p), 0)->file = &files->v[i];
        }
        sbuf tree = { 0 };
        render_tree(&root, &tree, 0);
        tree.s[--tree.len] = 0;             /* the Python joins its parts with newlines */
        sb_puts(&out, "\n");
        sb_puts(&out, tree.s);
        free(tree.s);
    } else {
        line(&out, "<p>(empty)</p>");
    }
    size_t others = 0;
    for (size_t i = 0; snapshot && i < snapshot->discs.n; i++)
        if (strcmp(rec_get(snapshot->discs.v[i], "Id") ? rec_get(snapshot->discs.v[i], "Id") : "", rec_get(disc, "Id"))) others++;
    if (others) {
        line(&out, "<h2>Other discs in this archive</h2>");
        line(&out, "<p>As of this disc's burn date. File lists: <code>catalog/volumes/</code>.</p>");
        line(&out, "<div class=\"scroll\"><table><tr><th>Id</th><th>Title</th><th>Coverage</th><th>Location</th><th>Files</th></tr>");
        for (size_t i = 0; i < snapshot->discs.n; i++) {
            const rec_record *d = snapshot->discs.v[i];
            const char *id = rec_get(d, "Id") ? rec_get(d, "Id") : "";
            if (!strcmp(id, rec_get(disc, "Id"))) continue;
            char *w = where_fn(snapshot, d);
            const char *cells[5] = { id, rec_get(d, "Title") ? rec_get(d, "Title") : "",
                                     rec_get(d, "Coverage") ? rec_get(d, "Coverage") : "", w,
                                     rec_get(d, "Files") ? rec_get(d, "Files") : "" };
            sb_puts(&out, "\n<tr>");
            for (int c = 0; c < 5; c++) {
                sb_puts(&out, "<td>");
                esc(&out, cells[c]);
                sb_puts(&out, "</td>");
            }
            sb_puts(&out, "</tr>");
            free(w);
        }
        line(&out, "</table></div>");
    }
    line(&out, "<h2>Verify and recover</h2>");
    sb_printf(&out, "\n<p>See <a href=\"README.txt\">README.txt</a>. Checksums: "
                    "<a href=\"manifest-sha256.txt\">manifest-sha256.txt</a>, "
                    "<a href=\"manifest-sha512.txt\">manifest-sha512.txt</a>. "
                    "Catalogue: <a href=\"catalog.rec\">catalog.rec</a>%s.</p>",
              snapshot ? ", <a href=\"catalog/archive.rec\">catalog/archive.rec</a>" : "");
    line(&out, "</main></body></html>\n");
    free(t.s);
    return out.s;
}
