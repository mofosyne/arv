/* Looking after the archive: which copies are known good, which editions are safe, what can be
 * retired, and what is owed (research/plan.md, collections; docs/workflow.md).
 *
 *   arv todo [--overdue YEARS]   what needs doing: images not burned, copies not read back,
 *                                editions not yet safe, replaced editions, discs kept in one place,
 *                                checks overdue (default: 5 years)
 *   arv objects [NAME] [--json]  what you keep, and where all its copies are: each data object's versions
 *                                and each collection's newest edition, the discs holding them and
 *                                every copy of those discs (form, temperature, read back), and
 *                                whether the original is still where it came from
 *   arv retire CODE [--yes [--accept-loss]] [-v]
 *                                the editions a later safe edition replaces (not those kept): lists the
 *                                files found only on them, and records nothing without --yes; refuses
 *                                while any file is only on them, unless --accept-loss
 *
 * arv never burns and never deletes: a copy is known good when arv burned --device read it back
 * identical to its image; an edition is safe when each of its discs has such a copy. */
#define _XOPEN_SOURCE 700
#include "arv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *get_or(const rec_record *r, const char *name, const char *dflt)
{
    const char *v = rec_get(r, name);
    return v ? v : dflt;
}

static int is_disc_event(const rec_record *e, const char *id, const char *type)
{
    return rec_get(e, "Disc") && !strcmp(rec_get(e, "Disc"), id) && rec_get(e, "Type") && !strcmp(rec_get(e, "Type"), type);
}

/* the date of the newest copy read back identical to its image, or NULL */
const char *disc_read_back(const archive *cat, const char *id)
{
    const char *date = NULL;
    for (size_t i = 0; i < cat->events.n; i++) {
        const rec_record *e = cat->events.v[i];
        if (is_disc_event(e, id, "replication") && !strcmp(get_or(e, "ReadBack", ""), "identical")) date = rec_get(e, "Date");
    }
    return date;
}

static int burned_at_all(const archive *cat, const char *id)
{
    for (size_t i = 0; i < cat->events.n; i++)
        if (is_disc_event(cat->events.v[i], id, "replication")) return 1;
    return 0;
}

/* the newest successful check of the disc itself (not the image test when it was made), or "" */
static const char *last_check(const archive *cat, const char *id)
{
    const char *last = "";
    for (size_t i = 0; i < cat->events.n; i++) {
        const rec_record *e = cat->events.v[i];
        if (!is_disc_event(e, id, "fixity check") || strcmp(get_or(e, "Outcome", ""), "success")) continue;
        if (!strncmp(get_or(e, "Note", ""), "image test after creation", 25)) continue;
        if (strcmp(get_or(e, "Date", ""), last) > 0) last = rec_get(e, "Date");
    }
    return last;
}

static int retired(const archive *cat, const char *id)
{
    const rec_record *d = archive_disc(cat, id);
    return d && rec_get(d, "Retired");
}

static void volumes(const rec_record *r, strlist *out)
{
    for (size_t f = 0; f < r->nfields; f++)
        if (!strcmp(r->fields[f].name, "Volume")) strlist_add(out, r->fields[f].value);
}

int edition_safe(const archive *cat, const rec_record *rev)
{
    strlist v = { 0 };
    volumes(rev, &v);
    int safe = v.n > 0;
    for (size_t i = 0; i < v.n; i++) safe &= disc_read_back(cat, v.v[i]) != NULL;
    strlist_free(&v);
    return safe;
}

static int edition_retired(const archive *cat, const rec_record *rev)
{
    strlist v = { 0 };
    volumes(rev, &v);
    int all = v.n > 0;
    for (size_t i = 0; i < v.n; i++) all &= retired(cat, v.v[i]);
    strlist_free(&v);
    return all;
}

/* a collection's editions, oldest first */
static void editions(const archive *cat, const rec_record *c, recs *out)
{
    for (size_t i = 0; i < cat->revisions.n; i++) {
        const rec_record *r = cat->revisions.v[i];
        if (rec_get(r, "Edition") && !strcmp(get_or(r, "Collection", ""), get_or(c, "Uuid", ""))) recs_add(out, cat->revisions.v[i]);
    }
}

