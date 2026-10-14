/*
 * arv describe (describe.py): a local LLM suggests a title, description, subjects and folder
 * tags for a folder or a disc in the catalogue, asks the owner what only they know, refines, and
 * the owner reviews every field. The result is a draft (JSON) for arv make --draft, or written
 * straight into the catalogue for a disc that already exists. Also the GUI's two calls: suggest
 * (one round, JSON in and out) and llm-status.
 */
#define _XOPEN_SOURCE 700
#include "assist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    const char *url, *model, *vision_url, *vision_model;
    int allow_remote, vision, per_folder, max_total;
} llm_opts;

/* --llm-url, --llm-model, --llm-allow-remote, --vision ...: 1 when argv[*i] was one of them */
static int llm_option(int argc, char **argv, int *i, llm_opts *o)
{
    const char *a = argv[*i], *v = *i + 1 < argc ? argv[*i + 1] : NULL;
    if (!strcmp(a, "--llm-allow-remote")) return o->allow_remote = 1;
    if (!strcmp(a, "--vision")) return o->vision = 1;
    const char **slot = !strcmp(a, "--llm-url") ? &o->url : !strcmp(a, "--llm-model") ? &o->model
        : !strcmp(a, "--vision-url") ? &o->vision_url : !strcmp(a, "--vision-model") ? &o->vision_model : NULL;
    if (slot && v) { *slot = v; (*i)++; return 1; }
    if (!strcmp(a, "--vision-per-folder") && v) { o->per_folder = atoi(v); (*i)++; return 1; }
    if (!strcmp(a, "--vision-max") && v) { o->max_total = atoi(v); (*i)++; return 1; }
    return 0;
}

static void log_line(const char *s)
{
    fprintf(stderr, "%s\n", s);
}

static void show(const suggestion *s)
{
    fprintf(stderr, "\n  Title:       %s\n  Description: %s\n  Subjects:    ", s->title, s->description);
    for (size_t i = 0; i < s->subjects.n; i++) fprintf(stderr, "%s%s", i ? ", " : "", s->subjects.v[i]);
    fputs("\n", stderr);
    int tags = 0, caps = 0;
    for (size_t i = 0; i < s->folders.n; i++) { tags |= s->folders.tags[i].n > 0; caps |= s->folders.caption[i] != NULL; }
    if (tags) {
        fputs("  Folder tags:\n", stderr);
        for (size_t i = 0; i < s->folders.n; i++) {
            if (!s->folders.tags[i].n) continue;
            char *f = xprintf("%s/", s->folders.folder[i]);
            fprintf(stderr, "    %-40s ", f);
            for (size_t k = 0; k < s->folders.tags[i].n; k++) fprintf(stderr, "%s%s", k ? ", " : "", s->folders.tags[i].v[k]);
            fputs("\n", stderr);
            free(f);
        }
    }
    if (caps) {
        fputs("  What sampled images show:\n", stderr);
        for (size_t i = 0; i < s->folders.n; i++)
            if (s->folders.caption[i]) {
                char *f = xprintf("%s/", s->folders.folder[i]);
                fprintf(stderr, "    %-40s %s\n", f, s->folders.caption[i]);
                free(f);
            }
    }
    fputs("\n", stderr);
}

/* suggest, ask the owner each question, refine; answers: question, answer, ... */
static int conversation(llm_client *c, const char *inv, const strlist *folders, int rounds, int max_questions,
                        suggestion *s, strlist *answers, char **err)
{
    memset(answers, 0, sizeof *answers);
    const char *model = llm_model(c, err);
    if (!model) return -1;
    fprintf(stderr, "Asking %s at %s ...\n", model, c->url);
    if (llm_suggest(c, inv, NULL, NULL, max_questions, folders, s, err)) return -1;
    for (int round = 0; round < rounds; round++) {
        show(s);
        if (!s->questions.n) break;
        fprintf(stderr, "The model has %zu question%s. Answer what you can; press Enter to skip one, or type '.' "
                        "to stop answering.\n", s->questions.n, s->questions.n == 1 ? "" : "s");
        size_t before = answers->n;
        for (size_t q = 0; q < s->questions.n; q++) {
            fprintf(stderr, "\nQ: %s\n", s->questions.v[q]);
            char *a = ask("A: ");
            int stop = !strcmp(a, ".");
            if (!stop && *a) {
                strlist_add(answers, s->questions.v[q]);
                strlist_add(answers, a);
            }
            free(a);
            if (stop) break;
        }
        if (answers->n == before) break;
        log_line("\nRefining with your answers ...");
        suggestion next;
        if (llm_suggest(c, inv, answers, s, round + 1 < rounds ? max_questions : 0, folders, &next, err)) return -1;
        suggestion_free(s);
        *s = next;
    }
    return 0;
}

static char *joined(const strlist *l)
{
    sbuf b = { 0 };
    for (size_t i = 0; i < l->n; i++) sb_printf(&b, "%s%s", i ? ", " : "", l->v[i]);
    return b.s ? b.s : xstrdup("");
}

/* [Enter] accepts the suggestion, '-' keeps the current value, anything else replaces it */
static char *pick(const char *label, const char *value, const char *old)
{
    char *hint = old && *old && strcmp(old, value) ? xprintf(" (current: %s)", old) : xstrdup("");
    char *prompt = xprintf("%s: [Enter] accept, '-' keep current, or type a replacement%s\n> ", label, hint);
    char *a = ask(prompt);
    free(prompt);
    free(hint);
    char *out = !strcmp(a, "-") ? xstrdup(old ? old : "") : *a ? xstrdup(a) : xstrdup(value);
    free(a);
    return out;
}

