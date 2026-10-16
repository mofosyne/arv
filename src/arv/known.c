/*
 * Known names: authority control for one person's archive (research/plan.md, "Known names, as
 * authority control"). An entry is a namespaced Tag record of the vocabulary (config/tags.rec):
 * its key (Name, made once and kept), its preferred form now (Label), the other forms it is found
 * as (Alias), what tells two of one name apart (Qualifier), entries it relates to (Related), the
 * same entity elsewhere (Same), why it was made (Basis), whether it stays home (Private) and
 * whether it is withdrawn (Retired). Changes to entries are events about name:KEY; a change to
 * where an entry is used is an ordinary tag change, an event with Sets and Replaces.
 *
 *   arv tags show KEY
 *   arv tags add KEY [--label L] [--alias A]... [--qualifier Q] [--same ID]... [--private|--public]
 *   arv tags label KEY LABEL          (the old label becomes an alias)
 *   arv tags alias KEY FORM
 *   arv tags merge KEY INTO           (every use of KEY becomes INTO; KEY is retired, an alias of INTO)
 *   arv tags split KEY [--new KEY2] [--qualifier Q] [--new-qualifier Q2]   (each use asked about)
 *   arv tags retire KEY [--why TEXT]
 */
#define _XOPEN_SOURCE 700
#include "arv.h"
#include "data.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *vocab_file(const arv_home *h)
{
    char *p = join(h->config_dir, "tags.rec");
    if (access(p, F_OK) && !mkdirs(h->config_dir)) write_text(p, DATA_DEFAULT_TAGS);
    return p;
}

/* the namespaces whose entries stay home by default: people's names are personal data */
static int private_by_default(const char *key)
{
    return !strncmp(key, "person:", 7) || !strncmp(key, "family:", 7);
}

size_t known_load(const arv_home *h, known_entry **out)
{
    *out = NULL;
    char *p = vocab_file(h);
    rec_file f;
    int bad = 0;
    size_t n = 0;
    if (!rec_read(p, &f, &bad)) {
        for (size_t i = 0; i < f.nrecords; i++) {
            const rec_record *r = &f.records[i];
            const char *name = rec_get(r, "Name");
            if (r->descriptor || !name || !strchr(name, ':') || (r->type && strcmp(r->type, "Tag"))) continue;
            *out = xrealloc(*out, (n + 1) * sizeof **out);
            known_entry *e = &(*out)[n++];
            memset(e, 0, sizeof *e);
            e->key = xstrdup(name);
            for (size_t j = 0; j < r->nfields; j++) {
                const char *fn = r->fields[j].name, *v = r->fields[j].value;
                if (!strcmp(fn, "Label") && !e->label) e->label = xstrdup(v);
                else if (!strcmp(fn, "Alias")) strlist_add(&e->aliases, v);
                else if (!strcmp(fn, "Qualifier") && !e->qualifier) e->qualifier = xstrdup(v);
                else if (!strcmp(fn, "Related")) strlist_add(&e->related, v);
                else if (!strcmp(fn, "Same")) strlist_add(&e->same, v);
                else if (!strcmp(fn, "Basis") && !e->basis) e->basis = xstrdup(v);
                else if (!strcmp(fn, "Private")) e->private_ = !strcmp(v, "yes");
                else if (!strcmp(fn, "Retired")) e->retired = xstrdup(v);
            }
        }
        rec_free(&f);
    }
    free(p);
    return n;
}

void known_free(known_entry *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(v[i].key);
        free(v[i].label);
        free(v[i].qualifier);
        free(v[i].basis);
        free(v[i].retired);
        strlist_free(&v[i].aliases);
        strlist_free(&v[i].related);
        strlist_free(&v[i].same);
    }
    free(v);
}

/* "anne-smith" -> "Anne Smith": a label for a key that has none yet */
char *known_label_of(const char *key)
{
    const char *v = strchr(key, ':') ? strchr(key, ':') + 1 : key;
    char *s = xstrdup(v);
    for (char *c = s; *c; c++) {
        if (*c == '-' || *c == '_') *c = ' ';
        if (c == s || c[-1] == ' ') *c = (char)toupper((unsigned char)*c);
    }
    return s;
}

/* ------------------------------------------------------------------ editing tags.rec, comments kept */

