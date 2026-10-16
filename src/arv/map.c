/*
 * The places the tags name, and where they are (research/plan.md, "Known names, as authority
 * control": Places). A place tag (place:kyoto) is an entry of the gazetteer, config/places.rec,
 * which gives its label, the places it is in and its position; the record itself keeps the tag
 * alone, never coordinates. Maps are for map tools: --geojson gives them the places, and --svg (and
 * each disc's index.html) an outline that needs nothing to show.
 *
 *   arv map [--json | --geojson | --svg]
 */
#define _XOPEN_SOURCE 700
#include "arv.h"
#include "data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char *key;
    size_t paths, objects;
    strlist discs;
} place_use;

typedef struct {
    place_use *v;
    size_t n;
} place_uses;

static double root(double x)               /* no libm: arv builds with one cc line */
{
    double r = x > 1 ? x / 2 : 1;
    for (int i = 0; i < 30 && x > 0; i++) r = (r + x / r) / 2;
    return x > 0 ? r : 0;
}

static place_use *use_of(place_uses *u, const char *key)
{
    for (size_t i = 0; i < u->n; i++)
        if (!strcmp(u->v[i].key, key)) return &u->v[i];
    u->v = xrealloc(u->v, (u->n + 1) * sizeof *u->v);
    place_use *p = &u->v[u->n++];
    memset(p, 0, sizeof *p);
    p->key = xstrdup(key);
    return p;
}

/* each place tag of a path's (or an object's) tags, counted once */
static void count_tags(place_uses *u, const char *tags, const char *disc, int object)
{
    char *copy = xstrdup(tags);
    strlist seen = { 0 };
    for (char *t = strtok(copy, ","); t; t = strtok(NULL, ",")) {
        while (*t == ' ') t++;
        char *e = t + strlen(t);
        while (e > t && e[-1] == ' ') *--e = 0;
        if (strncmp(t, "place:", 6) || !t[6] || strlist_has(&seen, t)) continue;
        strlist_add(&seen, t);
        place_use *p = use_of(u, t);
        if (object) p->objects++;
        else p->paths++;
        if (disc && !strlist_has(&p->discs, disc)) strlist_add(&p->discs, disc);
    }
    strlist_free(&seen);
    free(copy);
}

/* a tags.tsv's lines (PATH<tab>TAGS); with cat, a data object's own path counts as the object */
static void count_tsv(place_uses *u, const archive *cat, const char *disc, char *text)
{
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *tab = line[0] != '#' ? strchr(line, '\t') : NULL;
        if (!tab) continue;
        *tab = 0;
        char *end = strchr(tab + 1, '\t');
        if (end) *end = 0;
        const rec_record *o = cat ? object_at(cat, disc, line) : NULL;
        if (o && rec_get(o, "Uuid") && object_field(cat, rec_get(o, "Uuid"), "Tags")) continue;   /* its tags now: below */
        count_tags(u, tab + 1, disc, o && rec_get(o, "Uuid"));   /* a data object's own path: the object */
    }
}

static void uses_free(place_uses *u);

static void uses_in_home(const arv_home *h, const archive *cat, place_uses *u)
{
    memset(u, 0, sizeof *u);
    for (size_t i = 0; i < cat->discs.n; i++) {
        const char *id = rec_get(cat->discs.v[i], "Id");
        if (!id) continue;
        char *tf = home_volume_file(h, id, "tags.tsv"), *text = disc_tags_text(cat, id, access(tf, F_OK) ? NULL : tf);
        count_tsv(u, cat, id, text);
        free(text);
        free(tf);
    }
    strlist seen = { 0 };
    for (size_t i = 0; i < cat->objects.n; i++) {     /* an object once, by its tags now; on each disc it is on */
        const char *uuid = rec_get(cat->objects.v[i], "Uuid"), *t = uuid ? object_field(cat, uuid, "Tags") : NULL;
        const char *disc = rec_get(cat->objects.v[i], "Disc");
        if (!t) continue;
        if (!strlist_has(&seen, uuid)) {
            strlist_add(&seen, uuid);
            count_tags(u, t, disc, 1);
        } else {
            place_uses one = { 0 };
            count_tags(&one, t, disc, 1);     /* only its discs, added */
            for (size_t k = 0; k < one.n; k++) {
                place_use *p = use_of(u, one.v[k].key);
                if (disc && !strlist_has(&p->discs, disc)) strlist_add(&p->discs, disc);
            }
            uses_free(&one);
        }
    }
    strlist_free(&seen);
}