static void notes_of(const strlist *answers, strlist *notes)
{
    memset(notes, 0, sizeof *notes);
    for (size_t i = 0; i + 1 < answers->n; i += 2) {
        char *n = xprintf("Q: %s\nA: %s", answers->v[i], answers->v[i + 1]);
        strlist_add(notes, n);
        free(n);
    }
}

/* the draft as JSON: what describe saves and arv make --draft reads */
static jv *draft_json(const suggestion *s, const strlist *notes, const char *authorship, const char *agent, int with_questions)
{
    jv *d = suggestion_json(s, with_questions);
    if (!json_get(d, "captions")) jput(d, "captions", jobj());
    jput(d, "notes", jstrings(notes));
    jput(d, "authorship", jstr(authorship));
    if (agent) jput(d, "agent", jstr(agent));
    return d;
}

/* review: the owner accepts or edits each field; the reviewed suggestion replaces s */
static const char *review(suggestion *s, const rec_record *current, int disc)
{
    show(s);
    if (!disc) {                    /* an object: a title and a description (subjects and folder tags are a disc's) */
        char *title = pick("Title", s->title, rec_get(current, "Title"));
        char *desc = pick("Description", s->description, rec_get(current, "Description"));
        int kept = !strcmp(title, s->title) && !strcmp(desc, s->description);
        free(s->title);
        free(s->description);
        s->title = title;
        s->description = desc;
        strlist_free(&s->subjects);
        ftags_free(&s->folders);
        return kept ? "accepted" : "edited";
    }
    const char *old_title = current ? rec_get(current, "Title") : NULL, *old_desc = current ? rec_get(current, "Description") : NULL;
    strlist old_subjects = { 0 };
    for (size_t i = 0; current && i < current->nfields; i++)
        if (!strcmp(current->fields[i].name, "Subject")) strlist_add(&old_subjects, current->fields[i].value);
    char *subj = joined(&s->subjects), *old_subj = joined(&old_subjects);
    char *title = pick("Title", s->title, old_title), *desc = pick("Description", s->description, old_desc);
    char *subjects = pick("Subjects", subj, old_subj);
    int tags = 0, caps = 0;
    size_t ntags = 0;
    for (size_t i = 0; i < s->folders.n; i++) { if (s->folders.tags[i].n) { tags = 1; ntags++; } caps |= s->folders.caption[i] != NULL; }
    int dropped = 0;
    if (tags) {
        char *q = xprintf("Keep the %zu folder tags%s? [Y/n] ", ntags, caps ? " and image captions" : "");
        char *a = ask(q);
        dropped = a[0] == 'n' || a[0] == 'N';
        free(a);
        free(q);
    }
    int kept = !strcmp(title, s->title) && !strcmp(desc, s->description) && !strcmp(subjects, subj) && !dropped;
    free(s->title);
    free(s->description);
    s->title = title;
    s->description = desc;
    strlist_free(&s->subjects);
    char *copy = xstrdup(subjects);
    for (char *t = copy, *next; t; t = next) {
        next = strchr(t, ',');
        if (next) *next++ = 0;
        while (*t == ' ') t++;
        char *e = t + strlen(t);
        while (e > t && e[-1] == ' ') *--e = 0;
        for (char *p = t; *p; p++) *p = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
        if (*t) strlist_add(&s->subjects, t);
    }
    free(copy);
    if (dropped) ftags_free(&s->folders);
    free(subjects);
    free(subj);
    free(old_subj);
    strlist_free(&old_subjects);
    return kept ? "accepted" : "edited";
}

/* ------------------------------------------------------------------ applying a draft to a disc */

static const char *draft_note(const char *how)
{
    if (!strcmp(how, "suggested")) return "suggested by a model, not reviewed";
    if (!strcmp(how, "accepted")) return "suggested by a model, accepted by a person";
    if (!strcmp(how, "edited")) return "suggested by a model, changed by a person";
    if (!strcmp(how, "human")) return "written by a person";
    return how;
}

/* a folder of the disc (its listing has a file under it), or "." for the whole of data/ */
/* whether the disc has this folder, or this file (a tag may name either) */
static int disc_has_folder(const arv_home *h, const char *id, const char *folder)
{
    if (!strcmp(folder, ".")) return 1;
    char *path = home_volume_file(h, id, "listing.tsv"), *text = read_text(path);
    free(path);
    if (!text) return 1;                          /* no listing at home: nothing to check against */
    size_t n = strlen(folder);
    int found = 0;
    for (char *line = strtok(text, "\n"); line && !found; line = strtok(NULL, "\n")) {
        if (*line == '#') continue;
        char *p = strrchr(line, '\t');           /* the path is the last column */
        p = p ? p + 1 : line;
        found = !strncmp(p, folder, n) && (p[n] == '/' || !p[n]);
    }
    free(text);
    return found;
}

/* an object's description written on every record of one of its versions (each disc holding it),
 * with an event naming it (Object: object:UUID); the caller saves */