/* the record whose Name is key: its Name line at *start, its end (a blank line, %rec or the end) */
static int block_of(const char *text, const char *key, size_t *start, size_t *end)
{
    char *line = xprintf("Name: %s\n", key);
    const char *at = NULL;
    for (const char *s = text; (s = strstr(s, line)); s++)
        if (s == text || s[-1] == '\n') {
            at = s;
            break;
        }
    free(line);
    if (!at) return 0;
    *start = (size_t)(at - text);
    const char *e = at;
    while (*e) {
        const char *nl = strchr(e, '\n');
        if (!nl) {
            e += strlen(e);
            break;
        }
        e = nl + 1;
        if (*e == '\n' || !strncmp(e, "%rec:", 5)) break;
    }
    *end = (size_t)(e - text);
    return 1;
}

static int save_vocab(const char *path, const char *text)
{
    char *tmp = xprintf("%s.tmp", path);
    write_text(tmp, text);
    int rc = rename(tmp, path);
    free(tmp);
    return rc;
}

/* a field of an entry: set (the first such line replaced, or one added), or added (once); a new
 * entry, before the Namespace records, when there is none */
int known_field(const arv_home *h, const char *key, const char *field, const char *value, int add)
{
    char *p = vocab_file(h), *text = read_text(p);
    if (!text) {
        free(p);
        return -1;
    }
    size_t start, end;
    sbuf b = { 0 };
    char *line = xprintf("%s: %s\n", field, value);
    for (char *c = line; c[1]; c++)
        if (*c == '\n') *c = ' ';               /* one line a field */
    if (!block_of(text, key, &start, &end)) {
        char *ns = strstr(text, "\n%rec: Namespace");
        size_t cut = ns ? (size_t)(ns - text) + 1 : strlen(text);
        sb_add(&b, text, cut);
        if (b.len && b.s[b.len - 1] != '\n') sb_puts(&b, "\n");
        if (b.len < 2 || b.s[b.len - 2] != '\n') sb_puts(&b, "\n");
        sb_printf(&b, "Name: %s\n%s", key, line);
        if (ns) sb_puts(&b, "\n");
        sb_puts(&b, text + cut);
    } else {
        char *block = xprintf("%.*s", (int)(end - start), text + start), *head = xprintf("%s: ", field);
        char *same = strstr(block, line), *at = NULL;
        for (char *s = block; (s = strstr(s, head)); s++)
            if (s == block || s[-1] == '\n') {
                at = s;
                break;
            }
        sb_add(&b, text, start);
        if (add && same && (same == block || same[-1] == '\n')) {
            sb_puts(&b, block);             /* there already */
        } else if (!add && at) {
            char *nl = strchr(at, '\n');
            sb_add(&b, block, (size_t)(at - block));
            sb_puts(&b, line);
            sb_puts(&b, nl ? nl + 1 : "");
        } else {
            sb_puts(&b, block);
            if (b.len && b.s[b.len - 1] != '\n') sb_puts(&b, "\n");
            sb_puts(&b, line);
        }
        sb_puts(&b, text + end);
        free(block);
        free(head);
    }
    int rc = save_vocab(p, b.s);
    free(line);
    free(b.s);
    free(text);
    free(p);
    return rc;
}

/* the vocabulary's text without the entries kept home (Private: yes), for a disc anyone may read */
char *known_strip_private(const char *text)
{
    sbuf b = { 0 };
    sb_puts(&b, "");
    for (const char *s = text; *s;) {          /* record by record: blank-line separated */
        const char *e = strstr(s, "\n\n");
        size_t n = e ? (size_t)(e - s) + 2 : strlen(s);
        char *rec = xprintf("%.*s", (int)n, s);
        int keep = 1;
        for (const char *q = rec; (q = strstr(q, "Private: yes")); q++)
            if ((q == rec || q[-1] == '\n') && (q[12] == '\n' || !q[12])) keep = 0;
        if (keep) sb_puts(&b, rec);
        free(rec);
        s += n;
    }
    return b.s;
}

/* ------------------------------------------------------------------ where an entry is used */

typedef struct {
    strlist discs, paths, tags;     /* a disc's path, and its tags now */
    strlist objects, otags;         /* a data object with tags of its own, and them */
} uses;

static int tags_have(const char *tags, const char *key)
{
    size_t n = strlen(key);
    for (const char *t = tags; (t = strstr(t, key)); t++)
        if ((t == tags || t[-1] == ' ' || t[-1] == ',') && (t[n] == ',' || !t[n])) return 1;
    return 0;
}