static void uses_free(place_uses *u)
{
    for (size_t i = 0; i < u->n; i++) {
        free(u->v[i].key);
        strlist_free(&u->v[i].discs);
    }
    free(u->v);
}

static int by_uses(const void *a, const void *b)
{
    const place_use *x = a, *y = b;
    size_t nx = x->paths + x->objects, ny = y->paths + y->objects;
    return nx != ny ? (nx > ny ? -1 : 1) : strcmp(x->key, y->key);
}

/* ------------------------------------------------------------------ the picture */

/* an outline of the land with the places on it, framed to them (an inline SVG, light or dark) */
static char *map_svg(const known_entry *kv, size_t kn, const place_uses *u, int hide_private)
{
    double minx = 720, miny = 360, maxx = 0, maxy = 0;
    size_t shown = 0;
    for (size_t i = 0; i < u->n; i++) {
        const known_entry *e = known_find(kv, kn, u->v[i].key);
        double la, lo;
        if (!e || (hide_private && e->private_) || !known_position(kv, kn, e, &la, &lo)) continue;
        double x = (lo + 180) * 2, y = (90 - la) * 2;
        minx = x < minx ? x : minx, maxx = x > maxx ? x : maxx;
        miny = y < miny ? y : miny, maxy = y > maxy ? y : maxy;
        shown++;
    }
    if (!shown) return NULL;
    double w = (maxx - minx) * 1.4, hgt = (maxy - miny) * 1.4, cx = (minx + maxx) / 2, cy = (miny + maxy) / 2;
    if (w < 60) w = 60;                         /* thirty degrees at the least: the country around it */
    if (hgt < w / 2) hgt = w / 2;
    else w = hgt * 2;
    if (w > 720) w = 720, hgt = 360;
    double x0 = cx - w / 2, y0 = cy - hgt / 2;
    x0 = x0 < 0 ? 0 : x0 + w > 720 ? 720 - w : x0;
    y0 = y0 < 0 ? 0 : y0 + hgt > 360 ? 360 - hgt : y0;
    double k = w / 720;                         /* dots and words keep their size as it zooms */
    sbuf b = { 0 };
    sb_printf(&b, "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"%.1f %.1f %.1f %.1f\" class=\"arv-map\" role=\"img\" "
                  "aria-label=\"A map of %zu place%s\">\n", x0, y0, w, hgt, shown, shown == 1 ? "" : "s");
    sb_printf(&b, "<style>.sea{fill:#eef3f6}.land{fill:#dfe4d8;stroke:#aab4a2;stroke-width:%.3f}"
                  ".dot{fill:#b8402f;fill-opacity:.85;stroke:#fff;stroke-width:%.3f}.lbl{font:%.2fpx sans-serif;fill:#222}"
                  "@media (prefers-color-scheme:dark){.sea{fill:#182026}.land{fill:#38423a;stroke:#56614f}"
                  ".dot{fill:#e0705a;stroke:#182026}.lbl{fill:#e6e6e6}}</style>\n",
              0.5 * k, 0.6 * k, 9 * k);
    sb_puts(&b, "<rect class=\"sea\" x=\"0\" y=\"0\" width=\"720\" height=\"360\"/>\n<path class=\"land\" d=\"");
    sb_add(&b, DATA_WORLD, strcspn(DATA_WORLD, "\n"));
    sb_puts(&b, "\"/>\n");
    place_use *sorted = xmalloc((u->n + 1) * sizeof *sorted);
    memcpy(sorted, u->v, u->n * sizeof *sorted);
    qsort(sorted, u->n, sizeof *sorted, by_uses);
    size_t labelled = 0;
    for (size_t i = 0; i < u->n; i++) {
        const known_entry *e = known_find(kv, kn, sorted[i].key);
        double la, lo;
        if (!e || (hide_private && e->private_) || !known_position(kv, kn, e, &la, &lo)) continue;
        double x = (lo + 180) * 2, y = (90 - la) * 2, r = (2.2 + root((double)(sorted[i].paths + sorted[i].objects))) * k;
        char *where = known_where(kv, kn, e);
        size_t n = sorted[i].paths + sorted[i].objects;
        sb_printf(&b, "<circle class=\"dot\" cx=\"%.2f\" cy=\"%.2f\" r=\"%.2f\"><title>", x, y, r);
        html_esc(&b, where);
        sb_printf(&b, ": %zu path%s or object%s</title></circle>\n", n, n == 1 ? "" : "s", n == 1 ? "" : "s");
        if (labelled++ < 12) {                  /* the most used named; the rest by their titles */
            sb_printf(&b, "<text class=\"lbl\" x=\"%.2f\" y=\"%.2f\">", x + r + 1.5 * k, y + 3 * k);
            html_esc(&b, e->label ? e->label : e->key + 6);
            sb_puts(&b, "</text>\n");
        }
        free(where);
    }
    sb_puts(&b, "</svg>\n");
    free(sorted);
    return b.s;
}

