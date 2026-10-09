/* Editing the catalogue and looking at it (src/arv/cli.py): access, location, selection,
 * appraise, sets, names, where. Every change appends an Event, as the Python arv does, and the
 * output is the Python arv's, line for line. */
#define _XOPEN_SOURCE 700
#include "arv.h"
#include "data.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* printf's %-Ns counts bytes; Python counts characters */
static void pad(sbuf *b, const char *s, size_t width)
{
    sb_puts(b, s);
    for (size_t n = utf8_chars(s); n < width; n++) sb_puts(b, " ");
}

static char *upper_trim_copy(const char *s)
{
    while (s && isspace((unsigned char)*s)) s++;
    char *p = xstrdup(s ? s : ""), *e = p + strlen(p);
    while (e > p && isspace((unsigned char)e[-1])) *--e = 0;
    for (char *q = p; *q; q++) *q = (char)toupper((unsigned char)*q);
    return p;
}

static int code_ok(const char *code, size_t max)
{
    size_t n = strlen(code);
    if (!n || n > max || !(isupper((unsigned char)code[0]) || isdigit((unsigned char)code[0]))) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isupper((unsigned char)code[i]) && !isdigit((unsigned char)code[i]) && code[i] != '_' && code[i] != '-') return 0;
    return 1;
}

static void open_home(const char *given, arv_home *h, archive *cat)
{
    home_find(h, given, NULL);
    archive_load(cat, h->rec_path);
}

static void change(archive *cat, const char *disc_id, const char *obj, const char *note)
{
    char *who = person();
    rec_record *e = new_event(disc_id ? disc_id : obj, "metadata modification", "success", who, "human", note);
    if (!disc_id) {                  /* a place or a selection: Object instead of Disc */
        free(e->fields[0].name);
        e->fields[0].name = xstrdup("Object");
    }
    recs_add(&cat->events, e);
    free(who);
}

static int count_field(const rec_record *r, const char *name)
{
    int n = 0;
    for (size_t i = 0; i < r->nfields; i++) n += !strcmp(r->fields[i].name, name);
    return n;
}

/* list.insert(2, (name, value)): after Code and Name */
static void insert_at(rec_record *r, size_t at, const char *name, const char *value)
{
    rec_add(r, name, value);
    if (at >= r->nfields - 1) return;
    rec_field f = r->fields[r->nfields - 1];
    memmove(&r->fields[at + 1], &r->fields[at], (r->nfields - 1 - at) * sizeof *r->fields);
    r->fields[at] = f;
}

static void drop_field(rec_record *r, const char *name)
{
    size_t out = 0;
    for (size_t i = 0; i < r->nfields; i++) {
        if (!strcmp(r->fields[i].name, name)) {
            free(r->fields[i].name);
            free(r->fields[i].value);
        } else {
            r->fields[out++] = r->fields[i];
        }
    }
    r->nfields = out;
}

static const char *get_or(const rec_record *r, const char *name, const char *dflt)
{
    const char *v = r ? rec_get(r, name) : NULL;
    return v ? v : dflt;
}

/* ------------------------------------------------------------------ access */

int cmd_access(int argc, char **argv)
{
    const char *given = NULL, *disc_id = NULL, *level = NULL;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!disc_id) disc_id = argv[i];
        else if (!level) level = argv[i];
        else return 2;
    }
    if (!level || (strcmp(level, "public") && strcmp(level, "private") && strcmp(level, "sealed"))) return 2;
    arv_home h;
    archive cat;
    open_home(given, &h, &cat);
    rec_record *d = archive_disc(&cat, disc_id);
    if (!d) die2("no disc %s in %s", disc_id, h.rec_path);
    char before[16];
    snprintf(before, sizeof before, "%s", disc_access(d));
    rec_set(d, "Access", level);
    if (strcmp(before, level)) {
        char *note = xprintf("Access: %s -> %s", before, level);
        change(&cat, disc_id, NULL, note);
        free(note);
    }
    archive_save(&cat, h.rec_path);
    return 0;
}

/* ------------------------------------------------------------------ locations */

static rec_record *loc(const archive *cat, const char *code) { return archive_location(cat, code); }

static int location_under(const archive *cat, const char *value, const char *code)
{
    char *v = upper_trim_copy(value);
    int hit = !strcmp(v, code);
    for (rec_record *l = loc(cat, v); l && !hit; l = loc(cat, rec_get(l, "Parent")))
        hit = rec_get(l, "Code") && !strcmp(rec_get(l, "Code"), code);
    free(v);
    return hit;
}

static void show_location(const archive *cat, const rec_record *l, int depth, int verbose, strlist *seen)
{
    const char *code = rec_get(l, "Code");
    if (!code || strlist_has(seen, code) || depth > 64) return;
    strlist_add(seen, code);
    size_t direct = 0, inside = 0;
    for (size_t i = 0; i < cat->discs.n; i++) {
        const rec_record *d = cat->discs.v[i];
        int here = 0, in = 0;
        for (size_t f = 0; f < d->nfields; f++) {
            if (strcmp(d->fields[f].name, "Location")) continue;
            char *v = upper_trim_copy(d->fields[f].value);
            here |= !strcmp(v, code);
            free(v);
            in |= location_under(cat, d->fields[f].value, code);
        }
        direct += here;
        inside += in;
    }
    sbuf line = { 0 };
    for (int i = 0; i < depth; i++) sb_puts(&line, "  ");
    pad(&line, code, 10);
    sb_puts(&line, " ");
    pad(&line, get_or(l, "Name", "None"), 30);
    sb_printf(&line, " %zu disc%s", direct, direct == 1 ? "" : "s");
    if (inside != direct) sb_printf(&line, " (%zu including inside)", inside);
    if (rec_get(l, "Temperature")) sb_printf(&line, "  [%s]", rec_get(l, "Temperature"));
    puts(line.s);
    free(line.s);
    if (verbose)
        for (size_t i = 0; i < cat->discs.n; i++) {
            const rec_record *d = cat->discs.v[i];
            int here = 0;
            for (size_t f = 0; f < d->nfields; f++)
                if (!strcmp(d->fields[f].name, "Location")) {
                    char *v = upper_trim_copy(d->fields[f].value);
                    here |= !strcmp(v, code);
                    free(v);
                }
            if (here) printf("%*s  %s  %s\n", depth * 2 + 11, "", get_or(d, "Id", "None"), get_or(d, "Title", "None"));
        }
    for (size_t i = 0; i < cat->locations.n; i++) {
        char *p = upper_trim_copy(get_or(cat->locations.v[i], "Parent", ""));
        if (!strcmp(p, code)) show_location(cat, cat->locations.v[i], depth + 1, verbose, seen);
        free(p);
    }
    seen->n--;                      /* a place may appear under several branches only through cycles */
    free(seen->v[seen->n]);
}