static void uses_of(const arv_home *h, const archive *cat, const char *key, uses *u)
{
    memset(u, 0, sizeof *u);
    for (size_t i = 0; i < cat->discs.n; i++) {
        const char *id = rec_get(cat->discs.v[i], "Id");
        if (!id) continue;
        char *tf = home_volume_file(h, id, "tags.tsv"), *text = disc_tags_text(cat, id, access(tf, F_OK) ? NULL : tf);
        free(tf);
        for (char *line = text, *next; line && *line; line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            char *tab = line[0] != '#' ? strchr(line, '\t') : NULL;
            if (!tab) continue;
            *tab = 0;
            char *end = strchr(tab + 1, '\t');
            if (end) *end = 0;
            if (!tags_have(tab + 1, key)) continue;
            const rec_record *o = object_at(cat, id, line);
            if (o && rec_get(o, "Uuid")) {      /* a data object's own path: the object's tags (as describe --apply) */
                const char *uuid = rec_get(o, "Uuid"), *own = object_field(cat, uuid, "Tags");
                if (!strlist_has(&u->objects, uuid)) {
                    strlist_add(&u->objects, uuid);
                    strlist_add(&u->otags, own ? own : tab + 1);
                }
                continue;
            }
            strlist_add(&u->discs, id);
            strlist_add(&u->paths, line);
            strlist_add(&u->tags, tab + 1);
        }
        free(text);
    }
    for (size_t i = 0; i < cat->objects.n; i++) {
        const char *uuid = rec_get(cat->objects.v[i], "Uuid"), *t = uuid ? object_field(cat, uuid, "Tags") : NULL;
        if (!t || strlist_has(&u->objects, uuid) || !tags_have(t, key)) continue;
        strlist_add(&u->objects, uuid);
        strlist_add(&u->otags, t);
    }
}

static void uses_free(uses *u)
{
    strlist_free(&u->discs);
    strlist_free(&u->paths);
    strlist_free(&u->tags);
    strlist_free(&u->objects);
    strlist_free(&u->otags);
}

/* tags with one replaced by another (none: removed), each once */
static char *tags_with(const char *tags, const char *from, const char *to)
{
    strlist out = { 0 };
    char *copy = xstrdup(tags);
    for (char *t = strtok(copy, ","); t; t = strtok(NULL, ",")) {
        while (*t == ' ') t++;
        char *e = t + strlen(t);
        while (e > t && e[-1] == ' ') *--e = 0;
        const char *v = !strcmp(t, from) ? to : t;
        if (v && *v && !strlist_has(&out, v)) strlist_add(&out, v);
    }
    free(copy);
    sbuf b = { 0 };
    sb_puts(&b, "");
    for (size_t i = 0; i < out.n; i++) sb_printf(&b, "%s%s", i ? ", " : "", out.v[i]);
    strlist_free(&out);
    return b.s;
}

/* ------------------------------------------------------------------ events */

static void name_event(archive *cat, const char *key, const char *note, const char *field, const char *value)
{
    char *who = person(), *thing = xprintf("name:%s", key);
    rec_record *e = new_event(key, "metadata modification", "success", who, "human", note);
    free(e->fields[0].name);                /* Object: name:KEY, not Disc */
    free(e->fields[0].value);
    e->fields[0].name = xstrdup("Object");
    e->fields[0].value = xstrdup(thing);
    if (field) {
        rec_add(e, field, value);
        sets_field(cat, e, thing, field);
    }
    recs_add(&cat->events, e);
    free(who);
    free(thing);
}

/* one use moved from one entry to another: a tag change, as arv retag or describe --apply makes */
static void move_use(archive *cat, const uses *u, size_t i, int object, const char *from, const char *to, const char *note)
{
    char *who = person();
    if (object) {
        char *now = tags_with(u->otags.v[i], from, to), *thing = xprintf("object:%s", u->objects.v[i]);
        rec_record *e = new_event(u->objects.v[i], "metadata modification", "success", who, "human", note);
        free(e->fields[0].name);
        free(e->fields[0].value);
        e->fields[0].name = xstrdup("Object");
        e->fields[0].value = xstrdup(thing);
        rec_add(e, "Tags", now);
        sets_field(cat, e, thing, "Tags");
        recs_add(&cat->events, e);
        free(now);
        free(thing);
    } else {
        char *now = tags_with(u->tags.v[i], from, to);
        rec_record *e = new_event(u->discs.v[i], "metadata modification", "success", who, "human", note);
        tag_change(cat, e, u->discs.v[i], "Tags", u->paths.v[i], now);
        recs_add(&cat->events, e);
        free(now);
    }
    free(who);
}