/* for a disc's index.html: its places, a map and a list; NULL when its tags name none. A public
 * disc says nothing of the places kept home (a house, to the metre) beyond their tags. */
char *map_disc_html(const arv_home *h, const char *tags_tsv, int public_)
{
    char *text = tags_tsv ? read_text(tags_tsv) : NULL;
    if (!text) return NULL;
    place_uses u = { 0 };
    count_tsv(&u, NULL, NULL, text);
    free(text);
    if (!u.n) {
        uses_free(&u);
        return NULL;
    }
    known_entry *kv;
    size_t kn = known_load(h, &kv);
    qsort(u.v, u.n, sizeof *u.v, by_uses);
    sbuf b = { 0 };
    sb_puts(&b, "\n<h2>Places</h2>\n");
    char *svg = map_svg(kv, kn, &u, public_);
    if (svg) {
        sb_printf(&b, "<figure class=\"map\">%s</figure>\n", svg);
        free(svg);
    }
    sb_puts(&b, "<table><tr><th>Place</th><th>Tag</th><th>Paths</th></tr>");
    for (size_t i = 0; i < u.n; i++) {
        const known_entry *e = known_find(kv, kn, u.v[i].key);
        if (e && public_ && e->private_) e = NULL;
        char *where = e ? known_where(kv, kn, e) : xstrdup("");
        sb_puts(&b, "\n<tr><td>");
        html_esc(&b, where);
        sb_puts(&b, "</td><td><code>");
        html_esc(&b, u.v[i].key);
        sb_printf(&b, "</code></td><td>%zu</td></tr>", u.v[i].paths);
        free(where);
    }
    sb_puts(&b, "\n</table>");
    known_free(kv, kn);
    uses_free(&u);
    return b.s;
}

/* ------------------------------------------------------------------ arv map */

static void json_place(sbuf *b, const known_entry *kv, size_t kn, const place_use *p, int geo)
{
    const known_entry *e = known_find(kv, kn, p->key);
    double la = 0, lo = 0;
    int pos = e ? known_position(kv, kn, e, &la, &lo) : 0;
    char *where = e ? known_where(kv, kn, e) : xstrdup("");
    if (geo) {
        sb_printf(b, "{\"type\": \"Feature\", \"geometry\": {\"type\": \"Point\", \"coordinates\": [%.4f, %.4f]}, \"properties\": {", lo, la);
    } else {
        sb_puts(b, "{");
    }
    sb_puts(b, "\"key\": "); json_str(b, p->key);
    sb_puts(b, ", \"label\": "); json_str(b, e && e->label ? e->label : "");
    sb_puts(b, ", \"where\": "); json_str(b, where);
    sb_puts(b, ", \"kind\": "); json_str(b, e && e->kind ? e->kind : "");
    if (!geo && pos) sb_printf(b, ", \"lat\": %.4f, \"lon\": %.4f", la, lo);
    sb_printf(b, ", \"position\": \"%s\"", pos == 2 ? "own" : pos == 1 ? "broader" : "none");
    sb_printf(b, ", \"inGazetteer\": %s, \"private\": %s", e ? "true" : "false", e && e->private_ ? "true" : "false");
    sb_printf(b, ", \"paths\": %zu, \"objects\": %zu, \"discs\": [", p->paths, p->objects);
    for (size_t k = 0; k < p->discs.n; k++) {
        if (k) sb_puts(b, ", ");
        json_str(b, p->discs.v[k]);
    }
    sb_puts(b, geo ? "]}}" : "]}");
    free(where);
}