static int by_string(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static char *place_path(const archive *cat, const char *code)
{
    rec_record probe = { 0 };
    probe.type = "Disc";
    rec_add(&probe, "Location", code);
    char *p = archive_where(cat, &probe);
    rec_clear(&probe);
    return p;
}

int cmd_location(int argc, char **argv)
{
    const char *given = NULL, *action = NULL, *code_arg = NULL, *name = NULL, *within = NULL, *description = NULL,
               *temperature = NULL;
    int verbose = 0;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) verbose = 1;
        else if (i + 1 < argc && !strcmp(argv[i], "--temperature")) temperature = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--in")) within = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--description")) description = argv[++i];
        else if (!action) action = argv[i];
        else if (!code_arg) code_arg = argv[i];
        else if (!name) name = argv[i];
        else return 2;
    }
    if (!action || (strcmp(action, "list") && strcmp(action, "add") && strcmp(action, "move"))) return 2;
    arv_home h;
    archive cat;
    open_home(given, &h, &cat);
    if (!strcmp(action, "list")) {
        strlist seen = { 0 }, loose = { 0 };
        for (size_t i = 0; i < cat.locations.n; i++)
            if (!loc(&cat, rec_get(cat.locations.v[i], "Parent"))) show_location(&cat, cat.locations.v[i], 0, verbose, &seen);
        for (size_t i = 0; i < cat.discs.n; i++)
            for (size_t f = 0; f < cat.discs.v[i]->nfields; f++) {
                const rec_field *fl = &cat.discs.v[i]->fields[f];
                if (!strcmp(fl->name, "Location") && !loc(&cat, fl->value) && !strlist_has(&loose, fl->value))
                    strlist_add(&loose, fl->value);
            }
        if (loose.n) qsort(loose.v, loose.n, sizeof *loose.v, by_string);
        for (size_t i = 0; i < loose.n; i++) {
            sbuf l = { 0 };
            pad(&l, loose.v[i], 10);
            printf("%s (free text, not a location code)\n", l.s);
            free(l.s);
        }
        return 0;
    }
    if (!code_arg) die("arv location %s needs a location code", action);
    if (temperature && !temperature_ok(temperature))
        die("--temperature %s: hot (in active use), warm (online or reachable, left alone) or cold (offline)",
            temperature);
    char *code = upper_trim_copy(code_arg);
    if (!code_ok(code, 24)) die("location code %s: use 1-24 capital letters, digits, - or _", code_arg);
    char *parent = within ? upper_trim_copy(within) : NULL;
    if (parent && !loc(&cat, parent)) die("no location %s (add it first)", parent);
    rec_record *l = loc(&cat, code);
    if (!strcmp(action, "add")) {
        if (l) die2("location %s already exists (use 'arv location move' or edit %s)", code, h.rec_path);
        l = rec_alloc("Location");
        rec_add(l, "Code", code);
        rec_add(l, "Name", name ? name : code);
        if (parent) rec_add(l, "Parent", parent);
        if (description) rec_add(l, "Description", description);
        if (temperature) rec_add(l, "Temperature", temperature);
        recs_add(&cat.locations, l);
        char *obj = xprintf("location:%s", code), *note = parent ? xprintf("created in %s", parent) : xstrdup("created");
        change(&cat, NULL, obj, note);
        free(obj);
        free(note);
    } else {
        if (!l) die("no location %s", code);
        for (rec_record *c = parent ? loc(&cat, parent) : NULL; c; c = loc(&cat, rec_get(c, "Parent")))
            if (rec_get(c, "Code") && !strcmp(rec_get(c, "Code"), code))
                die("%s is inside that place; that would make a loop", parent);
        char *parent_before = rec_get(l, "Parent") ? xstrdup(rec_get(l, "Parent")) : NULL;
        char *name_before = xstrdup(get_or(l, "Name", "None"));
        if (!parent && name && parent_before) parent = xstrdup(parent_before);   /* a rename keeps the place */
        drop_field(l, "Parent");
        if (parent) insert_at(l, 2, "Parent", parent);
        if (name) rec_set(l, "Name", name);
        sbuf changes = { 0 };
        const char *now = rec_get(l, "Parent");
        if ((now == NULL) != (parent_before == NULL) || (now && strcmp(now, parent_before)))
            sb_printf(&changes, "moved into %s", now ? now : "the top level");
        if (strcmp(get_or(l, "Name", "None"), name_before))
            sb_printf(&changes, "%srenamed %s -> %s", changes.len ? "; " : "", name_before, get_or(l, "Name", "None"));
        if (temperature && strcmp(get_or(l, "Temperature", ""), temperature)) {
            sb_printf(&changes, "%stemperature %s -> %s", changes.len ? "; " : "", get_or(l, "Temperature", "none"), temperature);
            rec_set(l, "Temperature", temperature);
        }
        if (changes.len) {
            char *obj = xprintf("location:%s", code);
            change(&cat, NULL, obj, changes.s);
            free(obj);
        }
        free(changes.s);
        free(parent_before);
        free(name_before);
    }
    archive_save(&cat, h.rec_path);
    char *path = place_path(&cat, code);
    printf("%s: %s\n", code, path);
    free(path);
    return 0;
}

/* ------------------------------------------------------------------ selections */

static rec_record *coll(const archive *cat, const char *code)
{
    char *want = upper_trim_copy(code);
    rec_record *found = NULL;
    for (size_t i = 0; i < cat->selections.n && !found; i++)
        if (rec_get(cat->selections.v[i], "Code") && !strcmp(rec_get(cat->selections.v[i], "Code"), want))
            found = cat->selections.v[i];
    free(want);
    return found;
}

static char *selection_path(const archive *cat, const char *code)
{
    const rec_record *chain[64];
    size_t n = 0;
    for (rec_record *c = coll(cat, code); c && n < 64; c = coll(cat, rec_get(c, "Parent"))) {
        size_t k;
        for (k = 0; k < n && chain[k] != c; k++) {}
        if (k < n) break;
        chain[n++] = c;
    }
    if (!n) return xstrdup(code);
    sbuf b = { 0 };
    while (n--) sb_printf(&b, "%s%s", get_or(chain[n], "Name", get_or(chain[n], "Code", "")), n ? " / " : "");
    return b.s;
}