/* an entry made from an answer (arv describe --ask): its label, basis and, for people and families,
 * Private; the event goes into cat, for the caller to save */
void known_made(const arv_home *h, archive *cat, const char *key, const char *label, const char *basis)
{
    known_field(h, key, "Label", label, 0);
    known_field(h, key, "Basis", basis, 0);
    if (private_by_default(key)) known_field(h, key, "Private", "yes", 0);
    name_event(cat, key, basis, "Label", label);
}

/* ------------------------------------------------------------------ the commands */

static const known_entry *entry_of(const known_entry *v, size_t n, const char *key)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(v[i].key, key)) return &v[i];
    return NULL;
}

static char *line_in(const char *prompt)
{
    fputs(prompt, stderr);
    fflush(stderr);
    char *line = NULL;
    size_t cap = 0;
    ssize_t r = getline(&line, &cap, stdin);
    if (r < 0) {
        free(line);
        return xstrdup("");
    }
    while (r > 0 && (line[r - 1] == '\n' || line[r - 1] == '\r' || line[r - 1] == ' ')) line[--r] = 0;
    return line;
}

static void show_entry(const archive *cat, const known_entry *e, const uses *u)
{
    printf("%s  %s%s%s%s\n", e->key, e->label ? e->label : "(no label)", e->qualifier ? " (" : "", e->qualifier ? e->qualifier : "",
           e->qualifier ? ")" : "");
    for (size_t i = 0; i < e->aliases.n; i++) printf("  also: %s\n", e->aliases.v[i]);
    for (size_t i = 0; i < e->related.n; i++) printf("  related: %s\n", e->related.v[i]);
    for (size_t i = 0; i < e->same.n; i++) printf("  same as: %s\n", e->same.v[i]);
    if (e->basis) printf("  basis: %s\n", e->basis);
    printf("  %s\n", e->private_ ? "private: stays home, never on a public disc" : "on discs with the vocabulary");
    if (e->retired) printf("  RETIRED: %s\n", e->retired);
    printf("  used on %zu path%s and %zu data object%s\n", u->paths.n, u->paths.n == 1 ? "" : "s", u->objects.n,
           u->objects.n == 1 ? "" : "s");
    for (size_t i = 0; i < u->paths.n && i < 20; i++) printf("    %s  data/%s\n", u->discs.v[i], u->paths.v[i]);
    for (size_t i = 0; i < u->objects.n && i < 20; i++)
        printf("    object %s\n", object_slug(cat, object_at_uuid(cat, u->objects.v[i])));
}

static int known_ok(const char *key)
{
    const char *c = strchr(key, ':');
    if (!c || c == key || !c[1]) return 0;
    for (const char *s = key; *s; s++)
        if (isspace((unsigned char)*s) || *s == ',') return 0;
    return 1;
}