int cmd_map(int argc, char **argv)
{
    const char *given = NULL;
    int as_json = 0, geo = 0, svg = 0;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!strcmp(argv[i], "--json")) as_json = 1;
        else if (!strcmp(argv[i], "--geojson")) geo = 1;
        else if (!strcmp(argv[i], "--svg")) svg = 1;
        else return 2;
    }
    if (as_json + geo + svg > 1) return 2;
    arv_home h;
    archive cat;
    home_find(&h, given, NULL);
    archive_load(&cat, h.rec_path);
    known_entry *kv;
    size_t kn = known_load(&h, &kv);
    place_uses u;
    uses_in_home(&h, &cat, &u);
    qsort(u.v, u.n, sizeof *u.v, by_uses);
    size_t placed = 0;
    for (size_t i = 0; i < u.n; i++) {
        const known_entry *e = known_find(kv, kn, u.v[i].key);
        double la, lo;
        placed += e && known_position(kv, kn, e, &la, &lo);
    }
    if (as_json || geo) {
        sbuf b = { 0 };
        sb_puts(&b, geo ? "{\"type\": \"FeatureCollection\", \"features\": [" : "{" JSON_FORMAT ", \"places\": [");
        int first = 1;
        for (size_t i = 0; i < u.n; i++) {
            const known_entry *e = known_find(kv, kn, u.v[i].key);
            double la, lo;
            if (geo && !(e && known_position(kv, kn, e, &la, &lo))) continue;   /* a feature needs a position */
            sb_puts(&b, first ? "\n  " : ",\n  ");
            first = 0;
            json_place(&b, kv, kn, &u.v[i], geo);
        }
        sb_puts(&b, "\n]}\n");
        fputs(b.s, stdout);
        free(b.s);
    } else if (svg) {
        char *s = map_svg(kv, kn, &u, 0);
        if (!s) die("%s", "no place the tags name has a position yet (arv map lists them)");
        fputs(s, stdout);
        free(s);
    } else if (!u.n) {
        puts("No place tags yet. Tag a path or an object place:kyoto (arv describe --ask offers the places it finds,");
        puts("and the nearest to its photos' GPS); arv tags show place:KEY says what the gazetteer knows of one.");
    } else {
        printf("%zu place%s named by the tags, %zu with a position (arv map --geojson for map tools, --svg for a picture):\n", u.n,
               u.n == 1 ? "" : "s", placed);
        size_t missing = 0;
        for (size_t i = 0; i < u.n; i++) {
            const known_entry *e = known_find(kv, kn, u.v[i].key);
            double la, lo;
            int pos = e ? known_position(kv, kn, e, &la, &lo) : 0;
            char *where = e ? known_where(kv, kn, e) : xstrdup("(not in the gazetteer)"), *at = pos ? iso6709(la, lo) : xstrdup("");
            printf("  %-24s %s", u.v[i].key, where);
            for (size_t w = utf8_chars(where); w < 34; w++) putchar(' ');
            printf(" %-15s", pos == 1 ? "(a broader one)" : at);
            if (u.v[i].paths) printf(" %zu path%s", u.v[i].paths, u.v[i].paths == 1 ? "" : "s");
            if (u.v[i].objects) printf("%s %zu object%s", u.v[i].paths ? "," : "", u.v[i].objects, u.v[i].objects == 1 ? "" : "s");
            printf(" on %zu disc%s%s\n", u.v[i].discs.n, u.v[i].discs.n == 1 ? "" : "s", e && e->private_ ? "; private" : "");
            missing += !e;
            free(where);
            free(at);
        }
        if (missing)
            printf("%zu not in the gazetteer: arv tags add place:KEY --parent place:kyoto [--at LAT,LON] adds one (with a\n"
                   "position, it stays home); a broader place's position is used until it has its own.\n", missing);
    }
    uses_free(&u);
    known_free(kv, kn);
    return 0;
}