static void apply_to_object(archive *cat, const char *uuid, const char *version, const char *title, const char *desc,
                            const strlist *notes, const char *agent, const char *how, strlist *changed)
{
    memset(changed, 0, sizeof *changed);
    for (size_t i = 0; i < cat->objects.n; i++) {
        rec_record *o = cat->objects.v[i];
        if (strcmp(rec_get(o, "Uuid") ? rec_get(o, "Uuid") : "", uuid) || strcmp(rec_get(o, "Version") ? rec_get(o, "Version") : "", version))
            continue;
        if (title && (!rec_get(o, "Title") || strcmp(title, rec_get(o, "Title")))) {
            rec_set(o, "Title", title);
            if (!strlist_has(changed, "Title")) strlist_add(changed, "Title");
        }
        if (desc && (!rec_get(o, "Description") || strcmp(desc, rec_get(o, "Description")))) {
            rec_set(o, "Description", desc);
            if (!strlist_has(changed, "Description")) strlist_add(changed, "Description");
        }
        for (size_t k = 0; notes && k < notes->n; k++) {
            int have = 0;
            for (size_t f = 0; f < o->nfields && !have; f++) have = !strcmp(o->fields[f].name, "Note") && !strcmp(o->fields[f].value, notes->v[k]);
            if (have) continue;
            rec_add(o, "Note", notes->v[k]);          /* before Source, the home's own field, which stays last */
            for (size_t f = 0; f + 1 < o->nfields; f++)
                if (!strcmp(o->fields[f].name, "Source")) {
                    rec_field note = o->fields[o->nfields - 1];
                    memmove(&o->fields[f + 1], &o->fields[f], (o->nfields - 1 - f) * sizeof *o->fields);
                    o->fields[f] = note;
                    break;
                }
            if (!strlist_has(changed, "Note")) strlist_add(changed, "Note");
        }
    }
    if (!changed->n) return;
    char *what = joined(changed), *note = xprintf("updated %s of version %s (%s)", what, version, draft_note(how));
    rec_record *e = reviewed_event(uuid, agent, how, note);
    free(e->fields[0].name);                      /* Object: object:UUID, not Disc */
    free(e->fields[0].value);
    e->fields[0].name = xstrdup("Object");
    e->fields[0].value = xprintf("object:%s", uuid);
    recs_add(&cat->events, e);
    free(what);
    free(note);
}

/* the object a draft names on a disc: by its path there, its name or its Uuid; NULL */
static rec_record *object_on_disc(const archive *cat, const char *disc_id, const char *key)
{
    for (int pass = 0; pass < 3; pass++)
        for (size_t i = 0; i < cat->objects.n; i++) {
            rec_record *o = cat->objects.v[i];
            static const char *const by[] = { "Path", "Name", "Uuid" };
            if (rec_get(o, "Disc") && !strcmp(rec_get(o, "Disc"), disc_id) && rec_get(o, by[pass])
                && !strcmp(rec_get(o, by[pass]), key))
                return o;
        }
    return NULL;
}

/* a draft applied to a data object: its title, description and notes, on its newest version */
static void apply_object_draft(const arv_home *h, archive *cat, const rec_record *obj, const draft *d, strlist *changed)
{
    if (d->subjects.n || d->nft || d->ncap) log_line("Subjects and folder tags are a disc's: left out for a data object.");
    const char *how = d->authorship ? d->authorship : is_model(d->agent) ? "suggested" : "human";
    char *uuid = xstrdup(rec_get(obj, "Uuid")), *version = xstrdup(rec_get(obj, "Version") ? rec_get(obj, "Version") : "1");
    apply_to_object(cat, uuid, version, d->title, d->description, &d->notes, d->agent, how, changed);
    if (changed->n) archive_save(cat, h->rec_path);
    free(uuid);
    free(version);
}

/* describe.apply_to_disc: the draft written into the home catalogue, with an event; folder tags
 * and captions are merged into the disc's (only the folders named change), and a folder the disc
 * does not have is skipped with a warning */