int known_cmd(const char *sub, int argc, char **argv, const char *given)
{
    const char *label = NULL, *qual = NULL, *new_key = NULL, *new_qual = NULL, *why = NULL;
    strlist words = { 0 }, aliases = { 0 }, same = { 0 };
    int priv = -1;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (v && !strcmp(a, "--label")) label = argv[++i];
        else if (v && !strcmp(a, "--alias")) strlist_add(&aliases, argv[++i]);
        else if (v && !strcmp(a, "--qualifier")) qual = argv[++i];
        else if (v && !strcmp(a, "--same")) strlist_add(&same, argv[++i]);
        else if (v && !strcmp(a, "--new")) new_key = argv[++i];
        else if (v && !strcmp(a, "--new-qualifier")) new_qual = argv[++i];
        else if (v && !strcmp(a, "--why")) why = argv[++i];
        else if (!strcmp(a, "--private")) priv = 1;
        else if (!strcmp(a, "--public")) priv = 0;
        else if (a[0] == '-') return 2;
        else strlist_add(&words, a);
    }
    if (!words.n) return 2;
    char *key = tag_normalise(words.v[0]);
    if (!known_ok(key)) die("%s: an entry's key is a namespaced tag, NAMESPACE:value (person:anne-smith, software:kicad)", words.v[0]);
    arv_home h;
    archive cat;
    home_find(&h, given, NULL);
    archive_load(&cat, h.rec_path);
    known_entry *v;
    size_t n = known_load(&h, &v);
    const known_entry *e = entry_of(v, n, key);
    char today[11], *who = person();
    today_iso(today);
    uses u;
    uses_of(&h, &cat, key, &u);
    int rc = 0, saved = 0;
    if (!strcmp(sub, "show")) {
        if (!e && !u.paths.n && !u.objects.n) die("no entry %s, and no tag of it in use (arv tags lists them)", key);
        known_entry blank = { 0 };
        blank.key = key;
        show_entry(&cat, e ? e : &blank, &u);
        if (!e) printf("  not an entry yet: arv tags add %s --label \"...\"\n", key);
    } else if (!strcmp(sub, "add")) {
        if (words.n > 1) return 2;
        if (!e) {
            char *lb = label ? xstrdup(label) : known_label_of(key), *basis = xprintf("made by %s with arv tags add, %s", who, today);
            known_field(&h, key, "Label", lb, 0);
            known_field(&h, key, "Basis", basis, 0);
            if (priv == 1 || (priv < 0 && private_by_default(key))) known_field(&h, key, "Private", "yes", 0);
            name_event(&cat, key, "an entry of the known names, made", "Label", lb);
            printf("%s: an entry now, %s%s\n", key, lb, priv == 1 || (priv < 0 && private_by_default(key)) ? " (private: stays home)" : "");
            free(lb);
            free(basis);
        } else if (label) {
            known_field(&h, key, "Label", label, 0);
            name_event(&cat, key, "its label", "Label", label);
        }
        if (e && priv >= 0) known_field(&h, key, "Private", priv ? "yes" : "no", 0);
        for (size_t i = 0; i < aliases.n; i++) known_field(&h, key, "Alias", aliases.v[i], 1);
        for (size_t i = 0; i < same.n; i++) known_field(&h, key, "Same", same.v[i], 1);
        if (qual) known_field(&h, key, "Qualifier", qual, 0);
        if (e) printf("%s: updated\n", key);
        saved = 1;
    } else if (!strcmp(sub, "label")) {
        if (words.n != 2) return 2;
        if (e && e->label && strcmp(e->label, words.v[1])) known_field(&h, key, "Alias", e->label, 1);   /* the old form: a variant */
        known_field(&h, key, "Label", words.v[1], 0);
        name_event(&cat, key, e && e->label ? "its preferred form, the old one kept as an alias" : "its label", "Label", words.v[1]);
        if (e && e->label) printf("%s: %s (was %s, kept as an alias)\n", key, words.v[1], e->label);
        else printf("%s: %s\n", key, words.v[1]);
        saved = 1;
    } else if (!strcmp(sub, "alias")) {
        if (words.n != 2) return 2;
        known_field(&h, key, "Alias", words.v[1], 1);
        name_event(&cat, key, "another form it is found as", "Alias", words.v[1]);
        printf("%s: also found as %s\n", key, words.v[1]);
        saved = 1;
    } else if (!strcmp(sub, "retire")) {
        if (words.n != 1) return 2;
        if (!e) die2("no entry %s (arv tags show %s)", key, key);
        char *note = xprintf("retired: %s", why ? why : "by a person");
        known_field(&h, key, "Retired", why ? why : "yes", 0);
        name_event(&cat, key, note, "Retired", why ? why : "yes");
        printf("%s: retired, kept (no longer offered; its %zu uses keep it)\n", key, u.paths.n + u.objects.n);
        free(note);
        saved = 1;
    } else if (!strcmp(sub, "merge")) {
        if (words.n != 2) return 2;
        char *into = tag_normalise(words.v[1]);
        if (!known_ok(into) || !strcmp(into, key)) die("%s: merge an entry into another one (arv tags merge KEY INTO)", words.v[1]);
        const known_entry *t = entry_of(v, n, into);
        if (!t) {
            char *lb = known_label_of(into), *basis = xprintf("made by %s merging %s, %s", who, key, today);
            known_field(&h, into, "Label", lb, 0);
            known_field(&h, into, "Basis", basis, 0);
            if (private_by_default(into)) known_field(&h, into, "Private", "yes", 0);
            free(lb);
            free(basis);
        }
        char *note = xprintf("%s merged into %s", key, into);
        for (size_t i = 0; i < u.paths.n; i++) move_use(&cat, &u, i, 0, key, into, note);
        for (size_t i = 0; i < u.objects.n; i++) move_use(&cat, &u, i, 1, key, into, note);
        known_field(&h, into, "Alias", key, 1);   /* typed or found later, it means the entry merged into */
        if (e && e->label) known_field(&h, into, "Alias", e->label, 1);
        for (size_t i = 0; e && i < e->aliases.n; i++) known_field(&h, into, "Alias", e->aliases.v[i], 1);
        char *ret = xprintf("merged into %s", into);
        known_field(&h, key, "Retired", ret, 0);
        name_event(&cat, key, note, "Retired", ret);
        name_event(&cat, into, note, NULL, NULL);
        printf("%s merged into %s: %zu use%s moved (an event each); %s kept, retired, and an alias of %s\n", key, into,
               u.paths.n + u.objects.n, u.paths.n + u.objects.n == 1 ? "" : "s", key, into);
        free(ret);
        free(note);
        free(into);
        saved = 1;
    } else if (!strcmp(sub, "split")) {
        if (words.n != 1) return 2;
        if (!e && !u.paths.n && !u.objects.n) die("no entry %s, and no tag of it in use", key);
        char *two = new_key ? tag_normalise(new_key) : NULL;
        for (int k = 2; !two; k++) {        /* person:anne-smith-2, or the next free */
            char *c = xprintf("%s-%d", key, k);
            if (!entry_of(v, n, c)) two = c;
            else free(c);
        }
        if (!known_ok(two) || entry_of(v, n, two)) die("%s: the new entry's key must be a new namespaced tag", two);
        char *lb = e && e->label ? xstrdup(e->label) : known_label_of(key);
        char *q1 = qual ? xstrdup(qual) : NULL, *q2 = new_qual ? xstrdup(new_qual) : NULL;
        if (!q1) q1 = line_in("What tells this one apart (its Qualifier: dates, a relation, a place)? ");
        if (!q2) q2 = line_in("And the other one? ");
        if (!*q1 || !*q2) die("%s", "two of one name need a qualifier each, to tell them apart");
        char *basis = xprintf("split from %s by %s, %s", key, who, today);
        known_field(&h, key, "Qualifier", q1, 0);
        known_field(&h, two, "Label", lb, 0);
        known_field(&h, two, "Qualifier", q2, 0);
        known_field(&h, two, "Basis", basis, 0);
        if ((e && e->private_) || (!e && private_by_default(two))) known_field(&h, two, "Private", "yes", 0);
        char *note = xprintf("%s split: this use is %s (%s)", key, two, q2);
        size_t moved = 0, asked = 0, total = u.paths.n + u.objects.n;
        fprintf(stderr, "\n%zu use%s of %s (%s). For each: 1 %s (%s), 2 %s (%s); Enter keeps it 1, '.' stops (the rest stay 1)\n",
                total, total == 1 ? "" : "s", key, lb, key, q1, two, q2);
        int stop = 0;
        for (size_t i = 0; i < total && !stop; i++) {
            int object = i >= u.paths.n;
            size_t k = object ? i - u.paths.n : i;
            if (object) fprintf(stderr, "\nobject %s, tags %s\n", object_slug(&cat, object_at_uuid(&cat, u.objects.v[k])), u.otags.v[k]);
            else fprintf(stderr, "\n%s  data/%s, tags %s\n", u.discs.v[k], u.paths.v[k], u.tags.v[k]);
            char *a = line_in("> ");
            asked++;
            if (!strcmp(a, ".")) stop = 1;
            else if (!strcmp(a, "2")) {
                move_use(&cat, &u, k, object, key, two, note);
                moved++;
            }
            free(a);
        }
        name_event(&cat, key, note, "Qualifier", q1);
        name_event(&cat, two, basis, "Label", lb);
        printf("%s split: %s (%s) and %s (%s); %zu of %zu use%s moved to %s (an event each), %zu not asked about\n", key, key, q1,
               two, q2, moved, total, total == 1 ? "" : "s", two, total - asked);
        printf("Two entries share the form %s now: describe --ask asks which one, never takes it alone.\n", lb);
        free(note);
        free(basis);
        free(lb);
        free(q1);
        free(q2);
        free(two);
        saved = 1;
    } else {
        rc = 2;
    }
    if (saved) archive_save(&cat, h.rec_path);
    uses_free(&u);
    known_free(v, n);
    strlist_free(&words);
    strlist_free(&aliases);
    strlist_free(&same);
    free(who);
    free(key);
    return rc;
}