/* the newest safe edition, and the editions before it not kept and not yet retired */
static const rec_record *replaced(const archive *cat, const rec_record *c, recs *out)
{
    recs eds = { 0 };
    editions(cat, c, &eds);
    size_t safe = eds.n;
    while (safe > 0 && !edition_safe(cat, eds.v[safe - 1])) safe--;
    const rec_record *by = safe ? eds.v[safe - 1] : NULL;
    for (size_t i = 0; by && i + 1 < safe; i++)
        if (!rec_get(eds.v[i], "Keep") && !edition_retired(cat, eds.v[i])) recs_add(out, eds.v[i]);
    free(eds.v);
    return by;
}

/* ------------------------------------------------------------------ the copies of what you keep */

/* what is known of the copies of a set of discs (retired ones aside) */
typedef struct {
    size_t copies, cold, good;      /* replications; of them cold; read back identical */
    size_t discs;                   /* discs not retired */
} holding;

static void hold(const archive *cat, const char *id, holding *h)
{
    if (retired(cat, id)) return;
    h->discs++;
    for (size_t i = 0; i < cat->events.n; i++) {
        const rec_record *e = cat->events.v[i];
        if (!is_disc_event(e, id, "replication") || !strcmp(get_or(e, "Outcome", ""), "failure")) continue;
        h->copies++;
        h->cold += !strcmp(get_or(e, "Temperature", ""), "cold");
        h->good += !strcmp(get_or(e, "ReadBack", ""), "identical");
    }
}

/* "disc cold (read back), iso warm", "retired", or "no copy yet" */
static char *copies_text(const archive *cat, const char *id)
{
    if (retired(cat, id)) return xstrdup("retired");
    sbuf b = { 0 };
    sb_puts(&b, "");
    for (size_t i = 0; i < cat->events.n; i++) {
        const rec_record *e = cat->events.v[i];
        if (!is_disc_event(e, id, "replication") || !strcmp(get_or(e, "Outcome", ""), "failure")) continue;
        sb_printf(&b, "%s%s %s%s", b.len ? ", " : "", get_or(e, "Form", "disc"), get_or(e, "Temperature", "?"),
                  !strcmp(get_or(e, "ReadBack", ""), "identical") ? " (read back)" : "");
    }
    if (!b.len) sb_puts(&b, "no copy yet");
    const rec_record *d = archive_disc(cat, id);
    char *where = d ? archive_where(cat, d) : xstrdup("");
    if (*where) sb_printf(&b, "; at %s", where);
    free(where);
    return b.s;
}

static long version_of(const rec_record *o)
{
    return atol(get_or(o, "Version", "0"));
}

/* an object's newest version: the records holding it, and what is known of their copies */
static long newest(const archive *cat, const char *uuid, recs *out, holding *h)
{
    long top = 0;
    for (size_t i = 0; i < cat->objects.n; i++)
        if (!strcmp(get_or(cat->objects.v[i], "Uuid", ""), uuid) && version_of(cat->objects.v[i]) > top)
            top = version_of(cat->objects.v[i]);
    for (size_t i = 0; i < cat->objects.n; i++) {
        const rec_record *o = cat->objects.v[i];
        if (strcmp(get_or(o, "Uuid", ""), uuid) || version_of(o) != top) continue;
        if (out) recs_add(out, cat->objects.v[i]);
        if (h) hold(cat, get_or(o, "Disc", ""), h);
    }
    return top;
}

/* where an object's newest version was read from, as the home knows it (one place, or more for
 * copies); returns how many of them are there now */
static size_t object_sources(const archive *cat, const recs *top, strlist *out)
{
    size_t there = 0;
    for (size_t i = 0; i < top->n; i++) {
        const char *src = rec_get(top->v[i], "Source");
        if (!src || strlist_has(out, src)) continue;
        strlist_add(out, src);
        there += !access(src, F_OK);
    }
    (void)cat;
    return there;
}