static void apply_to_disc(const arv_home *h, archive *cat, rec_record *disc, const draft *d, const char *agent, strlist *changed)
{
    memset(changed, 0, sizeof *changed);
    char *id = xstrdup(rec_get(disc, "Id"));      /* a copy: the record's fields are rebuilt below */
    ftags before = { 0 }, f = { 0 };              /* the disc's tags now, and as the draft leaves them */
    if (d->title && (!rec_get(disc, "Title") || strcmp(d->title, rec_get(disc, "Title")))) {
        rec_set(disc, "Title", d->title);
        strlist_add(changed, "Title");
    }
    if (d->description && (!rec_get(disc, "Description") || strcmp(d->description, rec_get(disc, "Description")))) {
        rec_set(disc, "Description", d->description);
        strlist_add(changed, "Description");
    }
    strlist old = { 0 };
    for (size_t i = 0; i < disc->nfields; i++)
        if (!strcmp(disc->fields[i].name, "Subject")) strlist_add(&old, disc->fields[i].value);
    int same = old.n == d->subjects.n;
    for (size_t i = 0; same && i < old.n; i++) same = !strcmp(old.v[i], d->subjects.v[i]);
    if (d->subjects.n && !same) {
        /* the subjects replace the old ones, before the first Note, Location, Rights, Media or Files */
        rec_record keep = { 0 };
        rec_copy(&keep, disc);
        rec_clear(disc);
        int placed = 0;
        for (size_t i = 0; i < keep.nfields; i++) {
            const char *n = keep.fields[i].name;
            if (!strcmp(n, "Subject")) continue;
            if (!placed && (!strcmp(n, "Note") || !strcmp(n, "Location") || !strcmp(n, "Rights") || !strcmp(n, "Media") || !strcmp(n, "Files"))) {
                for (size_t k = 0; k < d->subjects.n; k++) rec_add(disc, "Subject", d->subjects.v[k]);
                placed = 1;
            }
            rec_add(disc, n, keep.fields[i].value);
        }
        if (!placed)
            for (size_t k = 0; k < d->subjects.n; k++) rec_add(disc, "Subject", d->subjects.v[k]);
        rec_clear(&keep);
        free(keep.fields);
        strlist_add(changed, "Subject");
    }
    strlist_free(&old);
    for (size_t i = 0; i < d->notes.n; i++) rec_add(disc, "Note", d->notes.v[i]);
    if (d->notes.n) strlist_add(changed, "Note");
    if (d->nft || d->ncap) {
        char *path = home_volume_file(h, id, "tags.tsv");
        read_tags_now(cat, id, path, &before);
        read_tags_now(cat, id, path, &f);
        size_t skipped = 0;
        for (size_t i = 0; i < d->nft; i++) {
            if (!disc_has_folder(h, id, d->ft_folder[i])) {
                fprintf(stderr, "%s has no folder or file %s: its tags are skipped\n", id, d->ft_folder[i]);
                skipped++;
                continue;
            }
            strlist *t = ftags_get(&f, d->ft_folder[i], 1);
            strlist_free(t);
            for (size_t k = 0; k < d->ft_tags[i].n; k++) strlist_add(t, d->ft_tags[i].v[k]);
        }
        for (size_t i = 0; i < d->ncap; i++) {
            if (!disc_has_folder(h, id, d->cap_folder[i])) {
                fprintf(stderr, "%s has no folder or file %s: its caption is skipped\n", id, d->cap_folder[i]);
                skipped++;
                continue;
            }
            ftags_set_caption(&f, d->cap_folder[i], d->cap_text[i]);
        }
        if (record_tag_changes(cat, NULL, id, &before, &f)) strlist_add(changed, "tags");   /* recorded on the event */
        free(path);
    }
    const char *how = d->authorship ? d->authorship : is_model(agent) ? "suggested" : "human";
    int objects = 0;
    for (size_t i = 0; i < d->nobj; i++) {        /* its data objects, each by its path on the disc */
        const rec_record *o = object_on_disc(cat, id, d->obj[i].path);
        if (!o) {
            fprintf(stderr, "%s has no data object %s: its description is skipped\n", id, d->obj[i].path);
            continue;
        }
        char *uuid = xstrdup(rec_get(o, "Uuid")), *version = xstrdup(rec_get(o, "Version") ? rec_get(o, "Version") : "1");
        strlist oc;
        apply_to_object(cat, uuid, version, d->obj[i].title, d->obj[i].description, &d->obj[i].notes, agent, how, &oc);
        if (oc.n) {
            char *what = joined(&oc), *line = xprintf("object %s (%s)", rec_get(o, "Name") ? rec_get(o, "Name") : uuid, what);
            strlist_add(changed, line);
            free(line);
            free(what);
            objects++;
        }
        strlist_free(&oc);
        free(uuid);
        free(version);
    }
    if (changed->n) {
        if ((size_t)objects < changed->n) {
            strlist mine = { 0 };                     /* the disc's own changes, for its event */
            for (size_t i = 0; i < changed->n; i++)
                if (strncmp(changed->v[i], "object ", 7)) strlist_add(&mine, changed->v[i]);
            char *what = joined(&mine), *note = xprintf("updated %s (%s)", what, draft_note(how));
            rec_record *ev = reviewed_event(id, agent, how, note);
            static const char *const replaced[] = { "Title", "Description", "Subject", NULL };   /* notes and tags add up */
            for (int k = 0; replaced[k]; k++)
                if (strlist_has(&mine, replaced[k])) sets_field(cat, ev, id, replaced[k]);
            record_tag_changes(cat, ev, id, &before, &f);  /* each path's new tags: Tags:, Caption: */
            recs_add(&cat->events, ev);
            free(what);
            free(note);
            strlist_free(&mine);
        }
        archive_save(cat, h->rec_path);
    }
    ftags_free(&before);
    ftags_free(&f);
    free(id);
}

/* a suggestion (or a reviewed one) as a draft, for applying straight away */
static void draft_of(const suggestion *s, const strlist *notes, const char *how, const char *agent, draft *d)
{
    memset(d, 0, sizeof *d);
    d->title = *s->title ? xstrdup(s->title) : NULL;
    d->description = *s->description ? xstrdup(s->description) : NULL;
    for (size_t i = 0; i < s->subjects.n; i++) strlist_add(&d->subjects, s->subjects.v[i]);
    for (size_t i = 0; notes && i < notes->n; i++) strlist_add(&d->notes, notes->v[i]);
    for (size_t i = 0; i < s->folders.n; i++) {
        if (s->folders.tags[i].n) {
            d->ft_folder = xrealloc(d->ft_folder, (d->nft + 1) * sizeof *d->ft_folder);
            d->ft_tags = xrealloc(d->ft_tags, (d->nft + 1) * sizeof *d->ft_tags);
            d->ft_folder[d->nft] = xstrdup(s->folders.folder[i]);
            memset(&d->ft_tags[d->nft], 0, sizeof *d->ft_tags);
            for (size_t k = 0; k < s->folders.tags[i].n; k++) strlist_add(&d->ft_tags[d->nft], s->folders.tags[i].v[k]);
            d->nft++;
        }
        if (s->folders.caption[i]) {
            d->cap_folder = xrealloc(d->cap_folder, (d->ncap + 1) * sizeof *d->cap_folder);
            d->cap_text = xrealloc(d->cap_text, (d->ncap + 1) * sizeof *d->cap_text);
            d->cap_folder[d->ncap] = xstrdup(s->folders.folder[i]);
            d->cap_text[d->ncap++] = xstrdup(s->folders.caption[i]);
        }
    }
    d->agent = xstrdup(agent);
    d->authorship = xstrdup(how);
}

/* ------------------------------------------------------------------ targets */

typedef struct {
    items files;
    char *name, *text_root, *image_root;
    rec_record *disc;       /* NULL: a folder or an object */
    rec_record *object;     /* a data object: its newest version (one of its records) */
} target;

/* a data object by its name, its Uuid or object:UUID: one record of its newest version; NULL
 * when there is none, or (with *err) when two objects have that name */