static void show_selection(const archive *cat, const rec_record *c, int depth)
{
    if (depth > 64) return;
    int items = count_field(c, "Item");
    sbuf line = { 0 };
    for (int i = 0; i < depth; i++) sb_puts(&line, "  ");
    pad(&line, get_or(c, "Code", "None"), 12);
    sb_puts(&line, " ");
    pad(&line, get_or(c, "Name", "None"), 36);
    sb_printf(&line, " %d item%s", items, items == 1 ? "" : "s");
    puts(line.s);
    free(line.s);
    for (size_t i = 0; i < cat->selections.n; i++) {
        char *p = upper_trim_copy(get_or(cat->selections.v[i], "Parent", ""));
        if (!strcmp(p, get_or(c, "Code", ""))) show_selection(cat, cat->selections.v[i], depth + 1);
        free(p);
    }
}

/* "DISC-ID:path" -> disc id and path (relative to data/); 1 when the path names a folder */
static int parse_item(const char *item, char **disc_id, char **path)
{
    while (isspace((unsigned char)*item)) item++;
    const char *colon = strchr(item, ':');
    size_t n = colon ? (size_t)(colon - item) : strlen(item);
    *disc_id = xmalloc(n + 1);
    memcpy(*disc_id, item, n);
    (*disc_id)[n] = 0;
    while (n && isspace((unsigned char)(*disc_id)[n - 1])) (*disc_id)[--n] = 0;
    const char *p = colon ? colon + 1 : "";
    while (isspace((unsigned char)*p)) p++;
    *path = xstrdup(p);
    size_t pl = strlen(*path);
    while (pl && isspace((unsigned char)(*path)[pl - 1])) (*path)[--pl] = 0;
    char *q = *path;
    while (*q == '/') q++;
    memmove(*path, q, strlen(q) + 1);
    return !**path || (*path)[strlen(*path) - 1] == '/';
}

/* paths in a disc's listing at home (files in data/ only), or NULL without a listing */
static strlist *listing_paths(const arv_home *h, const char *disc_id)
{
    char *p = home_volume_file(h, disc_id, "listing.tsv");
    listing l;
    strlist *out = NULL;
    if (!read_listing(p, &l)) {
        out = calloc(1, sizeof *out);
        for (size_t i = 0; i < l.n; i++)
            if (l.r[i].size >= 0) strlist_add(out, l.r[i].path);
        free_listing(&l);
    }
    free(p);
    return out;
}

static int any_prefix(const strlist *l, const char *prefix)
{
    for (size_t i = 0; i < l->n; i++)
        if (!strncmp(l->v[i], prefix, strlen(prefix))) return 1;
    return 0;
}

int cmd_selection(int argc, char **argv)
{
    const char *given = NULL, *action = NULL, *code_arg = NULL, *name = NULL, *within = NULL, *description = NULL;
    strlist items = { 0 };
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--name")) name = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--in")) within = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--description")) description = argv[++i];
        else if (!action) action = argv[i];
        else if (!code_arg) code_arg = argv[i];
        else strlist_add(&items, argv[i]);
    }
    static const char *const actions[] = { "list", "show", "add", "put", "drop", "move", NULL };
    int known = 0;
    for (int k = 0; actions[k] && action; k++) known |= !strcmp(action, actions[k]);
    if (!known) return 2;
    arv_home h;
    archive cat;
    open_home(given, &h, &cat);
    if (!strcmp(action, "list")) {
        for (size_t i = 0; i < cat.selections.n; i++)
            if (!coll(&cat, rec_get(cat.selections.v[i], "Parent"))) show_selection(&cat, cat.selections.v[i], 0);
        return 0;
    }
    if (!code_arg) die("arv selection %s needs a selection code", action);
    char *code = upper_trim_copy(code_arg);
    rec_record *c = coll(&cat, code);
    if (!strcmp(action, "show")) {
        if (!c) die("no selection %s", code);
        char *path = selection_path(&cat, code);
        printf("%s  %s\n", code, path);
        free(path);
        if (rec_get(c, "Description")) printf("  %s\n", rec_get(c, "Description"));
        for (size_t f = 0; f < c->nfields; f++) {
            if (strcmp(c->fields[f].name, "Item")) continue;
            char *id, *p;
            parse_item(c->fields[f].value, &id, &p);
            rec_record *d = archive_disc(&cat, id);
            char *w = d ? archive_where(&cat, d) : xstrdup("not in the catalogue");
            sbuf line = { 0 };
            sb_puts(&line, "  ");
            pad(&line, *p ? p : "(whole disc)", 50);
            sb_printf(&line, " %s  [%s]", id, w);
            puts(line.s);
            free(line.s);
            free(w);
            free(id);
            free(p);
        }
        return 0;
    }
    sbuf changes = { 0 };
    if (!strcmp(action, "add") && c) {     /* add to one that exists: its items go in, as put does */
        if (name || within || description || !items.n)
            die("selection %s already exists (arv selection put CODE ITEM... adds to it; move changes its name or place)",
                code);
        fprintf(stderr, "Note: selection %s already exists: adding to it (as arv selection put does)\n", code);
        action = "put";
    }
    if (!strcmp(action, "add")) {
        if (!code_ok(code, 32)) die("selection code %s: use 1-32 capital letters, digits, - or _", code_arg);
        if (within && !coll(&cat, within)) {
            char *w = upper_trim_copy(within);
            die("no selection %s (add it first)", w);
        }
        c = rec_alloc("Selection");
        rec_add(c, "Code", code);
        rec_add(c, "Name", name ? name : code);
        char *w = within ? upper_trim_copy(within) : NULL;
        if (w) rec_add(c, "Parent", w);
        if (description) rec_add(c, "Description", description);
        recs_add(&cat.selections, c);
        sb_printf(&changes, "created%s%s", w ? " in " : "", w ? w : "");
        free(w);
    } else if (!c) {
        die("no selection %s", code);
    }
    int items_before = count_field(c, "Item");
    char *parent_before = rec_get(c, "Parent") ? xstrdup(rec_get(c, "Parent")) : NULL;
    char *name_before = xstrdup(get_or(c, "Name", "None"));
    if (!strcmp(action, "add") || !strcmp(action, "put")) {
        for (size_t i = 0; i < items.n; i++) {
            char *id, *p;
            int is_folder = parse_item(items.v[i], &id, &p);
            if (!archive_disc(&cat, id))
                die("no disc %s in the catalogue (items are DISC-ID, DISC-ID:folder/ or DISC-ID:folder/file)", id);
            strlist *paths = *p ? listing_paths(&h, id) : NULL;
            if (*p && paths && !(is_folder ? any_prefix(paths, p) : strlist_has(paths, p))) {
                char *as_folder = xprintf("%s/", p);
                if (!is_folder && any_prefix(paths, as_folder)) {     /* a folder given without its slash */
                    free(p);
                    p = as_folder;
                } else {
                    fprintf(stderr, "Error: %s has no %s %s\n", id, is_folder ? "folder" : "file", p);
                    exit(1);
                }
            }
            char *item = *p ? xprintf("%s:%s", id, p) : xstrdup(id);
            int have = 0;
            for (size_t f = 0; f < c->nfields; f++) have |= !strcmp(c->fields[f].name, "Item") && !strcmp(c->fields[f].value, item);
            if (!have) rec_add(c, "Item", item);
            free(item);
            free(id);
            free(p);
        }
    } else if (!strcmp(action, "drop")) {
        strlist gone = { 0 };
        for (size_t i = 0; i < items.n; i++) {
            char *id, *p;
            parse_item(items.v[i], &id, &p);
            char *item = *p ? xprintf("%s:%s", id, p) : xstrdup(id);
            strlist_add(&gone, item);
            free(item);
            free(id);
            free(p);
        }
        size_t out = 0;
        for (size_t f = 0; f < c->nfields; f++) {
            if (!strcmp(c->fields[f].name, "Item") && strlist_has(&gone, c->fields[f].value)) {
                free(c->fields[f].name);
                free(c->fields[f].value);
            } else {
                c->fields[out++] = c->fields[f];
            }
        }
        c->nfields = out;
    } else if (!strcmp(action, "move")) {
        for (rec_record *x = within ? coll(&cat, within) : NULL; x; x = coll(&cat, rec_get(x, "Parent")))
            if (rec_get(x, "Code") && !strcmp(rec_get(x, "Code"), code)) {
                char *w = upper_trim_copy(within);
                fprintf(stderr, "Error: %s is inside %s; that would make a loop\n", w, code);
                exit(1);
            }
        drop_field(c, "Parent");
        if (within) {
            char *w = upper_trim_copy(within);
            insert_at(c, 2, "Parent", w);
            free(w);
        }
        if (name) rec_set(c, "Name", name);
    }
    int delta = count_field(c, "Item") - items_before;      /* counts only: paths could name sealed files */
    if (delta > 0) sb_printf(&changes, "%s%d item%s added", changes.len ? "; " : "", delta, delta == 1 ? "" : "s");
    else if (delta < 0) sb_printf(&changes, "%s%d item%s removed", changes.len ? "; " : "", -delta, delta == -1 ? "" : "s");
    if (!strcmp(action, "move")) {
        const char *now = rec_get(c, "Parent");
        if ((now == NULL) != (parent_before == NULL) || (now && strcmp(now, parent_before)))
            sb_printf(&changes, "%smoved into %s", changes.len ? "; " : "", now ? now : "the top level");
        if (strcmp(get_or(c, "Name", "None"), name_before))
            sb_printf(&changes, "%srenamed %s -> %s", changes.len ? "; " : "", name_before, get_or(c, "Name", "None"));
    }
    if (changes.len) {
        char *obj = xprintf("selection:%s", code);
        change(&cat, NULL, obj, changes.s);
        free(obj);
    }
    archive_save(&cat, h.rec_path);
    char *path = selection_path(&cat, code);
    printf("%s: %s, %d items\n", code, path, count_field(c, "Item"));
    free(path);
    free(changes.s);
    return 0;
}

