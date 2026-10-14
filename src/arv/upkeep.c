/* Looking after the archive: which copies are known good, which editions are safe, what can be
 * retired, and what is owed (research/plan.md, collections; docs/workflow.md).
 *
 *   arv todo [--overdue AGE]   what needs doing: images not burned, copies not read back,
 *                                editions not yet safe, replaced editions, discs kept in one place
 *                                or below their appraisal (essential: 3 copies in 3 places; important:
 *                                2 in 2; useful: 1), checks overdue (default: 5 years)
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

/* the date of the newest copy known good (read back as it was recorded, or checked since by its letter), or NULL */
const char *disc_read_back(const archive *cat, const char *id)
{
    const char *date = NULL;
    for (size_t i = 0; i < cat->events.n; i++) {
        const rec_record *e = cat->events.v[i];
        if (is_copy_event(e, id) && copy_read_back(cat, id, e)) date = rec_get(e, "Date");
    }
    return date;
}

static int burned_at_all(const archive *cat, const char *id)
{
    for (size_t i = 0; i < cat->events.n; i++)
        if (is_copy_event(cat->events.v[i], id)) return 1;
    return 0;
}

/* the newest successful check of the disc itself (not the image test when it was made), or "" */
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
        if (!is_copy_event(e, id) || !strcmp(get_or(e, "Outcome", ""), "failure")
            || copy_same_as(cat, id, e) || copy_shares_carrier(cat, id, e))   /* one disc twice, one tape: once */
            continue;
        h->copies++;
        h->cold += !strcmp(get_or(e, "Temperature", ""), "cold");
        h->good += copy_read_back(cat, id, e);
    }
}

/* "disc cold (read back), iso warm", "retired", or "no copy yet" */
/* where a copy is kept, readable (the place's name), or "" */
static char *copy_place(const archive *cat, const rec_record *e)
{
    const char *disc = rec_get(e, "Disc"), *loc = copy_location(cat, disc, e);
    if (!loc) return xstrdup("");
    rec_record probe = { 0 };
    probe.type = "Disc";
    rec_add(&probe, "Location", loc);
    char *at = archive_where(cat, &probe);
    rec_clear(&probe);
    return at;
}