static rec_record *find_object(const archive *cat, const char *t, char **err)
{
    if (!strncmp(t, "object:", 7)) t += 7;
    rec_record *best = NULL;
    for (size_t i = 0; i < cat->objects.n; i++) {
        rec_record *o = cat->objects.v[i];
        const char *u = rec_get(o, "Uuid"), *n = rec_get(o, "Name");
        if (!u || (strcmp(u, t) && (!n || strcmp(n, t)))) continue;
        if (best && strcmp(rec_get(best, "Uuid"), u)) {
            *err = xprintf("two data objects are called %s: name one by its Uuid (%s or %s; arv objects --json lists them)", t,
                           rec_get(best, "Uuid"), u);
            return NULL;
        }
        if (!best || atol(rec_get(o, "Version") ? rec_get(o, "Version") : "0") > atol(rec_get(best, "Version") ? rec_get(best, "Version") : "0"))
            best = o;
    }
    return best;
}

/* the files of an object's version, from its disc's listing, relative to the object */
static void items_of_object(const arv_home *h, const rec_record *o, items *out)
{
    items all;
    items_of_disc(h, rec_get(o, "Disc"), &all);
    const char *p = rec_get(o, "Path") ? rec_get(o, "Path") : ".";
    size_t n = strlen(p);
    memset(out, 0, sizeof *out);
    out->v = xmalloc((all.n + 1) * sizeof *out->v);
    for (size_t i = 0; i < all.n; i++) {
        const char *rel = !strcmp(p, ".") ? all.v[i].path
                          : !strcmp(all.v[i].path, p) ? strrchr(p, '/') ? strrchr(p, '/') + 1 : p
                          : !strncmp(all.v[i].path, p, n) && all.v[i].path[n] == '/' ? all.v[i].path + n + 1 : NULL;
        if (!rel || git_internal(rel)) continue;
        out->v[out->n] = all.v[i];
        out->v[out->n++].path = xstrdup(rel);
    }
    items_free(&all);
}

static int open_target(const char *t, const arv_home *h, archive *cat, const char *disc_root, target *out, char **err)
{
    memset(out, 0, sizeof *out);
    out->disc = archive_disc(cat, t);
    struct stat st;
    if (out->disc) {
        const char *id = rec_get(out->disc, "Id");
        items_of_disc(h, id, &out->files);
        out->name = xstrdup(id);
        if (disc_root) out->text_root = join(disc_root, "data");
        out->image_root = out->text_root ? xstrdup(out->text_root) : NULL;
    } else if (!stat(t, &st) && S_ISDIR(st.st_mode)) {
        char *src = abs_path(t), *slash = strrchr(src, '/');
        items_of_folder(src, &out->files);
        out->name = xstrdup(slash && slash[1] ? slash + 1 : src);
        out->text_root = src;
        out->image_root = xstrdup(src);
    } else if ((out->object = find_object(cat, t, err))) {
        items_of_object(h, out->object, &out->files);
        out->name = xstrdup(rec_get(out->object, "Name") ? rec_get(out->object, "Name") : t);
        const char *p = rec_get(out->object, "Path") ? rec_get(out->object, "Path") : ".";
        if (disc_root) {
            char *data = join(disc_root, "data");
            out->text_root = strcmp(p, ".") ? join(data, p) : xstrdup(data);
            free(data);
        } else if (rec_get(out->object, "Source")) {   /* where it came from, when it is still there */
            char *root = home_root(h), *src = path_abs(root, rec_get(out->object, "Source"));
            if (!stat(src, &st) && S_ISDIR(st.st_mode)) out->text_root = src;
            else free(src);
            free(root);
        }
        out->image_root = out->text_root ? xstrdup(out->text_root) : NULL;
    } else {
        if (!*err) *err = xprintf("%s is neither a disc id in the catalogue, a folder, nor a data object (arv objects lists them)", t);
        return -1;
    }
    return 0;
}

/* "Title": description (coverage; rights; origin), from an object's fields or what a folder says */
static void about_line(sbuf *b, const char *where, const char *const values[5])
{
    sb_printf(b, "  %s: ", where);
    if (values[0]) sb_printf(b, "\"%s\"%s", values[0], values[1] ? ": " : "");
    if (values[1]) sb_puts(b, values[1]);
    int facts = 0;
    for (int k = 2; k < 5; k++)
        if (values[k]) sb_printf(b, "%s%s", facts++ ? "; " : values[0] || values[1] ? " (" : "(", values[k]);
    sb_puts(b, facts ? ")\n" : "\n");
}

/* for the model: what a disc's data objects say about themselves (from the catalogue), or the git
 * repositories in a folder (read now: their README, manifest, licence, commit dates, origin) */
static char *objects_section(const archive *cat, const target *t)
{
    sbuf b = { 0 };
    sb_puts(&b, "");
    if (t->disc) {
        for (size_t i = 0; i < cat->objects.n; i++) {
            const rec_record *o = cat->objects.v[i];
            if (!rec_get(o, "Disc") || strcmp(rec_get(o, "Disc"), rec_get(t->disc, "Id"))) continue;
            const char *values[5];
            for (int k = 0; k < 5; k++) values[k] = rec_get(o, ABOUT_FIELDS[k]);
            if (!values[0] && !values[1] && !values[2] && !values[3] && !values[4]) continue;
            if (!b.len) sb_puts(&b, "\n\nData objects on it, as they describe themselves:\n");
            const char *p = rec_get(o, "Path") ? rec_get(o, "Path") : ".";
            const char *kind = rec_get(o, "Kind") ? rec_get(o, "Kind") : "";
            char *where = !strcmp(p, ".") ? xstrdup("the whole disc") : xprintf("%s%s", p, strcmp(kind, "file") ? "/" : "");
            about_line(&b, where, values);
            free(where);
        }
    } else if (!t->object && t->text_root) {
        strlist dirs;
        llm_folders(&t->files, &dirs);
        size_t found = 0;
        for (size_t i = 0; i < dirs.n && found < 20; i++) {
            char *dir = strcmp(dirs.v[i], ".") ? join(t->text_root, dirs.v[i]) : xstrdup(t->text_root), *git = join(dir, ".git");
            struct stat st;
            if (!stat(git, &st)) {
                about_info a;
                about_read(dir, 1, &a);
                if (about_any(&a)) {
                    if (!found++) sb_puts(&b, "\n\nGit repositories in it, as they describe themselves:\n");
                    const char *values[5];
                    about_fields(&a, values);
                    char *where = xprintf("%s/", dirs.v[i]);
                    about_line(&b, where, values);
                    free(where);
                }
                about_free(&a);
            }
            free(git);
            free(dir);
        }
        strlist_free(&dirs);
    }
    while (b.len && b.s[b.len - 1] == '\n') b.s[--b.len] = 0;
    return b.s;
}