/* ------------------------------------------------------------------ appraisals (src/arv/appraisal.py) */

static int standing(const rec_record *a)
{
    const char *how = rec_get(a, "Authorship");
    if (!how) {                         /* format 0.3 agent text */
        sbuf agents = { 0 };
        for (size_t f = 0; f < a->nfields; f++)
            if (!strcmp(a->fields[f].name, "Agent")) sb_printf(&agents, "%s ", a->fields[f].value);
        const char *t = agents.s ? agents.s : "";
        how = strstr(t, "(unreviewed)") ? "suggested" : strstr(t, "+ owner review") ? "accepted"
            : (strstr(t, "llm:") || strstr(t, "embeddings:") || strstr(t, "vision:")) ? "suggested"
            : strstr(t, "human:") ? "human" : "automatic";
        int s = !strcmp(how, "suggested") ? 0 : !strcmp(how, "automatic") ? 1 : 2;
        free(agents.s);
        return s;
    }
    return !strcmp(how, "suggested") ? 0 : !strcmp(how, "automatic") ? 1
         : (!strcmp(how, "human") || !strcmp(how, "accepted") || !strcmp(how, "edited")) ? 2 : 0;
}

/* the appraisal in force for exactly this target: highest standing, then newest, then last */
static rec_record *current(const archive *cat, const char *target)
{
    rec_record *best = NULL;
    int best_s = -1;
    const char *best_d = "";
    for (size_t i = 0; i < cat->appraisals.n; i++) {
        rec_record *a = cat->appraisals.v[i];
        if (!rec_get(a, "Target") || strcmp(rec_get(a, "Target"), target)) continue;
        int s = standing(a);
        const char *d = get_or(a, "Date", "");
        if (s > best_s || (s == best_s && strcmp(d, best_d) >= 0)) {
            best = a;
            best_s = s;
            best_d = d;
        }
    }
    return best;
}

/* the most a disc matters: the highest level (0 essential .. 3 incidental) in force on the disc,
 * anything on it, or its set, with *why its "<level> for <audience>"; -1 when nothing is appraised.
 * A model's unreviewed suggestion does not count. */
int disc_importance(const archive *cat, const char *id, char **why)
{
    static const char *const levels[] = { "essential", "important", "useful", "incidental" };
    const rec_record *d = archive_disc(cat, id);
    char *set = d && rec_get(d, "Set") ? xprintf("set:%s", rec_get(d, "Set")) : NULL;
    size_t n = strlen(id);
    strlist done = { 0 };
    int best = -1;
    *why = NULL;
    for (size_t i = 0; i < cat->appraisals.n; i++) {
        const char *t = rec_get(cat->appraisals.v[i], "Target");
        if (!t || strlist_has(&done, t)) continue;
        strlist_add(&done, t);
        if (!(set && !strcmp(t, set)) && !(!strncmp(t, id, n) && (!t[n] || t[n] == ':'))) continue;
        const rec_record *a = current(cat, t);
        if (!standing(a)) continue;
        for (size_t f = 0; f < a->nfields; f++) {
            if (strcmp(a->fields[f].name, "Importance")) continue;
            for (int k = 0; k < 4; k++) {
                size_t m = strlen(levels[k]);
                if (strncmp(a->fields[f].value, levels[k], m) || !isspace((unsigned char)a->fields[f].value[m])) continue;
                if (best < 0 || k < best) {
                    best = k;
                    free(*why);
                    *why = xstrdup(a->fields[f].value);
                }
            }
        }
    }
    strlist_free(&done);
    free(set);
    return best;
}