static void holding_text(const holding *h, sbuf *b)
{
    sb_printf(b, "%zu cop%s on %zu disc%s, %zu cold, %zu read back", h->copies, h->copies == 1 ? "y" : "ies", h->discs,
              h->discs == 1 ? "" : "s", h->cold, h->good);
}

/* a disc as the union view shows it: its copies (form, temperature, read back) and places */
static void disc_json(const archive *cat, const char *id, sbuf *b)
{
    const rec_record *d = archive_disc(cat, id);
    char *where = d ? archive_where(cat, d) : xstrdup("");
    sb_puts(b, "{\"disc\": ");
    json_str(b, id);
    sb_printf(b, ", \"retired\": %s, \"where\": ", retired(cat, id) ? "true" : "false");
    json_str(b, where);
    sb_puts(b, ", \"copies\": [");
    int first = 1;
    for (size_t i = 0; i < cat->events.n; i++) {
        const rec_record *e = cat->events.v[i];
        if (!is_disc_event(e, id, "replication") || !strcmp(get_or(e, "Outcome", ""), "failure")) continue;
        sb_puts(b, first ? "{\"form\": " : ", {\"form\": ");
        json_str(b, get_or(e, "Form", "disc"));
        sb_puts(b, ", \"temperature\": ");
        json_str(b, get_or(e, "Temperature", ""));
        sb_printf(b, ", \"readBack\": %s}", !strcmp(get_or(e, "ReadBack", ""), "identical") ? "true" : "false");
        first = 0;
    }
    sb_puts(b, "]}");
    free(where);
}

static void holding_json(const holding *h, sbuf *b)
{
    sb_printf(b, "{\"copies\": %zu, \"cold\": %zu, \"readBack\": %zu, \"discs\": %zu}", h->copies, h->cold, h->good, h->discs);
}

/* arv objects --json: the same as the text, for arv gui's Objects tab */
static void objects_json(const archive *cat)
{
    sbuf b = { 0 };
    strlist seen = { 0 };
    sb_puts(&b, "{\"objects\": [");
    for (size_t i = 0; i < cat->objects.n; i++) {
        const char *uuid = get_or(cat->objects.v[i], "Uuid", "");
        if (strlist_has(&seen, uuid)) continue;
        strlist_add(&seen, uuid);
        recs top = { 0 };
        holding hd = { 0 };
        long last = newest(cat, uuid, &top, &hd);
        const rec_record *o = top.v[0];
        strlist src = { 0 };
        size_t there = object_sources(cat, &top, &src);
        sb_puts(&b, seen.n > 1 ? ", {\"uuid\": " : "{\"uuid\": ");
        json_str(&b, uuid);
        sb_puts(&b, ", \"name\": ");
        json_str(&b, get_or(o, "Name", "?"));
        sb_puts(&b, ", \"kind\": ");
        json_str(&b, get_or(o, "Kind", "?"));
        sb_printf(&b, ", \"versions\": %ld, \"newest\": ", last);
        holding_json(&hd, &b);
        sb_puts(&b, ", \"sources\": [");
        for (size_t k = 0; k < src.n; k++) {
            sb_puts(&b, k ? ", {\"path\": " : "{\"path\": ");
            json_str(&b, src.v[k]);
            sb_printf(&b, ", \"there\": %s}", access(src.v[k], F_OK) ? "false" : "true");
        }
        sb_printf(&b, "], \"todo\": [%s%s%s], \"held\": [",
                  hd.discs && hd.copies && !hd.cold ? "\"no cold copy\"" : "",
                  hd.discs && hd.copies && !hd.cold && hd.discs && hd.copies < 2 && !there ? ", " : "",
                  hd.discs && hd.copies < 2 && !there ? "\"no longer where it came from, fewer than two copies\"" : "");
        int first = 1;
        for (long v = last; v >= 1; v--)
            for (size_t k = 0; k < cat->objects.n; k++) {
                const rec_record *x = cat->objects.v[k];
                if (strcmp(get_or(x, "Uuid", ""), uuid) || version_of(x) != v) continue;
                sb_printf(&b, "%s{\"version\": %ld, \"path\": ", first ? "" : ", ", v);
                json_str(&b, get_or(x, "Path", ""));
                sb_puts(&b, ", \"files\": ");
                sb_puts(&b, get_or(x, "Files", "0"));
                sb_puts(&b, ", \"bytes\": ");
                sb_puts(&b, get_or(x, "Bytes", "0"));
                sb_puts(&b, ", \"on\": ");
                disc_json(cat, get_or(x, "Disc", ""), &b);
                sb_puts(&b, "}");
                first = 0;
            }
        sb_puts(&b, "]}");
        strlist_free(&src);
        free(top.v);
    }
    sb_puts(&b, "], \"collections\": [");
    for (size_t i = 0; i < cat->collections.n; i++) {
        const rec_record *c = cat->collections.v[i];
        recs eds = { 0 };
        editions(cat, c, &eds);
        sb_puts(&b, i ? ", {\"code\": " : "{\"code\": ");
        json_str(&b, get_or(c, "Code", ""));
        sb_puts(&b, ", \"title\": ");
        json_str(&b, get_or(c, "Title", ""));
        sb_printf(&b, ", \"editions\": %zu, \"folder\": ", eds.n);
        const char *folder = collection_folder(cat, c);
        if (folder) {
            sb_puts(&b, "{\"path\": ");
            json_str(&b, folder);
            sb_printf(&b, ", \"there\": %s}", access(folder, F_OK) ? "false" : "true");
        } else {
            sb_puts(&b, "null");
        }
        sb_puts(&b, ", \"newest\": ");
        if (!eds.n) sb_puts(&b, "null");
        else {
            const rec_record *e = eds.v[eds.n - 1];
            strlist v = { 0 };
            volumes(e, &v);
            holding hd = { 0 };
            for (size_t k = 0; k < v.n; k++) hold(cat, v.v[k], &hd);
            sb_printf(&b, "{\"edition\": %s, \"holding\": ", get_or(e, "Edition", "0"));
            holding_json(&hd, &b);
            sb_puts(&b, ", \"discs\": [");
            for (size_t k = 0; k < v.n; k++) {
                if (k) sb_puts(&b, ", ");
                disc_json(cat, v.v[k], &b);
            }
            sb_puts(&b, "]}");
            strlist_free(&v);
        }
        sb_puts(&b, "}");
        free(eds.v);
    }
    sb_puts(&b, "]}\n");
    fputs(b.s, stdout);
    free(b.s);
    strlist_free(&seen);
}

