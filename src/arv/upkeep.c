/* Looking after the archive: which copies are known good, which editions are safe, what can be
 * retired, and what is owed (research/plan.md, collections; docs/workflow.md).
 *
 *   arv todo [--overdue YEARS]   what needs doing: images not burned, copies not read back,
 *                                editions not yet safe, replaced editions, discs kept in one place,
 *                                checks overdue (default: 5 years)
 *   arv retire CODE [--yes] [-v] the editions a later safe edition replaces (not those kept): lists the
 *                                files found only on them, and records nothing without --yes
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
    if (!items) printf("Nothing owed: every disc copied, read back, with a cold copy, kept in two places and checked within %d years.\n", years);
    return 0;
}

/* ------------------------------------------------------------------ arv retire */

int cmd_retire(int argc, char **argv)
{
    const char *given = NULL, *code = NULL;
    int yes = 0, verbose = 0;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!strcmp(argv[i], "--yes")) yes = 1;
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
                if (!strlist_has(&keep_hashes, hash) && !strlist_has(&at_risk, l + 66)) strlist_add(&at_risk, l + 66);
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
    if (!yes) {
        printf("Nothing recorded. arv retire %s --yes records them as retired (arv deletes nothing: the discs are "
               "yours to keep or destroy).\n", get_or(c, "Code", ""));
        return 0;
    }
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
                                 at_risk.n ? "; files only on retired discs were listed and accepted" : "");
            recs_add(&cat.events, new_event(v.v[k], "deaccession", "success", who, "human", note));
            free(note);
            free(where);
            n++;
        }
        strlist_free(&v);
    }
    archive_save(&cat, h.rec_path);
    printf("%zu disc%s retired (logged)\n", n, n == 1 ? "" : "s");
    free(who);
    return 0;
}