/* the target and everything above it, most specific first: file, folders, disc, set */
static void chain(const archive *cat, const char *target, strlist *out)
{
    if (!strncmp(target, "set:", 4) || selection_target(target)) {
        strlist_add(out, target);
        return;
    }
    char *id, *p;
    parse_item(target, &id, &p);
    if (*p) {
        size_t n = strlen(p);
        int folder = p[n - 1] == '/';
        while (n && p[n - 1] == '/') p[--n] = 0;
        if (!folder) {
            char *s = xprintf("%s:%s", id, p);
            strlist_add(out, s);
            free(s);
            char *slash = strrchr(p, '/');
            if (slash) *slash = 0;
            else *p = 0;
        }
        while (*p) {
            char *s = xprintf("%s:%s/", id, p);
            strlist_add(out, s);
            free(s);
            char *slash = strrchr(p, '/');
            if (slash) *slash = 0;
            else *p = 0;
        }
    }
    strlist_add(out, id);
    rec_record *d = archive_disc(cat, id);
    if (d && rec_get(d, "Set")) {
        char *s = xprintf("set:%s", rec_get(d, "Set"));
        strlist_add(out, s);
        free(s);
    }
    free(id);
    free(p);
}

static void check_target(const arv_home *h, const archive *cat, const char *target)
{
    if (selection_target(target)) {
        if (!coll(cat, selection_target(target))) die("no selection %s", selection_target(target));
        return;
    }
    if (!strncmp(target, "set:", 4)) {
        const char *code = target + 4;
        for (size_t i = 0; i < cat->discs.n; i++)
            if (rec_get(cat->discs.v[i], "Set") && !strcmp(rec_get(cat->discs.v[i], "Set"), code)) return;
        char *sets_path = join(h->config_dir, "sets.rec"), err[512];
        vocab v;
        int ok = !access(sets_path, F_OK) ? !vocab_load(&v, sets_path, NULL, err, sizeof err)
                                          : !vocab_load(&v, NULL, DATA_DEFAULT_SETS, err, sizeof err);
        if (!ok || !vocab_resolve(&v, code))
            die("no set %s (no disc has it, and the vocabulary does not know it)", code);
        return;
    }
    char *id, *p;
    int is_folder = parse_item(target, &id, &p);
    if (!archive_disc(cat, id))
        die("no disc %s in the catalogue (targets: DISC-ID, DISC-ID:folder/, DISC-ID:folder/file, set:CODE, "
            "selection:CODE)", id);
    strlist *paths = *p ? listing_paths(h, id) : NULL;
    if (paths && !(is_folder ? any_prefix(paths, p) : strlist_has(paths, p))) {
        char *as_folder = xprintf("%s/", p);
        fprintf(stderr, "Error: %s is not on %s%s\n", p, id,
                !is_folder && any_prefix(paths, as_folder) ? " (folders end with /)" : "");
        exit(1);
    }
}

static char *review_date(const char *text)
{
    char unit = 0;
    int n = 0;
    while (isspace((unsigned char)*text)) text++;
    if (sscanf(text, "%d %c", &n, &unit) == 2 && n >= 0 && strchr("ymYM", unit)) {
        struct tm tm;
        today_tm(&tm);
        int months = tm.tm_mon + n * (tolower((unsigned char)unit) == 'y' ? 12 : 1);
        return xprintf("%04d-%02d-%02d", tm.tm_year + 1900 + months / 12, months % 12 + 1, tm.tm_mday < 28 ? tm.tm_mday : 28);
    }
    long a, b;
    if (strlen(text) == 10 && edtf_span(text, &a, &b) == 1 && a == b) return xstrdup(text);
    die("review date: YYYY-MM-DD or a span such as 5y or 18m, not %s", text);
    return NULL;
}

