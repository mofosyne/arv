/* Questions kept until someone answers them (research/plan.md, "Questions that wait"): a model's,
 * from a draft applied unattended, and arv's own about a data object that says nothing about
 * itself. They live in the home only, <home>/drafts/questions.rec (a model's unanswered guesses
 * are not archival), and are listed by arv todo and asked by arv describe ID --answer, where an
 * answer becomes a Note on the disc or object ("Q: ...\nA: ..."). A question skipped stays,
 * marked Skipped, so it is not asked again. */
#define _XOPEN_SOURCE 700
#include "arv.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MAX_OPEN 5      /* per disc or object: a short list gets answered */

static char *questions_path(const arv_home *h)
{
    return join(h->drafts_dir, "questions.rec");
}

void questions_load(const arv_home *h, question_list *q)
{
    memset(q, 0, sizeof *q);
    q->path = questions_path(h);
    rec_file f;
    int bad = 0;
    if (access(q->path, F_OK) || rec_read(q->path, &f, &bad)) return;
    for (size_t i = 0; i < f.nrecords; i++) {
        const rec_record *r = &f.records[i];
        if (r->descriptor || !r->type || strcmp(r->type, "Question") || !rec_get(r, "About") || !rec_get(r, "Text")) continue;
        q->v = xrealloc(q->v, (q->n + 1) * sizeof *q->v);
        question *x = &q->v[q->n++];
        x->about = xstrdup(rec_get(r, "About"));
        x->text = xstrdup(rec_get(r, "Text"));
        x->asked = xstrdup(rec_get(r, "Asked") ? rec_get(r, "Asked") : "");
        x->agent = xstrdup(rec_get(r, "Agent") ? rec_get(r, "Agent") : "");
        x->skipped = rec_get(r, "Skipped") ? xstrdup(rec_get(r, "Skipped")) : NULL;
    }
    rec_free(&f);
}

void questions_save(const question_list *q)
{
    sbuf b = { 0 };
    sb_puts(&b, "# Questions about discs and data objects, kept until answered (arv describe ID --answer;\n"
                "# arv todo lists them). This home's only: never on a disc.\n%rec: Question\n");
    for (size_t i = 0; i < q->n; i++) {
        rec_record *r = rec_alloc("Question");
        rec_add(r, "About", q->v[i].about);
        rec_add(r, "Text", q->v[i].text);
        if (*q->v[i].asked) rec_add(r, "Asked", q->v[i].asked);
        if (*q->v[i].agent) rec_add(r, "Agent", q->v[i].agent);
        if (q->v[i].skipped) rec_add(r, "Skipped", q->v[i].skipped);
        char *buf = NULL;
        size_t len = 0;
        rec_format(r, &buf, &len);
        sb_puts(&b, "\n");
        sb_add(&b, buf, len);
        sb_puts(&b, "\n");
        free(buf);
        rec_clear(r);
        free(r->fields);
        free(r);
    }
    char *dir = xstrdup(q->path), *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    if (mkdirs(dir)) die("cannot create %s", dir);
    char *tmp = xprintf("%s.tmp", q->path);
    write_text(tmp, b.s);
    if (rename(tmp, q->path)) die("cannot write %s", q->path);
    free(tmp);
    free(dir);
    free(b.s);
}

void questions_free(question_list *q)
{
    for (size_t i = 0; i < q->n; i++) {
        free(q->v[i].about);
        free(q->v[i].text);
        free(q->v[i].asked);
        free(q->v[i].agent);
        free(q->v[i].skipped);
    }
    free(q->v);
    free(q->path);
    memset(q, 0, sizeof *q);
}

size_t questions_open(const question_list *q, const char *about)
{
    size_t n = 0;
    for (size_t i = 0; i < q->n; i++) n += !q->v[i].skipped && !strcmp(q->v[i].about, about);
    return n;
}

/* is this question answered already, in a Note of the disc or object ("Q: TEXT\nA: ...")? */
static int answered(const archive *cat, const char *about, const char *text)
{
    const recs *in = &cat->discs;
    const char *key = "Id", *want = about;
    if (!strncmp(about, "object:", 7)) in = &cat->objects, key = "Uuid", want = about + 7;
    size_t n = strlen(text);
    for (size_t i = 0; i < in->n; i++) {
        const rec_record *r = in->v[i];
        if (!rec_get(r, key) || strcmp(rec_get(r, key), want)) continue;
        for (size_t f = 0; f < r->nfields; f++) {
            const char *v = r->fields[f].value;
            if (!strcmp(r->fields[f].name, "Note") && !strncmp(v, "Q: ", 3) && !strncasecmp(v + 3, text, n) && v[3 + n] == '\n')
                return 1;
        }
    }
    return 0;
}

size_t questions_add(question_list *q, const archive *cat, const char *about, const strlist *texts, const char *agent, const char *today)
{
    size_t added = 0;
    for (size_t k = 0; k < texts->n; k++) {
        const char *t = texts->v[k];
        if (!*t || questions_open(q, about) >= MAX_OPEN || answered(cat, about, t)) continue;
        int dup = 0;                                /* asked before: open, or skipped for good */
        for (size_t i = 0; i < q->n && !dup; i++) dup = !strcmp(q->v[i].about, about) && !strcasecmp(q->v[i].text, t);
        if (dup) continue;
        q->v = xrealloc(q->v, (q->n + 1) * sizeof *q->v);
        question *x = &q->v[q->n++];
        x->about = xstrdup(about);
        x->text = xstrdup(t);
        x->asked = xstrdup(today);
        x->agent = xstrdup(agent ? agent : "");
        x->skipped = NULL;
        added++;
    }
    return added;
}

/* an answered question goes (its answer is a Note now); a skipped one stays, marked */
void questions_close(question_list *q, const char *about, const char *text, const char *skipped)
{
    for (size_t i = 0; i < q->n; i++) {
        if (strcmp(q->v[i].about, about) || strcmp(q->v[i].text, text)) continue;
        if (skipped) {
            free(q->v[i].skipped);
            q->v[i].skipped = xstrdup(skipped);
            return;
        }
        free(q->v[i].about);
        free(q->v[i].text);
        free(q->v[i].asked);
        free(q->v[i].agent);
        free(q->v[i].skipped);
        memmove(&q->v[i], &q->v[i + 1], (q->n - i - 1) * sizeof *q->v);
        q->n--;
        return;
    }
}

/* how arv names what a question is about: a disc's id, or an object's name (and a / for a folder) */
char *question_about_name(const archive *cat, const char *about)
{
    if (strncmp(about, "object:", 7)) return xstrdup(about);
    for (size_t i = 0; i < cat->objects.n; i++) {
        const rec_record *o = cat->objects.v[i];
        if (rec_get(o, "Uuid") && !strcmp(rec_get(o, "Uuid"), about + 7))
            return xprintf("%s%s", rec_get(o, "Name") ? rec_get(o, "Name") : about,
                           rec_get(o, "Kind") && !strcmp(rec_get(o, "Kind"), "file") ? "" : "/");
    }
    return xstrdup(about);
}