static char *copies_text(const archive *cat, const char *id)
{
    if (retired(cat, id)) return xstrdup("retired");
    int placed = 0;                 /* some copy says where it is kept */
    for (size_t i = 0; i < cat->events.n; i++) {
        const rec_record *e = cat->events.v[i];
        if (is_copy_event(e, id) && strcmp(get_or(e, "Outcome", "failure"), "failure"))
            placed |= copy_location(cat, id, e) != NULL;
    }
    sbuf b = { 0 };
    sb_puts(&b, "");

    for (size_t i = 0; i < cat->events.n; i++) {
        const rec_record *e = cat->events.v[i];
        if (!is_copy_event(e, id) || !strcmp(get_or(e, "Outcome", ""), "failure")) continue;
        sb_printf(&b, "%s%s%s%s %s%s", b.len ? ", " : "", copy_letter(cat, id, e) ? copy_letter(cat, id, e) : "",
                  copy_letter(cat, id, e) ? ": " : "", get_or(e, "Form", "disc"), get_or(e, "Temperature", "?"),
                  copy_read_back(cat, id, e) ? " (read back)" : "");
        const char *loc = copy_location(cat, id, e);
        if (loc) {
            char *at = copy_place(cat, e);
            sb_printf(&b, " at %s", at);
            free(at);

        } else if (placed) {
            sb_puts(&b, " (its place not recorded)");
        }
    }
    if (!b.len) sb_puts(&b, "no copy yet");

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
static size_t object_sources(const char *root, const recs *top, strlist *out)
{
    size_t there = 0;
    for (size_t i = 0; i < top->n; i++) {
        const char *stored = rec_get(top->v[i], "Source");
        if (!stored) continue;
        char *src = path_abs(root, stored);     /* kept relative to the home's root when inside it */
        if (!strlist_has(out, src)) {
            strlist_add(out, src);
            there += !access(src, F_OK);
        }
        free(src);
    }
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
        if (!is_copy_event(e, id) || !strcmp(get_or(e, "Outcome", ""), "failure")) continue;
        sb_puts(b, first ? "{\"copy\": " : ", {\"copy\": ");
        json_str(b, copy_letter(cat, id, e) ? copy_letter(cat, id, e) : "");
        sb_puts(b, ", \"form\": ");
        json_str(b, get_or(e, "Form", "disc"));
        sb_puts(b, ", \"temperature\": ");
        json_str(b, get_or(e, "Temperature", ""));
        sb_puts(b, ", \"location\": ");
        json_str(b, copy_location(cat, id, e) ? copy_location(cat, id, e) : "");
        char *at = copy_place(cat, e);
        sb_puts(b, ", \"at\": ");
        json_str(b, at);
        free(at);
        sb_puts(b, ", \"made\": ");
        json_str(b, get_or(e, "Date", ""));
        const char *checked = copy_last_check(cat, id, e);
        const rec_record *same = copy_same_as(cat, id, e);
        sb_puts(b, ", \"checked\": ");
        json_str(b, checked ? checked : "");
        sb_puts(b, ", \"sameAs\": ");                    /* the same disc recorded before, as this copy */
        json_str(b, same ? letter_of(cat, same) : "");
        const char *carrier = copy_carrier(cat, id, e);
        const rec_record *with = copy_shares_carrier(cat, id, e);
        sb_puts(b, ", \"carrier\": ");
        json_str(b, carrier ? carrier : "");
        sb_puts(b, ", \"carrierWith\": ");               /* an earlier copy on the same carrier */
        json_str(b, with ? letter_of(cat, with) : "");
        strlist ids;
        copy_identifiers(cat, id, e, &ids);
        sb_puts(b, ", \"identifiers\": [");
        for (size_t k = 0; k < ids.n; k++) {
            if (k) sb_puts(b, ", ");
            json_str(b, ids.v[k]);
        }
        sb_puts(b, "]");
        strlist_free(&ids);
        sb_printf(b, ", \"readBack\": %s}", copy_read_back(cat, id, e) ? "true" : "false");
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
static void objects_json(const archive *cat, const char *root)
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
        size_t there = object_sources(root, &top, &src);
        sb_puts(&b, seen.n > 1 ? ", {\"uuid\": " : "{\"uuid\": ");
        json_str(&b, uuid);
        sb_puts(&b, ", \"name\": ");
        json_str(&b, get_or(o, "Name", "?"));
        sb_puts(&b, ", \"kind\": ");
        json_str(&b, get_or(o, "Kind", "?"));
        static const char *const keys[] = { "title", "description", "coverage", "rights", "origin" };
        for (int k = 0; ABOUT_FIELDS[k]; k++) {        /* what it says about itself: "" when not known */
            sb_printf(&b, ", \"%s\": ", keys[k]);
            json_str(&b, get_or(o, ABOUT_FIELDS[k], ""));
        }
        sb_puts(&b, ", \"notes\": [");
        for (size_t f = 0, n = 0; f < o->nfields; f++)
            if (!strcmp(o->fields[f].name, "Note")) {
                if (n++) sb_puts(&b, ", ");
                json_str(&b, o->fields[f].value);
            }
        sb_puts(&b, "]");
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
                sb_puts(&b, ", \"arranged\": ");     /* its path as arranged, before a layout placed it */
                if (rec_get(x, "Arranged")) json_str(&b, rec_get(x, "Arranged"));
                else sb_puts(&b, "null");
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

/* arv objects DISC-ID: that disc's copies, each with its letter, form, place, and checks */
static int disc_copies_shown(const archive *cat, const char *id, int as_json)
{
    if (as_json) {
        sbuf b = { 0 };
        disc_json(cat, id, &b);
        puts(b.s);
        free(b.s);
        return 0;
    }
    size_t n = disc_copies(cat, id);    /* physical copies: a disc recorded twice counts once */
    if (retired(cat, id)) printf("%s: retired\n", id);
    else if (!n) printf("%s: no copy yet (arv burned, arv stored)\n", id);
    else printf("%s: %zu cop%s\n", id, n, n == 1 ? "y" : "ies");
    for (size_t i = 0; i < cat->events.n; i++) {
        const rec_record *e = cat->events.v[i];
        if (!is_copy_event(e, id) || !strcmp(get_or(e, "Outcome", ""), "failure")) continue;
        char *at = copy_place(cat, e);
        const char *checked = copy_last_check(cat, id, e);
        const rec_record *same = copy_same_as(cat, id, e);
        const char *temp = rec_get(e, "Temperature");
        printf("  %-2s %s%s%s, made %s; %s", letter_of(cat, e), get_or(e, "Form", "disc"), temp ? ", " : "", temp ? temp : "",
               get_or(e, "Date", "?"), *at ? at : "its place not recorded");
        if (checked) printf("; last checked %s", checked);
        else printf("; %s", copy_read_back(cat, id, e) ? "read back" : "not read back");
        strlist ids;
        copy_identifiers(cat, id, e, &ids);
        for (size_t k = 0; k < ids.n; k++) printf("%s%s", k ? ", " : "; ", ids.v[k]);
        strlist_free(&ids);
        if (same) printf("; the same disc as copy %s (recorded twice, counted once)", letter_of(cat, same));
        const char *carrier = copy_carrier(cat, id, e);
        const rec_record *with = copy_shares_carrier(cat, id, e);
        if (carrier) printf("; on %s", carrier);
        if (with) printf(" with copy %s (one carrier: counted once)", letter_of(cat, with));
        putchar('\n');
        free(at);
    }
    return 0;
}

/* what an object's newest version says about it: "Title": description (coverage; rights; origin),
 * and its notes when asked about it alone */
static void about_text(const rec_record *o, int notes)
{
    const char *t = rec_get(o, "Title"), *d = rec_get(o, "Description");
    if (t || d) printf("    %s%s%s%s%s\n", t ? "\"" : "", t ? t : "", t ? "\"" : "", t && d ? ": " : "", d ? d : "");
    sbuf facts = { 0 };
    for (int k = 2; ABOUT_FIELDS[k]; k++)
        if (rec_get(o, ABOUT_FIELDS[k])) sb_printf(&facts, "%s%s", facts.len ? "; " : "", rec_get(o, ABOUT_FIELDS[k]));
    if (facts.len) printf("    %s\n", facts.s);
    free(facts.s);
    for (size_t f = 0; notes && f < o->nfields; f++)
        if (!strcmp(o->fields[f].name, "Note")) printf("    note: %s\n", o->fields[f].value);
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
    if (want && archive_disc(&cat, want)) return disc_copies_shown(&cat, want, as_json);
    if (as_json) {
        if (want) return 2;
        char *root = home_root(&h);
        objects_json(&cat, root);
        free(root);
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
        if (!shown++) puts("Data objects:");
        sbuf line = { 0 };
        holding_text(&hd, &line);
        printf("  %s%s  %s, %ld version%s; newest: %s\n", name, file ? "" : "/", get_or(o, "Kind", "?"), last, last == 1 ? "" : "s",
               line.s);
        free(line.s);
        about_text(o, want != NULL);
        strlist src = { 0 };
        char *root = home_root(&h);
        object_sources(root, &top, &src);
        free(root);
        for (size_t k = 0; k < src.n; k++)
            printf("    %s %s (%s)\n", k ? "and" : "from", src.v[k], access(src.v[k], F_OK) ? "not there now" : "there now: arv status says if it changed");
        strlist_free(&src);
        for (long v = last; v >= 1; v--)
            for (size_t k = 0; k < cat.objects.n; k++) {
                const rec_record *x = cat.objects.v[k];
                if (strcmp(get_or(x, "Uuid", ""), uuid) || version_of(x) != v) continue;
                char *c = copies_text(&cat, get_or(x, "Disc", "?"));
                strlist from, to;               /* as it is called now (moves in the record) */
                disc_moves(&cat, get_or(x, "Disc", ""), &from, &to);
                char *path = moved_path(&from, &to, get_or(x, "Path", "?"));
                printf("    version %ld  %s  data/%s%s  %s", v, get_or(x, "Disc", "?"), strcmp(path, ".") ? path : "",
                       file || !strcmp(path, ".") ? "" : "/", c);
                if (strcmp(path, get_or(x, "Path", "?"))) printf("  (burned as data/%s)", get_or(x, "Path", "?"));
                putchar('\n');
                free(path);
                strlist_free(&from);
                strlist_free(&to);
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


static void tsv_field(const char *s)
{
    for (; s && *s; s++) putchar(*s == '\t' || *s == '\n' ? ' ' : *s);
}

/* arv todo --tsv: what is owed, one copy a line, for a script (a tape library or jukebox loop):
 * what (no-copy, read-back, overdue, missing, one-carrier), disc, copy, form, identifiers,
 * carrier, location, path, last check */
static int todo_tsv(const archive *cat, const char *cutoff)
{
    puts("# arv todo 1\twhat\tdisc\tcopy\tform\tidentifiers\tcarrier\tlocation\tpath\tlast checked");
    for (size_t i = 0; i < cat->discs.n; i++) {
        const char *id = get_or(cat->discs.v[i], "Id", "");
        if (retired(cat, id)) continue;
        if (!burned_at_all(cat, id)) {
            printf("no-copy\t%s\t\t\t\t\t\t\t\n", id);
            continue;
        }
        for (size_t k = 0; k < cat->events.n; k++) {
            const rec_record *e = cat->events.v[k];
            if (!is_copy_event(e, id) || !strcmp(get_or(e, "Outcome", ""), "failure") || copy_same_as(cat, id, e))
                continue;
            const char *checked = copy_last_check(cat, id, e), *path = copy_path(cat, id, e);
            const char *last = checked ? checked : copy_read_back(cat, id, e) ? get_or(e, "Date", "") : "";
            const char *what[4];
            int n = 0;
            if (!copy_read_back(cat, id, e)) what[n++] = "read-back";
            else if (strcmp(last, cutoff) < 0) what[n++] = "overdue";
            if (path && access(path, F_OK)) what[n++] = "missing";
            if (copy_shares_carrier(cat, id, e)) what[n++] = "one-carrier";
            for (int w = 0; w < n; w++) {
                strlist ids;
                copy_identifiers(cat, id, e, &ids);
                printf("%s\t%s\t%s\t%s\t", what[w], id, copy_letter(cat, id, e) ? copy_letter(cat, id, e) : "", get_or(e, "Form", "disc"));
                for (size_t j = 0; j < ids.n; j++) {
                    if (j) putchar(',');
                    tsv_field(ids.v[j]);
                }
                putchar('\t');
                tsv_field(copy_carrier(cat, id, e));
                putchar('\t');
                tsv_field(copy_location(cat, id, e));
                putchar('\t');
                tsv_field(path);
                printf("\t%s\n", last);
                strlist_free(&ids);
            }
        }
    }
    return 0;
}

int cmd_todo(int argc, char **argv)
{
    const char *given = NULL, *overdue = "5y";
    int tsv = 0;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--overdue")) overdue = argv[++i];
        else if (!strcmp(argv[i], "--tsv")) tsv = 1;
        else return 2;
    }
    arv_home h;
    home_find(&h, given, NULL);
    archive cat;
    archive_load(&cat, h.rec_path);
    char *cutoff = age_cutoff(overdue);
    if (!cutoff) die("--overdue %s: an age (5y, 18m, 90d) or a date (2021, 2021-06, 2021-06-30)", overdue);
    if (tsv) return todo_tsv(&cat, cutoff);
    size_t items = 0;
    int s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0;
    for (size_t i = 0; i < cat.discs.n; i++) {                  /* images made, never burned */
        const char *id = get_or(cat.discs.v[i], "Id", "");
        if (retired(&cat, id) || burned_at_all(&cat, id)) continue;
        heading(&s1, "No copy yet (burn the image and arv burned ID --device DRIVE, or keep it: arv stored ID PATH):");
        printf("  %s  made %s\n", id, get_or(cat.discs.v[i], "Date", ""));
        items++;
    }
    for (size_t i = 0; i < cat.discs.n; i++) {                  /* burned copies never read back */
        const char *id = get_or(cat.discs.v[i], "Id", "");
        if (retired(&cat, id) || !burned_at_all(&cat, id)) continue;
        sbuf which = { 0 };
        for (size_t k = 0; k < cat.events.n; k++) {
            const rec_record *e = cat.events.v[k];
            const char *letter = copy_letter(&cat, id, e);
            if (!is_copy_event(e, id) || !letter || !strcmp(get_or(e, "Outcome", ""), "failure")
                || strcmp(get_or(e, "Form", "disc"), "disc") || copy_read_back(&cat, id, e) || copy_same_as(&cat, id, e))
                continue;
            sb_printf(&which, "%s%s", which.len ? ", " : "", letter);
        }
        if (which.len) {
            heading(&s2, "Burned, never read back (arv check DRIVE --copy X ID):");
            printf("  %s  cop%s %s\n", id, strchr(which.s, ',') ? "ies" : "y", which.s);
            items++;
        }
        free(which.s);
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
    int s_one = 0;
    for (size_t i = 0; i < cat.discs.n; i++) {                  /* copies on one carrier: lost together */
        const char *id = get_or(cat.discs.v[i], "Id", "");
        if (retired(&cat, id)) continue;
        for (size_t k = 0; k < cat.events.n; k++) {
            const rec_record *e = cat.events.v[k], *with = is_copy_event(e, id) ? copy_shares_carrier(&cat, id, e) : NULL;
            if (!with) continue;
            heading(&s_one, "Copies on one carrier, lost together and counted once (keep one on another tape or drive):");
            printf("  %s  copies %s and %s on %s\n", id, letter_of(&cat, with), letter_of(&cat, e), copy_carrier(&cat, id, e));
            items++;
        }
    }
    int s_gone = 0;
    for (size_t i = 0; i < cat.discs.n; i++) {                  /* stored copies not where they were recorded */
        const char *id = get_or(cat.discs.v[i], "Id", "");
        if (retired(&cat, id)) continue;
        for (size_t k = 0; k < cat.events.n; k++) {
            const rec_record *e = cat.events.v[k];
            const char *at = is_copy_event(e, id) ? copy_path(&cat, id, e) : NULL;
            if (!at || !strcmp(get_or(e, "Outcome", ""), "failure") || !access(at, F_OK)) continue;
            heading(&s_gone, "Stored copies not found where recorded (a drive not mounted? moved: arv stored ID NEWPATH --copy X):");
            printf("  %s  copy %s  %s\n", id, letter_of(&cat, e), at);
            items++;
        }
    }
    int s7 = 0;
    for (size_t i = 0; i < cat.discs.n; i++) {                  /* no cold copy */
        const char *id = get_or(cat.discs.v[i], "Id", "");
        if (retired(&cat, id) || !burned_at_all(&cat, id)) continue;
        int cold = 0;
        for (size_t k = 0; k < cat.events.n; k++)
            cold |= is_copy_event(cat.events.v[k], id) && !strcmp(get_or(cat.events.v[k], "Temperature", ""), "cold");
        if (cold) continue;
        heading(&s7, "No cold copy, every copy is online or in use (burn one for the shelf):");
        printf("  %s\n", id);
        items++;
    }
    int s6 = 0, s10 = 0;
    for (size_t i = 0; i < cat.discs.n; i++) {                  /* kept in one place; below its appraisal */
        const rec_record *d = cat.discs.v[i];
        const char *id = get_or(d, "Id", "");
        if (retired(&cat, id) || !burned_at_all(&cat, id)) continue;
        strlist places;
        archive_copy_locations(&cat, id, &places);
        size_t place_count = places.n;
        strlist_free(&places);
        char *why;
        int level = disc_importance(&cat, id, &why);
        if (level < 0) {                                        /* not appraised: two places */
            if (place_count >= 2) continue;
            heading(&s5, "Kept in fewer than two places (arv burned/stored ... --location PLACE, or arv locate ID COPY PLACE):");
            printf("  %s  %s\n", id, place_count ? "one place" : "no place recorded");
            items++;
            continue;
        }
        static const size_t need[] = { 3, 2, 1, 0 };            /* copies, and places, by level */
        holding hd = { 0 };
        hold(&cat, id, &hd);
        if (hd.copies < need[level] || place_count < need[level]) {
            heading(&s10, "Fewer copies or places than its appraisal asks for (essential: 3 copies in 3 places; "
                          "important: 2 in 2; useful: 1):");
            printf("  %s  %s: %zu cop%s in %zu place%s\n", id, why, hd.copies, hd.copies == 1 ? "y" : "ies", place_count,
                   place_count == 1 ? "" : "s");
            items++;
        }
        free(why);
    }
    for (size_t i = 0; i < cat.discs.n; i++) {                  /* checks overdue */
        const char *id = get_or(cat.discs.v[i], "Id", "");
        const char *last = disc_last_check(&cat, id);
        if (retired(&cat, id) || !*last || strcmp(last, cutoff) >= 0) continue;
        char head[160];
        snprintf(head, sizeof head, "Not checked since %s (arv check DRIVE ID, or the copy's image or folder):", cutoff);
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
        char *root = home_root(&h);
        size_t there = object_sources(root, &top, &src);
        free(root);
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
    if (!items && !cat.discs.n)
        puts("No discs yet, so nothing is owed. Start with arv collection init FOLDER (or arv make FOLDER for a one-off disc).");
    else if (!items) printf("Nothing owed: every disc copied, read back, with a cold copy, kept in two places (or as its appraisal asks) and checked since %s.\n", cutoff);
    question_list q;                /* last, and not owed: a missing description is never as urgent as a missing copy */
    questions_load(&h, &q);
    strlist asked = { 0 };
    int s11 = 0;
    for (size_t i = 0; i < q.n; i++) {
        const char *about = q.v[i].about;
        if (q.v[i].skipped || strlist_has(&asked, about)) continue;
        strlist_add(&asked, about);
        int known = 0;
        if (strncmp(about, "object:", 7)) known = archive_disc(&cat, about) && !retired(&cat, about);
        else
            for (size_t k = 0; k < cat.objects.n && !known; k++) known = !strcmp(get_or(cat.objects.v[k], "Uuid", ""), about + 7);
        if (!known) continue;
        heading(&s11, "Questions waiting for an answer, never urgent (arv describe DISC-ID|OBJECT --answer):");
        size_t n = questions_open(&q, about);
        char *name = question_about_name(&cat, about);
        printf("  %s  %zu question%s%s\n", name, n, n == 1 ? "" : "s", strncmp(about, "object:", 7) ? "" : " (data object)");
        free(name);
    }
    strlist_free(&asked);
    questions_free(&q);
    return 0;
}

/* ------------------------------------------------------------------ arv retire */

/* a disc leaves every location and is marked retired, logged as "NOTE; was kept at ...TAIL" */
static void retire_disc(archive *cat, rec_record *d, const char *today, const char *who, const char *note, const char *tail)
{
    char *where = archive_where(cat, d);
    rec_add(d, "Retired", today);
    char *full = xprintf("%s%s%s%s", note, *where ? "; was kept at " : "", where, tail);
    recs_add(&cat->events, new_event(get_or(d, "Id", ""), "deaccession", "success", who, "human", full));
    free(full);
    free(where);
}

static int by_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* the SHA-256 of every file in a manifest ("SHA256  path" lines) */
static void manifest_hashes(const char *path, strlist *out)
{
    char *t = access(path, F_OK) ? NULL : read_text(path);
    for (char *l = t; l && *l;) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        if (strlen(l) > 66) {
            l[64] = 0;
            strlist_add(out, l);
        }
        l = nl ? nl + 1 : l + strlen(l);
    }
    free(t);
}

/* arv retire DISC-ID: one disc not part of a collection's edition (a disc plan's, or arv make
 * FOLDER's). Refused while a file is on no other disc that stays (the newest version of a data
 * object only here is named), unless --accept-loss, which records them on the disc as Lost. */
static int retire_one(const arv_home *h, archive *cat, rec_record *d, int yes, int accept_loss, int verbose)
{
    const char *id = get_or(d, "Id", "");
    if (rec_get(d, "Retired")) {
        printf("%s: retired already (%s)\n", id, rec_get(d, "Retired"));
        return 0;
    }
    for (size_t i = 0; i < cat->revisions.n; i++) {         /* an edition's disc goes with its edition */
        strlist v = { 0 };
        volumes(cat->revisions.v[i], &v);
        int in = strlist_has(&v, id);
        strlist_free(&v);
        if (!in) continue;
        const rec_record *c = NULL;
        for (size_t k = 0; k < cat->collections.n; k++)
            if (!strcmp(get_or(cat->collections.v[k], "Uuid", "-"), get_or(cat->revisions.v[i], "Collection", "")))
                c = cat->collections.v[k];
        char *msg = xprintf("%s is a disc of edition %s of %s: editions are retired together (arv retire %s)", id,
                            get_or(cat->revisions.v[i], "Edition", "?"), c ? get_or(c, "Code", "?") : "?",
                            c ? get_or(c, "Code", "CODE") : "CODE");
        die("%s", msg);
    }
    strlist keep = { 0 };                                   /* every file on a disc that stays */
    for (size_t i = 0; i < cat->discs.n; i++) {
        const char *other = get_or(cat->discs.v[i], "Id", "");
        if (!strcmp(other, id) || retired(cat, other)) continue;
        char *m = home_volume_file(h, other, "manifest.sha256");
        manifest_hashes(m, &keep);
        free(m);
    }
    if (keep.n) qsort(keep.v, keep.n, sizeof *keep.v, by_str);
    char *m = home_volume_file(h, id, "manifest.sha256"), *t = access(m, F_OK) ? NULL : read_text(m);
    if (!t) die2("the file list of %s is not in the home catalogue (%s): cannot tell what retiring it would lose", id, m);
    strlist only = { 0 };                                   /* "SHA256  PATH" (under data/) */
    for (char *l = t; *l;) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        if (strlen(l) > 66) {
            char hash[65], *key = hash;
            memcpy(hash, l, 64);
            hash[64] = 0;
            if (!keep.n || !bsearch(&key, keep.v, keep.n, sizeof *keep.v, by_str)) {
                const char *path = l + 66;
                char *line = xprintf("%s  %s", hash, strncmp(path, "data/", 5) ? path : path + 5);
                strlist_add(&only, line);
                free(line);
            }
        }
        l = nl ? nl + 1 : l + strlen(l);
    }
    free(t);
    free(m);
    printf("%s: %s\n", id, get_or(d, "Title", ""));
    int newest_lost = 0;                                    /* its data objects: which version, and elsewhere? */
    for (size_t i = 0; i < cat->objects.n; i++) {
        const rec_record *o = cat->objects.v[i];
        if (strcmp(get_or(o, "Disc", ""), id)) continue;
        const char *elsewhere = NULL;
        for (size_t k = 0; k < cat->objects.n && !elsewhere; k++) {
            const rec_record *x = cat->objects.v[k];
            if (x != o && strcmp(get_or(x, "Disc", ""), id) && !retired(cat, get_or(x, "Disc", "")) &&
                !strcmp(get_or(x, "Uuid", ""), get_or(o, "Uuid", "-")) && version_of(x) == version_of(o))
                elsewhere = get_or(x, "Disc", "?");
        }
        long top = newest(cat, get_or(o, "Uuid", ""), NULL, NULL);
        int file = !strcmp(get_or(o, "Kind", ""), "file");
        printf("  %s%s version %ld", get_or(o, "Name", "?"), file ? "" : "/", version_of(o));
        const rec_record *twin = NULL;      /* the same bytes as another object, on a disc that stays */
        for (size_t k = 0; k < cat->objects.n && !elsewhere && !twin; k++) {
            const rec_record *x = cat->objects.v[k];
            if (x != o && strcmp(get_or(x, "Disc", ""), id) && !retired(cat, get_or(x, "Disc", ""))
                && strcmp(get_or(x, "Uuid", ""), get_or(o, "Uuid", "-")) && !strcmp(get_or(x, "Tree", ""), get_or(o, "Tree", "-")))
                twin = x;
        }
        if (elsewhere) printf(": also on %s\n", elsewhere);
        else if (version_of(o) < top) printf(": only here; replaced by version %ld\n", top);
        else if (twin)
            printf(": the newest version, only here as this object; the same bytes are %s%s version %ld on %s\n",
                   get_or(twin, "Name", "?"), strcmp(get_or(twin, "Kind", ""), "file") ? "/" : "", version_of(twin),
                   get_or(twin, "Disc", "?"));
        else {
            printf(": the newest version, and its only copy is here\n");
            newest_lost++;
        }
    }
    if (only.n) {
        printf("%zu file%s on no disc that stays:\n", only.n, only.n == 1 ? " is" : "s are");
        size_t limit = verbose ? only.n : only.n < 20 ? only.n : 20;
        for (size_t i = 0; i < limit; i++) printf("  %s\n", only.v[i] + 66);
        if (limit < only.n) printf("  ... %zu more (-v for all)\n", only.n - limit);
    }
    if (only.n && !accept_loss) {
        printf("%s: this disc holds the only copy of %s%s. Archive %s again first, or arv retire %s --yes "
               "--accept-loss to retire it anyway.\n", yes ? "Refused" : "Nothing recorded",
               only.n == 1 ? "that file" : "those files", newest_lost ? ", including the newest version of a data object" : "",
               only.n == 1 ? "it" : "them", id);
        strlist_free(&only);
        strlist_free(&keep);
        return yes ? 1 : 0;
    }
    if (!yes) {
        printf("Nothing recorded. arv retire %s --yes%s records it as retired%s (arv deletes nothing: the disc is "
               "yours to keep or destroy).\n", id, only.n ? " --accept-loss" : "", only.n ? ", and those files as lost" : "");
        strlist_free(&only);
        strlist_free(&keep);
        return 0;
    }
    for (size_t i = 0; i < only.n; i++) rec_add(d, "Lost", only.v[i]);
    char today[11], *who = person();
    today_iso(today);
    retire_disc(cat, d, today, who, "retired", only.n ? "; files only on it recorded as lost (Lost:)" : "");
    archive_save(cat, h->rec_path);
    printf("%s retired (logged)\n", id);
    if (only.n) printf("%zu file%s recorded as lost (arv find lists them)\n", only.n, only.n == 1 ? "" : "s");
    free(who);
    strlist_free(&only);
    strlist_free(&keep);
    return 0;
}

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
    rec_record *one = archive_disc(&cat, code);
    if (one) return retire_one(&h, &cat, one, yes, accept_loss, verbose);
    const rec_record *c = archive_collection(&cat, code);
    if (!c) die("no collection or disc %s", code);
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
            char *note = xprintf("retired: edition %s of %s, replaced by edition %s (revision %.12s)",
                                 get_or(old.v[i], "Edition", ""), get_or(c, "Code", ""), get_or(by, "Edition", ""),
                                 get_or(by, "Node", ""));
            retire_disc(&cat, d, today, who, note, at_risk.n ? "; files only on retired discs recorded as lost (Lost:)" : "");
            free(note);
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

/* ------------------------------------------------------------------ inventory: a library's barcodes against the record */

typedef struct {
    char *barcode, *where;          /* where: "slot 3", "drive 0", or "" */
} seen_tag;

/* the barcodes in mtx status output (VolumeTag), or one a line in a plain list */
static size_t read_tags(FILE *in, seen_tag **out)
{
    size_t n = 0;
    char line[1024];
    *out = NULL;
    while (fgets(line, sizeof line, in)) {
        char *tag = strstr(line, "VolumeTag"), where[48] = "", *p;
        if ((p = strstr(line, "Storage Element ")) && !strstr(line, "Data Transfer")) snprintf(where, sizeof where, "slot %ld", strtol(p + 16, NULL, 10));
        else if ((p = strstr(line, "Data Transfer Element "))) snprintf(where, sizeof where, "drive %ld", strtol(p + 22, NULL, 10));
        if (tag) {
            tag += 9;
            while (*tag == ' ' || *tag == '=') tag++;
        } else if (!strstr(line, "Element") && !strchr(line, ':') && line[0] != '#') {
            tag = line;                                     /* a plain list: one barcode a line */
            while (*tag == ' ' || *tag == '\t') tag++;
        } else {
            continue;
        }
        size_t len = strcspn(tag, " \t\r\n");
        if (!len) continue;
        *out = xrealloc(*out, (n + 1) * sizeof **out);
        (*out)[n].barcode = xprintf("%.*s", (int)len, tag);
        (*out)[n].where = xstrdup(where);
        n++;
    }
    return n;
}

/* the copies a barcode names: on the copy itself (an identifier), or on its carrier */
static int copy_has_barcode(const archive *cat, const char *disc, const rec_record *e, const char *id)
{
    const char *carrier = copy_carrier(cat, disc, e);
    if (carrier && !strcmp(carrier, id)) return 1;
    strlist ids;
    copy_identifiers(cat, disc, e, &ids);
    int has = strlist_has(&ids, id);
    strlist_free(&ids);
    return has;
}

/* arv inventory [-v] PLACE [FILE]: a library's or jukebox's barcodes (mtx status, or a list) against
 * the copies recorded there: which are missing, which are unknown, which were recorded elsewhere */
int cmd_inventory(int argc, char **argv)
{
    const char *given = NULL, *library = NULL, *file = NULL;
    int verbose = 0;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (argv[i][0] != '-' && !library) library = argv[i];
        else if (argv[i][0] != '-' && !file) file = argv[i];
        else return 2;
    }
    if (!library) return 2;
    arv_home h;
    home_find(&h, given, NULL);
    archive cat;
    archive_load(&cat, h.rec_path);
    char *code = place(&cat, library);
    FILE *in = file ? fopen(file, "r") : stdin;
    if (!in) die("cannot read %s", file);
    seen_tag *tags;
    size_t n = read_tags(in, &tags), known = 0, missing = 0, unknown = 0, elsewhere = 0, unmarked = 0;
    if (file) fclose(in);
    char *at = NULL;
    {
        rec_record probe = { 0 };
        probe.type = "Disc";
        rec_add(&probe, "Location", code);
        at = archive_where(&cat, &probe);
        rec_clear(&probe);
    }
    sbuf report = { 0 }, known_lines = { 0 };
    int *matched = xmalloc((n + 1) * sizeof *matched);
    for (size_t t = 0; t < n; t++) {                    /* each barcode read: the copies it names */
        char *id = xprintf("barcode:%s", tags[t].barcode);
        matched[t] = 0;
        for (size_t i = 0; i < cat.events.n; i++) {
            const rec_record *e = cat.events.v[i];
            const char *disc = rec_get(e, "Disc");
            if (!disc || !is_copy_event(e, disc) || retired(&cat, disc) || !copy_has_barcode(&cat, disc, e, id))
                continue;
            matched[t]++;
            const char *loc = copy_location(&cat, disc, e);
            if (verbose) sb_printf(&known_lines, "  %-9s %s  %s copy %s\n", tags[t].where, tags[t].barcode, disc, letter_of(&cat, e));
            if (!loc || strcmp(loc, code)) {
                char *was = copy_place(&cat, e);
                sb_printf(&report, "ELSEWHERE  %s%s%s: %s copy %s, recorded at %s (moved here? arv stored ... --copy %s --location %s, or arv locate %s %s PLACE)\n",
                          tags[t].where, *tags[t].where ? " " : "", tags[t].barcode, disc, letter_of(&cat, e), *was ? was : "no place",
                          letter_of(&cat, e), code, disc, letter_of(&cat, e));
                free(was);
                elsewhere++;
            }
        }
        if (matched[t]) known++;
        else {
            sb_printf(&report, "UNKNOWN    %s%s%s: not this archive's (or not recorded yet: --barcode %s)\n",
                      tags[t].where, *tags[t].where ? " " : "", tags[t].barcode, tags[t].barcode);
            unknown++;
        }
        free(id);
    }
    for (size_t i = 0; i < cat.discs.n; i++) {           /* each copy recorded here: is it in the library? */
        const char *disc = get_or(cat.discs.v[i], "Id", "");
        if (retired(&cat, disc)) continue;
        for (size_t k = 0; k < cat.events.n; k++) {
            const rec_record *e = cat.events.v[k];
            const char *loc = is_copy_event(e, disc)
                                  ? copy_location(&cat, rec_get(e, "Disc"), e) : NULL;
            if (!loc || strcmp(loc, code)) continue;
            int found = 0, any = 0;
            for (size_t t = 0; t < n && !found; t++) {
                char *id = xprintf("barcode:%s", tags[t].barcode);
                found = copy_has_barcode(&cat, disc, e, id);
                free(id);
            }
            const char *carrier = copy_carrier(&cat, disc, e);
            strlist ids;
            copy_identifiers(&cat, disc, e, &ids);
            for (size_t j = 0; j < ids.n; j++) any |= !strncmp(ids.v[j], "barcode:", 8);
            any |= carrier && !strncmp(carrier, "barcode:", 8);
            strlist_free(&ids);
            if (found) continue;
            if (!any) {
                sb_printf(&report, "UNMARKED   %s copy %s: recorded here, with no barcode to find it by (arv identify %s %s barcode:...)\n",
                          disc, letter_of(&cat, e), disc, letter_of(&cat, e));
                unmarked++;
            } else {
                sb_printf(&report, "MISSING    %s copy %s: recorded at %s, its barcode not read there\n", disc, letter_of(&cat, e), at);
                missing++;
            }
        }
    }
    printf("%s: %zu barcode%s read; %zu known, %zu unknown; %zu cop%s recorded here missing, %zu without a barcode, %zu recorded elsewhere\n",
           at, n, n == 1 ? "" : "s", known, unknown, missing, missing == 1 ? "y" : "ies", unmarked, elsewhere);
    if (known_lines.s) fputs(known_lines.s, stdout);
    if (report.s) fputs(report.s, stdout);
    free(known_lines.s);
    free(report.s);
    for (size_t t = 0; t < n; t++) free(tags[t].barcode), free(tags[t].where);
    free(tags);
    free(matched);
    free(at);
    free(code);
    return missing ? 1 : 0;
}
