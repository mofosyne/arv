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
    if (!json_get(d, "folder_captions")) jput(d, "folder_captions", jobj());
    jput(d, "notes", jstrings(notes));
    jput(d, "authorship", jstr(authorship));
    if (agent) jput(d, "agent", jstr(agent));
    return d;
}

/* review: the owner accepts or edits each field; the reviewed suggestion replaces s */
static const char *review(suggestion *s, const rec_record *current)
{
    show(s);
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

/* describe.apply_to_disc: the draft written into the home catalogue, with an event */
static void apply_to_disc(const arv_home *h, archive *cat, rec_record *disc, const draft *d, const char *agent, strlist *changed)
{
    memset(changed, 0, sizeof *changed);
    char *id = xstrdup(rec_get(disc, "Id"));      /* a copy: the record's fields are rebuilt below */
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
        ftags f = { 0 };
        for (size_t i = 0; i < d->nft; i++) {
            strlist *t = ftags_get(&f, d->ft_folder[i], 1);
            strlist_free(t);
            for (size_t k = 0; k < d->ft_tags[i].n; k++) strlist_add(t, d->ft_tags[i].v[k]);
        }
        for (size_t i = 0; i < d->ncap; i++) ftags_set_caption(&f, d->cap_folder[i], d->cap_text[i]);
        char *path = home_volume_file(h, id, "tags.tsv");
        write_tags_file(path, &f);
        free(path);
        ftags_free(&f);
        strlist_add(changed, "folder tags");
    }
    if (changed->n) {
        const char *how = d->authorship ? d->authorship : is_model(agent) ? "suggested" : "human";
        char *what = joined(changed), *note = xprintf("updated %s (%s)", what, draft_note(how));
        recs_add(&cat->events, reviewed_event(id, agent, how, note));
        archive_save(cat, h->rec_path);
        free(what);
        free(note);
    }
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
    rec_record *disc;       /* NULL: a folder */
} target;

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
    } else {
        *err = xprintf("%s is neither a disc id in the catalogue nor a folder", t);
        return -1;
    }
    return 0;
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

/* ------------------------------------------------------------------ arv describe */

int assist_describe(int argc, char **argv)
{
    const char *tgt = NULL, *save = NULL, *disc_root = NULL, *apply = NULL, *home = NULL;
    int rounds = 2, questions = 5, show_inventory = 0;
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
        else if (a[0] != '-' && !tgt) tgt = a;
        else return 2;
    }
    if (!tgt) return 2;
    arv_home h;
    archive cat;
    home_find(&h, home, NULL);
    archive_load(&cat, h.rec_path);
    rec_record *disc = archive_disc(&cat, tgt);
    char *err = NULL;
    if (apply) {          /* no LLM: a saved (perhaps hand-edited) draft applied to a disc */
        if (!disc) die("--apply needs a disc id from the catalogue, not %s", tgt);
        draft d;
        draft_load(apply, &d, 1);
        strlist changed;
        apply_to_disc(&h, &cat, disc, &d, d.agent, &changed);
        char *what = joined(&changed);
        printf("%s: updated %s\n", rec_get(disc, "Id"), *what ? what : "nothing");
        free(what);
        return 0;
    }
    llm_client c;
    if (llm_open(&c, o.url, o.model, o.allow_remote, &err)) die("%s", err);
    target t;
    if (open_target(tgt, &h, &cat, disc_root, &t, &err)) die("%s", err);
    seen s;
    if (look_at_images(&o, &c, &t, &s, &err)) die("%s", err);
    char *inv = llm_inventory(&t.files, t.name, t.disc, t.text_root), *vis = vision_inventory_section(&s);
    char *full = xprintf("%s%s", inv, vis);
    free(inv);
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
    const char *how = interactive ? review(&sug, t.disc) : "suggested";
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
    if (t.disc && interactive) {
        char *q = xprintf("Apply to %s in the home catalogue? [y/N] ", rec_get(t.disc, "Id")), *a = ask(q);
        if (a[0] == 'y' || a[0] == 'Y') {
            draft d;
            strlist changed;
            draft_of(&sug, &notes, how, agent, &d);
            apply_to_disc(&h, &cat, t.disc, &d, agent, &changed);
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
    home_find(&h, home, NULL);
    archive_load(&cat, h.rec_path);
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