static void close_target(target *t)
{
    items_free(&t->files);
    free(t->name);
    free(t->text_root);
    free(t->image_root);
}

static int look_at_images(const llm_opts *o, llm_client *c, const target *t, seen *out, char **err)
{
    memset(out, 0, sizeof *out);
    if (!o->vision) return 0;
    if (!t->image_root) {
        *err = xstrdup("--vision needs the files: pass a folder, or --disc-root for a mounted disc");
        return -1;
    }
    llm_client v;
    const char *model = o->vision_model;
    if (!model && !(model = llm_model(c, err))) return -1;
    if (llm_open(&v, o->vision_url ? o->vision_url : c->url, model, 0, err)) return -1;
    fprintf(stderr, "Looking at sample images with %s (local only) ...\n", model);
    int rc = vision_analyse(&v, t->image_root, &t->files, o->per_folder, o->max_total, out, err);
    if (!rc && !out->n) log_line("No images that could be shown (install ffmpeg for TIFF/HEIC/RAW and video).");
    return rc;
}

/* ------------------------------------------------------------------ questions kept for later */

/* what a question is about: a disc's id, or object:UUID */
static char *about_of(const rec_record *disc, const rec_record *obj)
{
    return disc ? xstrdup(rec_get(disc, "Id")) : xprintf("object:%s", rec_get(obj, "Uuid"));
}

/* a draft's questions, kept for whoever answers later; how many were new */
static size_t keep_questions(const arv_home *h, const archive *cat, const char *about, const draft *d)
{
    if (!d->questions.n) return 0;
    question_list q;
    questions_load(h, &q);
    char today[11];
    today_iso(today);
    size_t n = questions_add(&q, cat, about, &d->questions, d->agent, today);
    if (n) questions_save(&q);
    questions_free(&q);
    return n;
}

/* the questions waiting about a disc or an object, asked at the terminal: an answer becomes a Note
 * (by the person), Enter skips one for good, '.' stops and leaves the rest; how many were answered */
static size_t answer_waiting(const arv_home *h, archive *cat, rec_record *disc, const rec_record *obj, const char *name)
{
    char *about = about_of(disc, obj);
    question_list q;
    questions_load(h, &q);
    size_t open = questions_open(&q, about);
    if (!open) {
        questions_free(&q);
        free(about);
        return 0;
    }
    fprintf(stderr, "%zu question%s about %s, kept from before. Answer what you can; press Enter to skip one for good, "
                    "or type '.' to stop (the rest wait).\n", open, open == 1 ? "" : "s", name);
    strlist ask_these = { 0 }, notes = { 0 };
    for (size_t i = 0; i < q.n; i++)
        if (!q.v[i].skipped && !strcmp(q.v[i].about, about)) strlist_add(&ask_these, q.v[i].text);
    char today[11];
    today_iso(today);
    size_t skipped = 0;
    for (size_t i = 0; i < ask_these.n; i++) {
        const char *agent = "";
        for (size_t k = 0; k < q.n; k++)
            if (!strcmp(q.v[k].about, about) && !strcmp(q.v[k].text, ask_these.v[i])) agent = q.v[k].agent;
        fprintf(stderr, "\nQ: %s%s%s%s\n", ask_these.v[i], *agent ? " (asked by " : "", agent, *agent ? ")" : "");
        char *a = ask("A: ");
        int stop = !strcmp(a, ".");
        if (!stop) {
            if (*a) {
                char *note = xprintf("Q: %s\nA: %s", ask_these.v[i], a);
                strlist_add(&notes, note);
                free(note);
            } else {
                skipped++;
            }
            questions_close(&q, about, ask_these.v[i], *a ? NULL : today);
        }
        free(a);
        if (stop) break;
    }
    if (notes.n) {
        char *who = person();
        strlist changed;
        if (disc) {
            draft d;
            memset(&d, 0, sizeof d);
            d.notes = notes;
            d.agent = who;
            d.authorship = (char *)"human";
            apply_to_disc(h, cat, disc, &d, who, &changed);
        } else {
            apply_to_object(cat, rec_get(obj, "Uuid"), rec_get(obj, "Version") ? rec_get(obj, "Version") : "1", NULL, NULL, &notes,
                            who, "human", &changed);
            archive_save(cat, h->rec_path);
        }
        strlist_free(&changed);
        free(who);
    }
    questions_save(&q);
    fprintf(stderr, "%zu answered (kept as notes), %zu skipped, %zu still waiting.\n\n", notes.n, skipped, questions_open(&q, about));
    size_t n = notes.n;
    strlist_free(&notes);
    strlist_free(&ask_these);
    questions_free(&q);
    free(about);
    return n;
}

/* ------------------------------------------------------------------ arv describe */