int cmd_objects(int argc, char **argv)
{
    const char *given = NULL, *want = NULL;
    int as_json = 0;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!strcmp(argv[i], "--json")) as_json = 1;
        else if (argv[i][0] != '-' && !want) want = argv[i];
        else return 2;
    }
    arv_home h;
    home_find(&h, given, NULL);
    archive cat;
    archive_load(&cat, h.rec_path);
    if (as_json) {
        if (want) return 2;
        objects_json(&cat);
        return 0;
    }
    size_t shown = 0;
    strlist seen = { 0 };
    for (size_t i = 0; i < cat.objects.n; i++) {          /* data objects, each lineage once */
        const char *uuid = get_or(cat.objects.v[i], "Uuid", "");
        if (strlist_has(&seen, uuid)) continue;
        strlist_add(&seen, uuid);
        recs top = { 0 };
        holding hd = { 0 };
        long last = newest(&cat, uuid, &top, &hd);
        const rec_record *o = top.v[0];
        const char *name = get_or(o, "Name", "?");
        int file = !strcmp(get_or(o, "Kind", ""), "file");
        if (want && strcmp(want, name) && strcmp(want, uuid)) {
            free(top.v);
            continue;
        }
        if (!shown++) puts("Data objects (disc plans):");
        sbuf line = { 0 };
        holding_text(&hd, &line);
        printf("  %s%s  %s, %ld version%s; newest: %s\n", name, file ? "" : "/", get_or(o, "Kind", "?"), last, last == 1 ? "" : "s",
               line.s);
        free(line.s);
        strlist src = { 0 };
        object_sources(&cat, &top, &src);
        for (size_t k = 0; k < src.n; k++)
            printf("    %s %s (%s)\n", k ? "and" : "from", src.v[k], access(src.v[k], F_OK) ? "not there now" : "there now: arv status says if it changed");
        strlist_free(&src);
        for (long v = last; v >= 1; v--)
            for (size_t k = 0; k < cat.objects.n; k++) {
                const rec_record *x = cat.objects.v[k];
                if (strcmp(get_or(x, "Uuid", ""), uuid) || version_of(x) != v) continue;
                char *c = copies_text(&cat, get_or(x, "Disc", "?"));
                const char *path = get_or(x, "Path", "?");
                printf("    version %ld  %s  data/%s%s  %s\n", v, get_or(x, "Disc", "?"), strcmp(path, ".") ? path : "",
                       file || !strcmp(path, ".") ? "" : "/", c);
                free(c);
            }
        free(top.v);
    }
    strlist_free(&seen);
    int heading_shown = 0;
    for (size_t i = 0; i < cat.collections.n; i++) {       /* collections: the newest edition's discs */
        const rec_record *c = cat.collections.v[i];
        if (want && strcmp(want, get_or(c, "Code", "")) && strcmp(want, get_or(c, "Uuid", ""))) continue;
        recs eds = { 0 };
        editions(&cat, c, &eds);
        if (!heading_shown++) puts("Collections:");
        shown++;
        const char *folder = collection_folder(&cat, c);
        if (!eds.n) {
            printf("  %s  \"%s\": no edition yet\n", get_or(c, "Code", ""), get_or(c, "Title", ""));
        } else {
            const rec_record *e = eds.v[eds.n - 1];
            strlist v = { 0 };
            volumes(e, &v);
            holding hd = { 0 };
            for (size_t k = 0; k < v.n; k++) hold(&cat, v.v[k], &hd);
            sbuf line = { 0 };
            holding_text(&hd, &line);
            printf("  %s  \"%s\": %zu edition%s; newest: %s/%s, %s\n", get_or(c, "Code", ""), get_or(c, "Title", ""), eds.n,
                   eds.n == 1 ? "" : "s", get_or(c, "Code", ""), get_or(e, "Edition", ""), line.s);
            free(line.s);
            for (size_t k = 0; k < v.n; k++) {
                char *t = copies_text(&cat, v.v[k]);
                printf("    %s  %s\n", v.v[k], t);
                free(t);
            }
            strlist_free(&v);
        }
        if (folder) printf("    workflow folder %s (%s)\n", folder, access(folder, F_OK) ? "not there now" : "there now");
        free(eds.v);
    }
    if (!shown) {
        if (want) die("nothing kept is called %s (arv objects lists them)", want);
        puts("Nothing kept as a data object or collection yet (arv plan, arv collection init).");
    }
    return 0;
}

