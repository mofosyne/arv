/* Drafts (describe.load_draft): the suggestions arv describe and arv tag save as JSON, which
 * arv make --draft takes and arv describe --apply writes into the catalogue. Anything may write
 * one (a person, a script, a model elsewhere): the format is in README.md, "Describing from anywhere". */
#define _XOPEN_SOURCE 700
#include "arv.h"

#include <stdlib.h>
#include <string.h>

int is_model(const char *agent)        /* catalog.is_model: a model's judgement */
{
    return agent && (strstr(agent, "llm:") || strstr(agent, "embeddings:") || strstr(agent, "vision:"));
}

static char *jtext(const jv *v)                /* a string value; NULL when absent, empty or not a string */
{
    return v && v->kind == 's' && *v->str ? xstrdup(v->str) : NULL;
}

static void jlist(const jv *v, strlist *out)
{
    for (size_t i = 0; v && v->kind == 'a' && i < v->n; i++)
        if (v->vals[i]->kind == 's') strlist_add(out, v->vals[i]->str);
}

/* a saved draft; accept: it is applied by a person, so a model's suggestion becomes accepted */
void draft_load(const char *path, draft *d, int accept)
{
    memset(d, 0, sizeof *d);
    char *text = NULL;
    if (!strcmp(path, "-")) {                  /* - : the draft on standard input */
        sbuf b = { 0 };
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, stdin)) > 0) sb_add(&b, buf, n);
        text = b.s ? b.s : xstrdup("");
    } else text = read_text(path);
    if (!text) die("cannot read the draft %s", path);
    jv *doc = json_parse(text);
    free(text);
    if (!doc || doc->kind != 'o') die("%s is not a JSON draft (arv describe --save writes them)", path);
    d->title = jtext(json_get(doc, "title"));
    d->description = jtext(json_get(doc, "description"));
    jlist(json_get(doc, "subjects"), &d->subjects);
    jlist(json_get(doc, "notes"), &d->notes);
    const jv *ft = json_get(doc, "folder_tags"), *caps = json_get(doc, "folder_captions");
    for (size_t i = 0; ft && ft->kind == 'o' && i < ft->n; i++) {
        d->ft_folder = xrealloc(d->ft_folder, (d->nft + 1) * sizeof *d->ft_folder);
        d->ft_tags = xrealloc(d->ft_tags, (d->nft + 1) * sizeof *d->ft_tags);
        d->ft_folder[d->nft] = xstrdup(ft->keys[i]);
        memset(&d->ft_tags[d->nft], 0, sizeof *d->ft_tags);
        jlist(ft->vals[i], &d->ft_tags[d->nft]);
        d->nft++;
    }
    for (size_t i = 0; caps && caps->kind == 'o' && i < caps->n; i++) {
        if (caps->vals[i]->kind != 's') continue;
        d->cap_folder = xrealloc(d->cap_folder, (d->ncap + 1) * sizeof *d->cap_folder);
        d->cap_text = xrealloc(d->cap_text, (d->ncap + 1) * sizeof *d->cap_text);
        d->cap_folder[d->ncap] = xstrdup(caps->keys[i]);
        d->cap_text[d->ncap++] = xstrdup(caps->vals[i]->str);
    }
    d->agent = jtext(json_get(doc, "agent"));
    if (!d->agent) d->agent = xstrdup("draft");
    static const char *const kinds[] = { "automatic", "suggested", "accepted", "edited", "human", NULL };
    const jv *how = json_get(doc, "authorship");
    for (int k = 0; kinds[k] && how && how->kind == 's'; k++)
        if (!strcmp(how->str, kinds[k])) d->authorship = xstrdup(kinds[k]);
    if (!d->authorship) d->authorship = xstrdup(is_model(d->agent) ? "suggested" : "human");
    if (accept && !strcmp(d->authorship, "suggested")) {          /* describe.accept_draft */
        free(d->authorship);
        d->authorship = xstrdup("accepted");
    }
    jfree(doc);
}

/* a metadata modification event, and who saw it (catalog.reviewed_agents): a model's suggestion that
 * a person accepted or edited names that person too */
rec_record *reviewed_event(const char *disc_id, const char *agent, const char *how, const char *note)
{
    rec_record *e = new_event(disc_id, "metadata modification", "success", agent, how, NULL);
    if ((!strcmp(how, "accepted") || !strcmp(how, "edited")) && strncmp(agent, "human:", 6)) {
        char *who = person();
        rec_add(e, "Agent", who);
        free(who);
    }
    if (note) rec_add(e, "Note", note);
    return e;
}