int assist_describe(int argc, char **argv)
{
    const char *tgt = NULL, *save = NULL, *disc_root = NULL, *apply = NULL, *home = NULL;
    int rounds = 2, questions = 5, show_inventory = 0, suggested = 0, answer = 0;
    llm_opts o = { 0 };
    o.per_folder = 3;
    o.max_total = 40;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (llm_option(argc, argv, &i, &o)) continue;
        if (!strcmp(a, "--rounds") && v) { rounds = atoi(v); i++; }
        else if (!strcmp(a, "--questions") && v) { questions = atoi(v); i++; }
        else if (!strcmp(a, "--save") && v) { save = v; i++; }
        else if (!strcmp(a, "--disc-root") && v) { disc_root = v; i++; }
        else if (!strcmp(a, "--apply") && v) { apply = v; i++; }
        else if (!strcmp(a, "--home") && v) { home = v; i++; }
        else if (!strcmp(a, "--show-inventory")) show_inventory = 1;
        else if (!strcmp(a, "--suggested")) suggested = 1;
        else if (!strcmp(a, "--answer")) answer = 1;
        else if (a[0] != '-' && !tgt) tgt = a;
        else return 2;
    }
    if (!tgt) return 2;
    arv_home h;
    archive cat;
    if (home_try(&h, home, NULL)) memset(&cat, 0, sizeof cat);   /* no archive: a folder can still be described */
    else archive_load(&cat, h.rec_path);
    rec_record *disc = archive_disc(&cat, tgt);
    char *err = NULL;
    if (suggested && !apply) die("%s", "--suggested goes with --apply");
    if (answer) {         /* no LLM: the questions kept about a disc or an object, asked now */
        rec_record *obj = disc ? NULL : find_object(&cat, tgt, &err);
        if (err) die("%s", err);
        if (!disc && !obj) die("--answer needs a disc id or a data object from the catalogue, not %s", tgt);
        if (!isatty(0)) die("%s", "--answer asks at a terminal");
        char *about = about_of(disc, obj), *name = question_about_name(&cat, about);
        question_list q;
        questions_load(&h, &q);
        size_t open = questions_open(&q, about);
        questions_free(&q);
        if (!open) printf("No questions waiting about %s.\n", name);
        else answer_waiting(&h, &cat, disc, obj, name);
        free(about);
        free(name);
        return 0;
    }
    if (apply) {          /* no LLM: a saved (perhaps hand-edited, or written elsewhere) draft applied to a disc */
        rec_record *obj = disc ? NULL : find_object(&cat, tgt, &err);
        if (err) die("%s", err);
        if (!disc && !obj) die("--apply needs a disc id or a data object from the catalogue, not %s", tgt);
        draft d;
        draft_load(apply, &d, !suggested);    /* --suggested: a model's draft stays a suggestion, for review */
        strlist changed;
        if (obj) {
            apply_object_draft(&h, &cat, obj, &d, &changed);
            tgt = rec_get(obj, "Name") ? rec_get(obj, "Name") : tgt;
        } else {
            apply_to_disc(&h, &cat, disc, &d, d.agent, &changed);
            tgt = rec_get(disc, "Id");
        }
        char *what = joined(&changed), *about = about_of(disc, obj);
        size_t kept = keep_questions(&h, &cat, about, &d);   /* the draft's questions wait for an answer */
        printf("%s: updated %s", tgt, *what ? what : "nothing");
        if (kept) printf("; %zu question%s kept for later (arv describe %s --answer)", kept, kept == 1 ? "" : "s", tgt);
        printf("\n");
        free(what);
        free(about);
        return 0;
    }
    llm_client c;
    if (llm_open(&c, o.url, o.model, o.allow_remote, &err)) die("%s", err);
    target t;
    if (open_target(tgt, &h, &cat, disc_root, &t, &err)) die("%s", err);
    if ((t.disc || t.object) && isatty(0) && !show_inventory)   /* the questions kept from before, first: */
        answer_waiting(&h, &cat, t.disc, t.object, t.name);     /* the model then sees the answers as notes */
    seen s;
    if (look_at_images(&o, &c, &t, &s, &err)) die("%s", err);
    char *inv = llm_inventory(&t.files, t.name, t.disc ? t.disc : t.object, t.text_root), *vis = vision_inventory_section(&s);
    char *objs = objects_section(&cat, &t), *full = xprintf("%s%s%s", inv, objs, vis);
    free(inv);
    free(objs);
    free(vis);
    if (show_inventory) {
        printf("%s\n", full);
        close_target(&t);
        return 0;
    }
    strlist folders, answers = { 0 };
    llm_folders(&t.files, &folders);
    int interactive = isatty(0);
    suggestion sug;
    int rc = interactive ? conversation(&c, full, &folders, rounds, questions, &sug, &answers, &err)
                         : llm_suggest(&c, full, NULL, NULL, questions, &folders, &sug, &err);
    if (rc) die("%s", err);
    vision_merge(&sug, &s);
    strlist notes;
    const char *how = interactive ? review(&sug, t.object ? t.object : t.disc, !t.object) : "suggested";
    notes_of(&answers, &notes);
    char *agent = llm_agent(&c);
    if (!interactive && !save) {
        sbuf b = { 0 };
        jv *d = draft_json(&sug, &notes, how, NULL, 1);
        json_dump(&b, d, 2);
        printf("%s\n", b.s);
        return 0;
    }
    if (save) {
        jv *d = draft_json(&sug, &notes, how, agent, !interactive);
        save_json(save, d);
        jfree(d);
        fprintf(stderr, "Draft saved to %s (use: arv make --draft %s ...)\n", save, save);
    }
    if ((t.disc || t.object) && interactive) {
        char *q = t.disc ? xprintf("Apply to %s in the home catalogue? [y/N] ", rec_get(t.disc, "Id"))
                         : xprintf("Apply to the data object %s (version %s) in the home catalogue? [y/N] ", t.name,
                                   rec_get(t.object, "Version") ? rec_get(t.object, "Version") : "1");
        char *a = ask(q);
        if (a[0] == 'y' || a[0] == 'Y') {
            draft d;
            strlist changed;
            draft_of(&sug, &notes, how, agent, &d);
            if (t.object) apply_object_draft(&h, &cat, t.object, &d, &changed);
            else apply_to_disc(&h, &cat, t.disc, &d, agent, &changed);
            char *what = joined(&changed);
            fprintf(stderr, "Updated: %s\n", *what ? what : "nothing");
            free(what);
        } else {
            log_line("Not applied.");
        }
        free(q);
        free(a);
    }
    return 0;
}