int cmd_appraise(int argc, char **argv)
{
    const char *given = NULL, *target_arg = NULL, *basis = NULL, *review = NULL, *due = NULL;
    int due_given = 0;
    strlist importance = { 0 };
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--importance")) strlist_add(&importance, argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--basis")) basis = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--review")) review = argv[++i];
        else if (!strcmp(argv[i], "--due")) {
            due_given = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-' && isdigit((unsigned char)argv[i + 1][0])) due = argv[++i];
        } else if (!target_arg) target_arg = argv[i];
        else return 2;
    }
    arv_home h;
    archive cat;
    open_home(given, &h, &cat);
    if (due_given || !target_arg) {
        char today[11];
        today_iso(today);
        const char *on = due ? due : today;
        strlist targets = { 0 };
        for (size_t i = 0; i < cat.appraisals.n; i++) {
            const char *t = rec_get(cat.appraisals.v[i], "Target");
            if (t && !strlist_has(&targets, t)) strlist_add(&targets, t);
        }
        recs rows = { 0 };
        for (size_t i = 0; i < targets.n; i++) {
            rec_record *a = current(&cat, targets.v[i]);
            if (a && rec_get(a, "Review") && strcmp(rec_get(a, "Review"), on) <= 0) recs_add(&rows, a);
        }
        for (size_t i = 1; i < rows.n; i++)            /* stable sort by review date */
            for (size_t k = i; k > 0 && strcmp(rec_get(rows.v[k - 1], "Review"), rec_get(rows.v[k], "Review")) > 0; k--) {
                rec_record *t = rows.v[k];
                rows.v[k] = rows.v[k - 1];
                rows.v[k - 1] = t;
            }
        for (size_t i = 0; i < rows.n; i++) {
            sbuf imp = { 0 };
            sb_puts(&imp, "");
            for (size_t f = 0; f < rows.v[i]->nfields; f++)
                if (!strcmp(rows.v[i]->fields[f].name, "Importance"))
                    sb_printf(&imp, "%s%s", imp.len ? "; " : "", rows.v[i]->fields[f].value);
            printf("%s\treview %s\t%s\n", rec_get(rows.v[i], "Target"), rec_get(rows.v[i], "Review"), imp.s);
            free(imp.s);
        }
        if (!rows.n) fprintf(stderr, "No appraisals due for review%s%s.\n", due ? " by " : "", due ? due : "");
        return 0;
    }
    char *target = xstrdup(target_arg);
    {
        char *s = target;
        while (isspace((unsigned char)*s)) s++;
        memmove(target, s, strlen(s) + 1);
        size_t n = strlen(target);
        while (n && isspace((unsigned char)target[n - 1])) target[--n] = 0;
    }
    check_target(&h, &cat, target);
    if (!importance.n && !basis) {
        strlist up = { 0 };
        chain(&cat, target, &up);
        rec_record *a = NULL;
        const char *source = NULL;
        for (size_t i = 0; i < up.n && !a; i++)
            if ((a = current(&cat, up.v[i]))) source = up.v[i];
        if (!a) {
            printf("%s: not appraised\n", target);
            return 0;
        }
        if (!strcmp(source, target)) printf("%s\n", target);
        else printf("%s  (from %s)\n", target, source);
        for (size_t f = 0; f < a->nfields; f++)
            if (!strcmp(a->fields[f].name, "Importance")) printf("  %s\n", a->fields[f].value);
        static const char *const keys[] = { "Basis", "Review", "Date", "Authorship", NULL };
        for (int k = 0; keys[k]; k++)
            if (rec_get(a, keys[k]) && *rec_get(a, keys[k])) printf("  %s: %s\n", keys[k], rec_get(a, keys[k]));
        sbuf agents = { 0 };
        sb_puts(&agents, "");
        for (size_t f = 0; f < a->nfields; f++)
            if (!strcmp(a->fields[f].name, "Agent")) sb_printf(&agents, "%s%s", agents.len ? ", " : "", a->fields[f].value);
        printf("  Agent: %s\n", agents.s);
        size_t earlier = 0;
        for (size_t i = 0; i < cat.appraisals.n; i++)
            earlier += cat.appraisals.v[i] != a && rec_get(cat.appraisals.v[i], "Target") &&
                       !strcmp(rec_get(cat.appraisals.v[i], "Target"), target);
        if (earlier) printf("  (%zu earlier appraisal%s of this target in the log)\n", earlier, earlier == 1 ? "" : "s");
        return 0;
    }
    char *when = review ? review_date(review) : NULL;
    rec_record *a = new_appraisal(target, &importance, basis, when);
    recs_add(&cat.appraisals, a);
    archive_save(&cat, h.rec_path);
    sbuf imp = { 0 };
    for (size_t f = 0; f < a->nfields; f++)
        if (!strcmp(a->fields[f].name, "Importance")) sb_printf(&imp, "%s%s", imp.len ? "; " : "", a->fields[f].value);
    printf("%s: %s\n", target, imp.len ? imp.s : get_or(a, "Basis", ""));
    free(imp.s);
    return 0;
}

/* ------------------------------------------------------------------ sets */

typedef struct {
    const archive *cat;
    const vocab *v;
    int verbose;
} sets_ctx;

static int disc_in_code(const rec_record *d, const char *code)
{
    for (size_t f = 0; f < d->nfields; f++) {
        const char *n = d->fields[f].name, *v = d->fields[f].value;
        if ((!strcmp(n, "Set") || !strcmp(n, "Category")) && !strcmp(v, code)) return 1;
        if (!strcmp(n, "Path")) {
            char *copy = xstrdup(v);
            int hit = 0;
            for (char *t = strtok(copy, "/"); t && !hit; t = strtok(NULL, "/")) hit = !strcmp(t, code);
            free(copy);
            if (hit) return 1;
        }
    }
    return 0;
}

static size_t direct_count(const archive *cat, const char *code)
{
    size_t n = 0;
    for (size_t i = 0; i < cat->discs.n; i++) {
        const rec_record *d = cat->discs.v[i];
        int hit = rec_get(d, "Set") && !strcmp(rec_get(d, "Set"), code);
        for (size_t f = 0; f < d->nfields && !hit; f++)
            hit = !strcmp(d->fields[f].name, "Category") && !strcmp(d->fields[f].value, code);
        n += hit;
    }
    return n;
}

static int by_order(const void *a, const void *b)
{
    const vset *x = *(const vset *const *)a, *y = *(const vset *const *)b;
    if (x->order != y->order) return x->order < y->order ? -1 : 1;
    return strcmp(x->code, y->code);
}

static void show_set(const sets_ctx *s, const vset *e, int depth)
{
    if (depth > 64) return;
    size_t within = 0, direct = direct_count(s->cat, e->code);
    for (size_t i = 0; i < s->cat->discs.n; i++) within += disc_in_code(s->cat->discs.v[i], e->code);
    sbuf line = { 0 };
    for (int i = 0; i < depth; i++) sb_puts(&line, "  ");
    pad(&line, e->code, 8);
    sb_puts(&line, " ");
    pad(&line, e->name, 28);
    sb_puts(&line, " ");
    if (within) {
        sb_printf(&line, "%zu disc%s", direct, direct == 1 ? "" : "s");
        if (within != direct) sb_printf(&line, " (%zu including below)", within);
    }
    if (e->parents.n > 1) {
        sb_puts(&line, "   [also under ");
        for (size_t i = 0; i < e->parents.n; i++) sb_printf(&line, "%s%s", i ? ", " : "", e->parents.v[i]);
        sb_puts(&line, "]");
    }
    puts(line.s);
    free(line.s);
    if (s->verbose) {
        sbuf aliases = { 0 }, matches = { 0 };
        for (size_t i = 0; i < e->aliases.n; i++) sb_printf(&aliases, "%s%s", i ? ", " : "", e->aliases.v[i]);
        for (size_t i = 0; i < e->matches.n; i++) sb_printf(&matches, "%s%s", i ? " " : "", e->matches.v[i]);
        const char *labels[] = { "scope", "aliases", "matches" };
        const char *values[] = { e->scope_note, aliases.s ? aliases.s : "", matches.s ? matches.s : "" };
        for (int k = 0; k < 3; k++)
            if (*values[k]) printf("%*s%s: %s\n", depth * 2 + 9, "", labels[k], values[k]);
        free(aliases.s);
        free(matches.s);
    }
    const vset **kids = xmalloc((s->v->n + 1) * sizeof *kids);
    size_t n = 0;
    for (size_t i = 0; i < s->v->n; i++)
        if (strlist_has(&s->v->e[i].parents, e->code)) kids[n++] = &s->v->e[i];
    if (n) qsort(kids, n, sizeof *kids, by_order);
    for (size_t i = 0; i < n; i++) show_set(s, kids[i], depth + 1);
    free(kids);
}