/* ------------------------------------------------------------------ arv todo */

static void heading(int *shown, const char *text)
{
    if (!*shown) printf("%s\n", text);
    *shown = 1;
}

int cmd_todo(int argc, char **argv)
{
    const char *given = NULL;
    int years = 5;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--overdue")) years = atoi(argv[++i]);
        else return 2;
    }
    arv_home h;
    home_find(&h, given, NULL);
    archive cat;
    archive_load(&cat, h.rec_path);
    struct tm tm;
    today_tm(&tm);
    char cutoff[40];
    snprintf(cutoff, sizeof cutoff, "%04d-%02d-%02d", tm.tm_year + 1900 - years, tm.tm_mon + 1, tm.tm_mday);
    size_t items = 0;
    int s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0;
    for (size_t i = 0; i < cat.discs.n; i++) {                  /* images made, never burned */
        const char *id = get_or(cat.discs.v[i], "Id", "");
        if (retired(&cat, id) || burned_at_all(&cat, id)) continue;
        heading(&s1, "No copy yet (burn the image and arv burned ID --device DRIVE, or keep it: arv stored ID PATH):");
        printf("  %s  made %s\n", id, get_or(cat.discs.v[i], "Date", ""));
        items++;
    }
    for (size_t i = 0; i < cat.discs.n; i++) {                  /* burned, no copy known good */
        const char *id = get_or(cat.discs.v[i], "Id", "");
        if (retired(&cat, id) || !burned_at_all(&cat, id) || disc_read_back(&cat, id) || *last_check(&cat, id)) continue;
        heading(&s2, "Burned, never read back (arv check --device DRIVE ID):");
        printf("  %s\n", id);
        items++;
    }
    for (size_t k = 0; k < cat.collections.n; k++) {            /* editions: not safe yet; replaced */
        const rec_record *c = cat.collections.v[k];
        recs eds = { 0 }, old = { 0 };
        editions(&cat, c, &eds);
        if (eds.n && !edition_safe(&cat, eds.v[eds.n - 1]) && !edition_retired(&cat, eds.v[eds.n - 1])) {
            heading(&s3, "Editions not safe yet (every disc burned and read back):");
            printf("  %s/%s\n", get_or(c, "Code", ""), get_or(eds.v[eds.n - 1], "Edition", ""));
            items++;
        }
        const rec_record *by = replaced(&cat, c, &old);
        if (old.n) {
            heading(&s4, "Replaced by a safe edition, ready to retire (arv retire CODE):");
            printf("  %s: edition", get_or(c, "Code", ""));
            for (size_t i = 0; i < old.n; i++) printf("%s %s", i ? "," : "", get_or(old.v[i], "Edition", ""));
            printf(" (replaced by edition %s)\n", get_or(by, "Edition", ""));
            items++;
        }
        free(eds.v);
        free(old.v);
    }
    int s7 = 0;
    for (size_t i = 0; i < cat.discs.n; i++) {                  /* no cold copy */
        const char *id = get_or(cat.discs.v[i], "Id", "");
        if (retired(&cat, id) || !burned_at_all(&cat, id)) continue;
        int cold = 0;
        for (size_t k = 0; k < cat.events.n; k++)
            cold |= is_disc_event(cat.events.v[k], id, "replication") && !strcmp(get_or(cat.events.v[k], "Temperature", ""), "cold");
        if (cold) continue;
        heading(&s7, "No cold copy, every copy is online or in use (burn one for the shelf):");
        printf("  %s\n", id);
        items++;
    }
    int s6 = 0;
    for (size_t i = 0; i < cat.discs.n; i++) {                  /* kept in one place */
        const rec_record *d = cat.discs.v[i];
        const char *id = get_or(d, "Id", "");
        if (retired(&cat, id) || !burned_at_all(&cat, id)) continue;
        int places = 0;
        for (size_t f = 0; f < d->nfields; f++) places += !strcmp(d->fields[f].name, "Location");
        if (places >= 2) continue;
        heading(&s5, "Kept in fewer than two places (arv locate ID PLACE --add):");
        printf("  %s  %s\n", id, places ? "one place" : "no place recorded");
        items++;
    }
    for (size_t i = 0; i < cat.discs.n; i++) {                  /* checks overdue */
        const char *id = get_or(cat.discs.v[i], "Id", "");
        const char *last = last_check(&cat, id);
        if (retired(&cat, id) || !*last || strcmp(last, cutoff) >= 0) continue;
        char head[96];
        snprintf(head, sizeof head, "Not checked in %d years (arv check --device DRIVE ID):", years);
        heading(&s6, head);
        printf("  %s  last %s\n", id, last);
        items++;
    }
    int s8 = 0, s9 = 0;
    strlist seen = { 0 };
    for (size_t i = 0; i < cat.objects.n; i++) {               /* data objects: their newest version */
        const char *uuid = get_or(cat.objects.v[i], "Uuid", "");
        if (strlist_has(&seen, uuid)) continue;
        strlist_add(&seen, uuid);
        recs top = { 0 };
        holding hd = { 0 };
        long v = newest(&cat, uuid, &top, &hd);
        const rec_record *o = top.v[0];
        strlist src = { 0 };
        size_t there = object_sources(&cat, &top, &src);
        int file = !strcmp(get_or(o, "Kind", ""), "file");
        if (hd.discs && hd.copies && !hd.cold) {
            heading(&s8, "Data objects with no cold copy of their newest version (burn one for the shelf):");
            printf("  %s%s  version %ld\n", get_or(o, "Name", "?"), file ? "" : "/", v);
            items++;
        }
        if (hd.discs && hd.copies < 2 && !there) {
            heading(&s9, "Data objects no longer where they came from, with fewer than two copies of their newest version:");
            printf("  %s%s  version %ld: %zu cop%s%s%s\n", get_or(o, "Name", "?"), file ? "" : "/", v, hd.copies,
                   hd.copies == 1 ? "y" : "ies", src.n ? "; was at " : "", src.n ? src.v[0] : "");
            items++;
        }
        strlist_free(&src);
        free(top.v);
    }
    strlist_free(&seen);
    if (!items) printf("Nothing owed: every disc copied, read back, with a cold copy, kept in two places and checked within %d years.\n", years);
    return 0;
}