/* ------------------------------------------------------------------ for arv-gui */

static int print_json(jv *d)
{
    sbuf b = { 0 };
    json_dump(&b, d, -1);
    printf("%s\n", b.s);
    free(b.s);
    jfree(d);
    return 0;
}

static int print_error(const char *msg)
{
    jv *d = jobj();
    jput(d, "error", jstr(msg));
    return print_json(d);
}

/* arv-assist llm-status [--llm-url U] [--llm-model M] [--llm-allow-remote]: is a model there? */
int assist_llm_status(int argc, char **argv)
{
    llm_opts o = { 0 };
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--home") && i + 1 < argc) {       /* given by main(); not needed here */
            i++;
            continue;
        }
        if (!llm_option(argc, argv, &i, &o)) return 2;
    }
    llm_client c;
    char *err = NULL;
    jv *d = jobj();
    if (!llm_open(&c, o.url, o.model, o.allow_remote, &err) && llm_model(&c, &err)) {
        jput(d, "available", jnum("true"));
        jput(d, "url", jstr(c.url));
        jput(d, "model", jstr(c.model));
    } else {
        jput(d, "available", jnum("false"));
        jput(d, "error", jstr(err));
    }
    return print_json(d);
}

/* arv-assist suggest [--home H] [LLM options] < request.json: one round for the GUI. The request:
 * {"source": folder} or {"disc_id": id}, and "answers" [[q, a]...], "previous", "seen", "vision".
 * The reply: the suggestion, "seen" (to send back, so images are looked at once) and "agent". */
int assist_suggest(int argc, char **argv)
{
    const char *home = NULL;
    llm_opts o = { 0 };
    o.per_folder = 3;
    o.max_total = 40;
    for (int i = 0; i < argc; i++) {
        if (llm_option(argc, argv, &i, &o)) continue;
        if (!strcmp(argv[i], "--home") && i + 1 < argc) home = argv[++i];
        else return 2;
    }
    sbuf in = { 0 };
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, stdin)) > 0) sb_add(&in, buf, n);
    jv *req = in.s ? json_parse(in.s) : NULL;
    free(in.s);
    if (!req || req->kind != 'o') return print_error("the request is not a JSON object");
    char *err = NULL;
    llm_client c;
    if (llm_open(&c, o.url, o.model, o.allow_remote, &err)) return print_error(err);
    arv_home h;
    archive cat;
    if (home_try(&h, home, NULL)) memset(&cat, 0, sizeof cat);   /* no archive: a folder can still be described */
    else archive_load(&cat, h.rec_path);
    const char *disc_id = jstr_of(json_get(req, "disc_id")), *source = jstr_of(json_get(req, "source"));
    target t;
    if (open_target(disc_id ? disc_id : source ? source : "", &h, &cat, NULL, &t, &err)) return print_error(err);
    if (disc_id && !t.disc) return print_error("no such disc");
    seen s = { 0 };
    seen_from_json(json_get(req, "seen"), &s);
    const jv *vis = json_get(req, "vision");
    if (vis && vis->kind == 't' && !strcmp(vis->str, "true") && !s.n && !t.disc) {
        o.vision = 1;
        if (look_at_images(&o, &c, &t, &s, &err)) return print_error(err);
    }
    char *inv = llm_inventory(&t.files, t.name, t.disc, t.disc ? NULL : t.text_root), *vs = vision_inventory_section(&s);
    char *full = xprintf("%s%s", inv, vs);
    strlist answers = { 0 }, folders;
    const jv *qa = json_get(req, "answers");
    for (size_t i = 0; qa && qa->kind == 'a' && i < qa->n; i++) {
        const jv *pair = qa->vals[i];
        if (pair->kind != 'a' || pair->n < 2 || pair->vals[0]->kind != 's' || pair->vals[1]->kind != 's') continue;
        const char *a = pair->vals[1]->str;
        while (*a == ' ' || *a == '\t' || *a == '\n') a++;
        if (!*a) continue;
        strlist_add(&answers, pair->vals[0]->str);
        strlist_add(&answers, pair->vals[1]->str);
    }
    suggestion prev = { 0 }, sug;
    const jv *pv = json_get(req, "previous");
    int have_prev = 0;
    if (pv && pv->kind == 'o') {
        sbuf b = { 0 };
        json_dump(&b, pv, -1);
        have_prev = !llm_parse(b.s, NULL, &prev, &err);
        free(b.s);
    }
    llm_folders(&t.files, &folders);
    if (llm_suggest(&c, full, &answers, have_prev ? &prev : NULL, 5, &folders, &sug, &err)) return print_error(err);
    vision_merge(&sug, &s);
    jv *d = suggestion_json(&sug, 1);
    jput(d, "seen", seen_json(&s));
    char *agent = llm_agent(&c);
    jput(d, "agent", jstr(agent));
    free(inv);
    free(vs);
    free(full);
    return print_json(d);
}