int cmd_sets(int argc, char **argv)
{
    const char *given = NULL;
    int verbose = 0;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) verbose = 1;
        else return 2;
    }
    arv_home h;
    archive cat;
    open_home(given, &h, &cat);
    char *sets_path = join(h.config_dir, "sets.rec"), err[512];
    if (access(sets_path, F_OK)) {               /* created from the default on first use */
        if (mkdirs(h.config_dir)) die("cannot create %s", h.config_dir);
        write_text(sets_path, DATA_DEFAULT_SETS);
    }
    vocab v;
    if (vocab_load(&v, sets_path, NULL, err, sizeof err)) die("%s", err);
    sets_ctx s = { &cat, &v, verbose };
    const vset **roots = xmalloc((v.n + 1) * sizeof *roots);
    size_t n = 0;
    for (size_t i = 0; i < v.n; i++)
        if (!v.e[i].parents.n) roots[n++] = &v.e[i];
    if (n) qsort(roots, n, sizeof *roots, by_order);
    for (size_t i = 0; i < n; i++) show_set(&s, roots[i], 0);
    free(roots);
    strlist unknown = { 0 };
    for (size_t i = 0; i < cat.discs.n; i++) {
        const rec_record *d = cat.discs.v[i];
        for (size_t f = 0; f < d->nfields; f++)
            if ((!strcmp(d->fields[f].name, "Set") || !strcmp(d->fields[f].name, "Category")) && *d->fields[f].value &&
                !vocab_get(&v, d->fields[f].value) && !strlist_has(&unknown, d->fields[f].value))
                strlist_add(&unknown, d->fields[f].value);
    }
    if (unknown.n) qsort(unknown.v, unknown.n, sizeof *unknown.v, by_string);
    for (size_t i = 0; i < unknown.n; i++) {
        size_t c = direct_count(&cat, unknown.v[i]);
        sbuf line = { 0 };
        pad(&line, unknown.v[i], 8);
        printf("%s (not in %s) %zu disc%s\n", line.s, sets_path, c, c == 1 ? "" : "s");
        free(line.s);
    }
    return 0;
}

/* ------------------------------------------------------------------ names */

/* file paths under dir in os.walk order: a folder's files, then its folders (sorted) */
static void walk_names(const char *dir, const char *rel, strlist *out)
{
    DIR *d = opendir(dir);
    strlist files = { 0 }, dirs = { 0 };
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *full = join(dir, e->d_name);
        struct stat st, lst;
        int is_dir = !stat(full, &st) && S_ISDIR(st.st_mode);
        if (is_dir) {
            if (!lstat(full, &lst) && !S_ISLNK(lst.st_mode)) strlist_add(&dirs, e->d_name);  /* links to folders: not walked */
        } else {
            strlist_add(&files, e->d_name);
        }
        free(full);
    }
    closedir(d);
    if (files.n) qsort(files.v, files.n, sizeof *files.v, by_string);
    if (dirs.n) qsort(dirs.v, dirs.n, sizeof *dirs.v, by_string);
    for (size_t i = 0; i < files.n; i++) {
        char *r = *rel ? join(rel, files.v[i]) : xstrdup(files.v[i]);
        strlist_add(out, r);
        free(r);
    }
    for (size_t i = 0; i < dirs.n; i++) {
        char *full = join(dir, dirs.v[i]), *r = *rel ? join(rel, dirs.v[i]) : xstrdup(dirs.v[i]);
        walk_names(full, r, out);
        free(full);
        free(r);
    }
    strlist_free(&files);
    strlist_free(&dirs);
}

static void report(const name_issues *x, size_t limit)
{
    for (int pass = 1; pass >= 0; pass--) {          /* errors, then warnings */
        size_t n = 0, shown = 0;
        for (size_t i = 0; i < x->n; i++) {
            if (x->v[i].error != pass) continue;
            n++;
            if (shown >= limit) continue;
            shown++;
            const char *p = x->v[i].path;
            size_t chars = utf8_chars(p);
            sbuf s = { 0 };
            if (chars <= 70) sb_puts(&s, p);
            else {                                    /* path[:40] + "…" + path[-25:] */
                const char *q = p;
                for (int k = 0; k < 40; k++) utf8_next(&q);
                sb_add(&s, p, (size_t)(q - p));
                sb_puts(&s, "\xe2\x80\xa6");
                const char *t = p;
                for (size_t k = 0; k < chars - 25; k++) utf8_next(&t);
                sb_puts(&s, t);
            }
            printf("  %s: %s: %s\n", pass ? "error" : "warning", s.s, x->v[i].problem);
            free(s.s);
        }
        if (n > limit) printf("  ... and %zu more %ss\n", n - limit, pass ? "error" : "warning");
    }
}

int cmd_names(int argc, char **argv)
{
    const char *source = NULL;
    long limit = 20;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) i++;
        else if (i + 1 < argc && !strcmp(argv[i], "--limit")) limit = atol(argv[++i]);
        else if (!source) source = argv[i];
        else return 2;
    }
    if (!source) return 2;
    struct stat st;
    char *src = realpath(source, NULL);
    if (!src || stat(src, &st) || !S_ISDIR(st.st_mode)) die("%s is not a directory", source);
    strlist paths = { 0 };
    walk_names(src, "", &paths);
    int errors = 0;
    name_issues x = { 0 };
    names_check(paths.v, paths.n, &x);
    if (!x.n) printf("all %zu names kept exactly\n", paths.n);
    else printf("%zu issue%s\n", x.n, x.n == 1 ? "" : "s");
    report(&x, limit ? (size_t)limit : x.n);
    for (size_t i = 0; i < x.n; i++) errors |= x.v[i].error;
    names_free(&x);
    return errors ? 1 : 0;
}

/* ------------------------------------------------------------------ where */

int cmd_where(int argc, char **argv)
{
    const char *given = NULL;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else return 2;
    }
    arv_home h;
    home_find(&h, given, NULL);
    printf("%s\n  found by %s\n", h.path, h.how);
    archive cat;
    archive_load(&cat, h.rec_path);
    if (cat.homes.n)
        printf("  archive %s%s%s\n", get_or(cat.homes.v[0], "Uuid", "?"), rec_get(cat.homes.v[0], "Name") ? ", " : "",
               rec_get(cat.homes.v[0], "Name") ? rec_get(cat.homes.v[0], "Name") : "");
    else
        puts("  archive: no identity yet (made by arv init, or the first change)");
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char *config = xdg && *xdg ? join(xdg, "arv/homes.rec") : xprintf("%s/.config/arv/homes.rec", home ? home : "");
    rec_file f;
    int bad = 0;
    if (!access(config, F_OK) && !rec_read(config, &f, &bad)) {
        int any = 0;
        for (size_t i = 0; i < f.nrecords; i++) {
            const rec_record *r = &f.records[i];
            if (r->descriptor || !r->type || strcmp(r->type, "Home")) continue;
            if (!any++) printf("homes on this machine (%s):\n", config);
            sbuf line = { 0 };
            sb_puts(&line, "  ");
            pad(&line, get_or(r, "Name", "None"), 12);
            sb_printf(&line, " %s%s", get_or(r, "Path", "None"),
                      rec_get(r, "Default") && !strcmp(rec_get(r, "Default"), "yes") ? "  (default)" : "");
            puts(line.s);
            free(line.s);
        }
        rec_free(&f);
    }
    free(config);
    return 0;
}