/* ------------------------------------------------------------------ arv retire */

int cmd_retire(int argc, char **argv)
{
    const char *given = NULL, *code = NULL;
    int yes = 0, accept_loss = 0, verbose = 0;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!strcmp(argv[i], "--yes")) yes = 1;
        else if (!strcmp(argv[i], "--accept-loss")) accept_loss = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (argv[i][0] != '-' && !code) code = argv[i];
        else return 2;
    }
    if (!code) return 2;
    arv_home h;
    home_find(&h, given, NULL);
    archive cat;
    archive_load(&cat, h.rec_path);
    const rec_record *c = archive_collection(&cat, code);
    if (!c) die("no collection %s", code);
    recs old = { 0 };
    const rec_record *by = replaced(&cat, c, &old);
    if (!by) {
        printf("%s: no edition is safe yet (each of its discs burned and read back: arv burned --device); nothing to retire\n",
               get_or(c, "Code", ""));
        return 0;
    }
    if (!old.n) {
        printf("%s: nothing to retire (edition %s is safe; every earlier edition is kept or retired)\n", get_or(c, "Code", ""),
               get_or(by, "Edition", ""));
        return 0;
    }
    /* files on the discs being retired that no kept edition holds (by content) */
    strlist keep_hashes = { 0 }, at_risk = { 0 };
    strlist *only = xmalloc(old.n * sizeof *only);  /* per edition: its files on no disc that stays */
    memset(only, 0, old.n * sizeof *only);
    recs eds = { 0 };
    editions(&cat, c, &eds);
    for (size_t i = 0; i < eds.n; i++) {
        if (recs_has(&old, eds.v[i]) || edition_retired(&cat, eds.v[i])) continue;
        char *p = revision_manifest_path(&h, get_or(eds.v[i], "Node", "")), *t = access(p, F_OK) ? NULL : read_text(p);
        for (char *l = t; l && *l;) {
            char *nl = strchr(l, '\n');
            if (nl) *nl = 0;
            if (strlen(l) > 66) {
                l[64] = 0;
                strlist_add(&keep_hashes, l);
            }
            l = nl ? nl + 1 : l + strlen(l);
        }
        free(t);
        free(p);
    }
    for (size_t i = 0; i < old.n; i++) {
        char *p = revision_manifest_path(&h, get_or(old.v[i], "Node", "")), *t = access(p, F_OK) ? NULL : read_text(p);
        if (!t) die("the manifest of edition %s is not in the home catalogue: cannot tell what retiring it would lose",
                    get_or(old.v[i], "Edition", ""));
        for (char *l = t; *l;) {
            char *nl = strchr(l, '\n');
            if (nl) *nl = 0;
            if (strlen(l) > 66) {
                char hash[65];
                memcpy(hash, l, 64);
                hash[64] = 0;
                if (!strlist_has(&keep_hashes, hash)) {
                    strlist_add(&only[i], l);
                    if (!strlist_has(&at_risk, l + 66)) strlist_add(&at_risk, l + 66);
                }
            }
            l = nl ? nl + 1 : l + strlen(l);
        }
        free(t);
        free(p);
    }
    printf("%s: edition %s (%s) is safe; it replaces (editions marked kept stay):\n", get_or(c, "Code", ""),
           get_or(by, "Edition", ""), get_or(by, "Date", ""));
    for (size_t i = 0; i < old.n; i++) {
        strlist v = { 0 };
        volumes(old.v[i], &v);
        printf("  edition %s (%s):", get_or(old.v[i], "Edition", ""), get_or(old.v[i], "Date", ""));
        for (size_t k = 0; k < v.n; k++) printf(" %s", v.v[k]);
        if (only[i].n) printf("  (%zu file%s on no disc that stays)", only[i].n, only[i].n == 1 ? "" : "s");
        putchar('\n');
        strlist_free(&v);
    }
    if (at_risk.n) {
        printf("%zu file%s only on these discs (not in the newer edition):\n", at_risk.n,
               at_risk.n == 1 ? " is" : "s are");
        size_t limit = verbose ? at_risk.n : at_risk.n < 20 ? at_risk.n : 20;
        for (size_t i = 0; i < limit; i++) printf("  %s\n", at_risk.v[i]);
        if (limit < at_risk.n) printf("  ... %zu more (-v for all)\n", at_risk.n - limit);
    }
    if (at_risk.n && !accept_loss) {
        /* these discs hold the only copy: refuse, unless the person says the loss is accepted */
        printf("%s: these discs hold the only copy of %s. Keep the edition%s holding them (arv collection keep "
               "%s %s), or arv retire %s --yes --accept-loss to retire them anyway.\n",
               yes ? "Refused" : "Nothing recorded", at_risk.n == 1 ? "that file" : "those files",
               old.n == 1 ? "" : "s", get_or(c, "Code", ""), old.n == 1 ? get_or(old.v[0], "Edition", "N") : "N",
               get_or(c, "Code", ""));
        return yes ? 1 : 0;
    }
    if (!yes) {
        printf("Nothing recorded. arv retire %s --yes%s records them as retired%s (arv deletes nothing: the discs are "
               "yours to keep or destroy).\n", get_or(c, "Code", ""), at_risk.n ? " --accept-loss" : "",
               at_risk.n ? ", and those files as lost" : "");
        return 0;
    }
    for (size_t i = 0; i < old.n; i++)                  /* the loss, on the edition: "Lost: SHA256  PATH" */
        for (size_t k = 0; k < only[i].n; k++) rec_add(old.v[i], "Lost", only[i].v[k]);
    char today[11], *who = person();
    today_iso(today);
    size_t n = 0;
    for (size_t i = 0; i < old.n; i++) {
        strlist v = { 0 };
        volumes(old.v[i], &v);
        for (size_t k = 0; k < v.n; k++) {
            rec_record *d = archive_disc(&cat, v.v[k]);
            if (!d || rec_get(d, "Retired")) continue;
            char *where = archive_where(&cat, d);
            size_t kept = 0;                    /* it leaves every location */
            for (size_t f = 0; f < d->nfields; f++) {
                if (!strcmp(d->fields[f].name, "Location")) {
                    free(d->fields[f].name);
                    free(d->fields[f].value);
                } else {
                    d->fields[kept++] = d->fields[f];
                }
            }
            d->nfields = kept;
            rec_add(d, "Retired", today);
            char *note = xprintf("retired: edition %s of %s, replaced by edition %s (revision %.12s)%s%s%s",
                                 get_or(old.v[i], "Edition", ""), get_or(c, "Code", ""), get_or(by, "Edition", ""),
                                 get_or(by, "Node", ""), *where ? "; was kept at " : "", where,
                                 at_risk.n ? "; files only on retired discs recorded as lost (Lost:)" : "");
            recs_add(&cat.events, new_event(v.v[k], "deaccession", "success", who, "human", note));
            free(note);
            free(where);
            n++;
        }
        strlist_free(&v);
    }
    archive_save(&cat, h.rec_path);
    printf("%zu disc%s retired (logged)\n", n, n == 1 ? "" : "s");
    if (at_risk.n) printf("%zu file%s recorded as lost (arv log %s, arv find)\n", at_risk.n, at_risk.n == 1 ? "" : "s",
                          get_or(c, "Code", ""));
    for (size_t i = 0; i < old.n; i++) strlist_free(&only[i]);
    free(only);
    free(who);
    return 0;
}