/* ------------------------------------------------------------------ rebuild */

/* the disc id of a mounted or extracted disc, from its bag-info.txt */
static char *disc_root_id(const char *root)
{
    char *p = join(root, "bag-info.txt"), *text = read_text(p), *out = NULL;
    free(p);
    for (char *l = text ? strtok(text, "\n") : NULL; l && !out; l = strtok(NULL, "\n"))
        if (!strncmp(l, "External-Identifier:", 20)) {
            char *v = l + 20;
            while (isspace((unsigned char)*v)) v++;
            size_t n = strlen(v);
            while (n && isspace((unsigned char)v[n - 1])) v[--n] = 0;
            out = xstrdup(v);
        }
    free(text);
    return out;
}

/* Merge the catalogue carried by a disc (mounted or extracted) into the home catalogue. */
int cmd_rebuild(int argc, char **argv)
{
    const char *given = NULL, *root = NULL;
    int prefer = 0, any_archive = 0;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!strcmp(argv[i], "--prefer-disc")) prefer = 1;
        else if (!strcmp(argv[i], "--any-archive")) any_archive = 1;
        else if (!root) root = argv[i];
        else return 2;
    }
    if (!root) return 2;
    arv_home h;
    archive cat;
    open_home(given, &h, &cat);
    char *snap_dir = join(root, "catalog");
    char *sources[] = { join(snap_dir, "archive.rec"), join(root, "catalog.rec") };
    int any = 0;
    strlist added = { 0 }, updated = { 0 };
    size_t events = 0;
    for (int k = 0; k < 2; k++) {
        if (access(sources[k], F_OK)) continue;
        any = 1;
        archive *other = calloc(1, sizeof *other);      /* its records join the home catalogue: kept */
        archive_load(other, sources[k]);
        /* a disc belongs to one home: another home's disc is not merged by accident */
        const char *theirs = other->homes.n ? rec_get(other->homes.v[0], "Uuid") : NULL;
        if (!theirs && k == 1 && other->file.nrecords) {
            const rec_record *arc = rec_first(&other->file, "Archive");
            theirs = arc ? rec_get(arc, "HomeUuid") : NULL;
        }
        int fresh = !cat.homes.n && !cat.discs.n;      /* an empty home takes the disc's identity */
        if (theirs && !fresh && strcmp(theirs, archive_home_uuid(&cat)) && !any_archive) {
            const char *nm = other->homes.n && rec_get(other->homes.v[0], "Name") ? rec_get(other->homes.v[0], "Name") : NULL;
            fprintf(stderr, "Error: this disc belongs to another archive (home %s%s%s), not to %s (home %s).\n"
                            "Each archive is its own privacy sphere; --any-archive merges it anyway.\n",
                    theirs, nm ? ", " : "", nm ? nm : "", h.path, archive_home_uuid(&cat));
            exit(1);
        }
        if (theirs && fresh && !cat.homes.n) {     /* the whole record (name, date) when the disc has it */
            rec_record *r = other->homes.n ? other->homes.v[0] : rec_alloc("Home");
            if (!other->homes.n) rec_add(r, "Uuid", theirs);
            recs_add(&cat.homes, r);
        }
        events += archive_merge(&cat, other, prefer, &added, &updated);
    }
    if (!any) die("%s has neither catalog/archive.rec nor catalog.rec", root);
    static const char *const KINDS[] = { "manifest.sha256", "listing.tsv", "formats.csv", "tags.tsv", "extents.tsv", "git.tsv",
                                         NULL };
    char *own_id = disc_root_id(root);
    size_t copied = 0;
    for (size_t i = 0; i < cat.discs.n; i++) {
        const char *id = rec_get(cat.discs.v[i], "Id");
        if (!id) continue;
        for (int k = 0; KINDS[k]; k++) {
            char *from = xprintf("%s/volumes/%s/%s", snap_dir, id, KINDS[k]);
            if (k == 0 && access(from, F_OK) && own_id && !strcmp(id, own_id)) {   /* a disc without catalog/ manifests */
                free(from);
                from = join(root, "manifest-sha256.txt");
            }
            char *to = home_volume_file(&h, id, KINDS[k]);
            if (!access(from, F_OK) && access(to, F_OK)) {
                char *dir = xprintf("%s/volumes/%s", h.catalog_dir, id);
                if (mkdirs(dir)) die("cannot create %s", dir);
                copy_file(from, to);
                copied++;
                free(dir);
            }
            free(from);
            free(to);
        }
    }
    for (int k = 0; HOME_VOCABULARIES[k]; k++) {         /* the vocabularies, when the home has none yet */
        char *from = xprintf("%s/config/%s", snap_dir, HOME_VOCABULARIES[k]), *to = join(h.config_dir, HOME_VOCABULARIES[k]);
        if (!access(from, F_OK) && access(to, F_OK)) {
            if (mkdirs(h.config_dir)) die("cannot create %s", h.config_dir);
            copy_file(from, to);
            printf("Restored config/%s from the disc\n", HOME_VOCABULARIES[k]);
        } else if (!access(from, F_OK)) {
            char *a = read_text(from), *b = read_text(to);
            if (strcmp(a, b))
                printf("Kept the home's config/%s; the disc's differs (%s)\n", HOME_VOCABULARIES[k], from);
            free(a);
            free(b);
        }
        free(from);
        free(to);
    }
    if (mkdirs(h.catalog_dir)) die("cannot create %s", h.catalog_dir);
    archive_save(&cat, h.rec_path);
    sbuf list = { 0 };
    sb_puts(&list, "");
    if (added.n) {
        sb_puts(&list, " (");
        for (size_t i = 0; i < added.n; i++) sb_printf(&list, "%s%s", i ? ", " : "", added.v[i]);
        sb_puts(&list, ")");
    }
    printf("Added %zu disc(s)%s, updated %zu, %zu new event(s), %zu file list(s) copied into %s\n", added.n, list.s,
           updated.n, events, copied, h.path);
    free(list.s);
    return 0;
}
