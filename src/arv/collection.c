/* Collections: something kept over time and made into discs again and again, from one workflow
 * folder with one history (research/plan.md, "collections, editions and the workflow folder").
 *
 *   arv collection init [FOLDER] --code CODE --title TITLE [--set CODE] [--category CODE]...
 *                       [--access LEVEL] [--description TEXT]
 *   arv collection list
 *   arv collection show CODE
 *   arv collection keep CODE N        edition N is never offered for retiring
 *
 * The workflow folder's .arv marker is a pointer file with one more line, "Collection: UUID", so
 * the home is found from it as from any pointer file. arv make on a marked folder makes the
 * collection's next edition (make.c) and records it as a Revision. */
#define _XOPEN_SOURCE 700
#include "arv.h"
#include "../bagit/bagit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int is_dir(const char *p)
{
    struct stat st;
    return !stat(p, &st) && S_ISDIR(st.st_mode);
}

static int is_reg(const char *p)
{
    struct stat st;
    return !stat(p, &st) && S_ISREG(st.st_mode);
}

static const char *get_or(const rec_record *r, const char *name, const char *dflt)
{
    const char *v = rec_get(r, name);
    return v ? v : dflt;
}

/* ------------------------------------------------------------------ the marker */

char *marker_path(const char *folder)
{
    return join(folder, ".arv");
}

char *marker_collection(const char *folder)
{
    char *path = marker_path(folder);
    char *uuid = NULL;
    if (is_reg(path)) {
        rec_file f;
        int bad = 0;
        if (rec_read(path, &f, &bad)) die("cannot read %s", path);
        for (size_t i = 0; i < f.nrecords && !uuid; i++)
            if (rec_get(&f.records[i], "Collection")) uuid = xstrdup(rec_get(&f.records[i], "Collection"));
        rec_free(&f);
    }
    free(path);
    return uuid;
}

/* ------------------------------------------------------------------ records */

rec_record *archive_collection(const archive *a, const char *code_or_uuid)
{
    for (size_t i = 0; i < a->collections.n; i++) {
        rec_record *c = a->collections.v[i];
        const char *code = rec_get(c, "Code"), *uuid = rec_get(c, "Uuid");
        if ((code && !strcasecmp(code, code_or_uuid)) || (uuid && !strcmp(uuid, code_or_uuid))) return c;
    }
    return NULL;
}

/* the collection's newest revision (revisions are appended in order), or NULL */
rec_record *collection_head(const archive *a, const char *uuid)
{
    rec_record *head = NULL;
    for (size_t i = 0; i < a->revisions.n; i++)
        if (rec_get(a->revisions.v[i], "Collection") && !strcmp(rec_get(a->revisions.v[i], "Collection"), uuid))
            head = a->revisions.v[i];
    return head;
}

int collection_editions(const archive *a, const char *uuid)
{
    int n = 0;
    for (size_t i = 0; i < a->revisions.n; i++) {
        const rec_record *r = a->revisions.v[i];
        if (rec_get(r, "Collection") && !strcmp(rec_get(r, "Collection"), uuid) && rec_get(r, "Edition")) n++;
    }
    return n;
}

/* ------------------------------------------------------------------ hashes: Tree and Node */

static int by_entry(const void *a, const void *b)
{
    return strcmp((*(const entry *const *)a)->path, (*(const entry *const *)b)->path);
}

/* the revision's manifest: "SHA256  path" per file, sorted by path (bytewise) */
char *tree_manifest(const entries *files)
{
    const entry **v = xmalloc((files->n + 1) * sizeof *v);
    for (size_t i = 0; i < files->n; i++) v[i] = &files->v[i];
    qsort(v, files->n, sizeof *v, by_entry);
    sbuf b = { 0 };
    sb_puts(&b, "");
    for (size_t i = 0; i < files->n; i++) sb_printf(&b, "%s  %s\n", v[i]->sha256, v[i]->path);
    free(v);
    return b.s;
}

static void sha256_text(const char *text, char hex[65])
{
    sha256_ctx c;
    unsigned char d[32];
    sha256_init(&c);
    sha256_update(&c, text, strlen(text));
    sha256_final(&c, d);
    sha256_hex(d, hex);
}

/* Tree: SHA-256 of the manifest; Node: SHA-256 of "Tree: ..\nParent: ..\nDate: ..\nMessage: ..\n"
 * (Parent and Message lines only when there are any) */
void revision_hashes(const char *text, const char *parent, const char *date, const char *message,
                     char tree[65], char node[65])
{
    sha256_text(text, tree);
    sbuf b = { 0 };
    sb_printf(&b, "Tree: %s\n", tree);
    if (parent) sb_printf(&b, "Parent: %s\n", parent);
    sb_printf(&b, "Date: %s\n", date);
    if (message && *message) sb_printf(&b, "Message: %s\n", message);
    sha256_text(b.s, node);
    free(b.s);
}

char *revision_manifest_path(const arv_home *h, const char *node)
{
    return xprintf("%s/revisions/%s.sha256", h->catalog_dir, node);
}

/* ------------------------------------------------------------------ arv collection */

static int code_ok(const char *code)
{
    size_t n = strlen(code);           /* a disc id prefix: 2-8 capital letters or digits */
    if (n < 2 || n > 8) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isupper((unsigned char)code[i]) && !isdigit((unsigned char)code[i])) return 0;
    return 1;
}

static void list_line(const archive *cat, const rec_record *c)
{
    const char *uuid = get_or(c, "Uuid", "");
    const rec_record *head = collection_head(cat, uuid);
    int editions = collection_editions(cat, uuid);
    printf("%-8s  %s  (%d edition%s", get_or(c, "Code", ""), get_or(c, "Title", ""), editions, editions == 1 ? "" : "s");
    if (head) printf("; last %s, %s", get_or(head, "Date", ""), rec_get(head, "Edition") ? "an edition" : "a checkpoint");
    puts(")");
}

static int init(int argc, char **argv, const char *given)
{
    const char *folder = ".", *code_arg = NULL, *title = NULL, *set = NULL, *access_level = "private", *description = NULL;
    strlist categories = { 0 };
    int have_folder = 0;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && !strcmp(argv[i], "--code")) code_arg = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--title")) title = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--set")) set = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--category")) strlist_add(&categories, argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--access")) access_level = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--description")) description = argv[++i];
        else if (argv[i][0] != '-' && !have_folder) { folder = argv[i]; have_folder = 1; }
        else return 2;
    }
    if (!code_arg || !title) die("%s", "arv collection init needs --code CODE and --title TITLE");
    if (strcmp(access_level, "private") && strcmp(access_level, "public") && strcmp(access_level, "sealed"))
        die("--access %s: private, public or sealed", access_level);
    char *code = xstrdup(code_arg);
    for (char *q = code; *q; q++) *q = (char)toupper((unsigned char)*q);
    if (!code_ok(code)) die("collection code %s: 2-8 capital letters or digits (it starts each disc id)", code_arg);
    char *abs = realpath(folder, NULL);
    if (!abs || !is_dir(abs)) die("%s is not a folder", folder);
    char *mpath = marker_path(abs);
    if (is_dir(mpath)) die("%s holds a home (.arv folder); make the collection in a folder of its own", abs);
    char *existing = marker_collection(abs);
    if (existing) die("this folder is already the workflow folder of collection %s", existing);

    arv_home h;
    home_find(&h, given, abs);
    archive cat;
    archive_load(&cat, h.rec_path);
    if (archive_collection(&cat, code)) die("there is already a collection %s", code);

    char uuid[37];
    uuid4(uuid);
    rec_record *c = rec_alloc("Collection");
    rec_add(c, "Code", code);
    rec_add(c, "Uuid", uuid);
    rec_add(c, "Title", title);
    if (description) rec_add(c, "Description", description);
    if (set) {
        char *s = xstrdup(set);
        for (char *q = s; *q; q++) *q = (char)toupper((unsigned char)*q);
        rec_add(c, "Set", s);
        free(s);
    }
    for (size_t i = 0; i < categories.n; i++) {
        char *s = xstrdup(categories.v[i]);
        for (char *q = s; *q; q++) *q = (char)toupper((unsigned char)*q);
        rec_add(c, "Category", s);
        free(s);
    }
    rec_add(c, "Access", access_level);
    recs_add(&cat.collections, c);

    /* the marker: the home (as any pointer file), and the collection */
    char *home_abs = realpath(h.path, NULL);
    if (!home_abs) {
        home_ensure(&h);
        home_abs = realpath(h.path, NULL);
    }
    char *marker = xprintf("# arv: this folder is the workflow folder of collection %s (%s)\nHome: %s\nCollection: %s\n",
                           code, title, home_abs ? home_abs : h.path, uuid);
    if (is_reg(mpath)) die("%s exists (a pointer file); remove it first, the marker replaces it", mpath);
    write_text(mpath, marker);

    char *who = person(), *obj = xprintf("collection:%s", code);
    char *note = xprintf("collection %s created; workflow folder %s (marker)", code, abs);
    rec_record *e = new_event(obj, "accession", "success", who, "human", note);
    free(e->fields[0].name);
    e->fields[0].name = xstrdup("Object");
    rec_add(e, "Folder", abs);          /* as arv link records a folder: the workflow folder, now */
    rec_add(e, "How", "marker");
    rec_add(e, "State", "present");
    recs_add(&cat.events, e);
    archive_save(&cat, h.rec_path);
    printf("%s: collection %s, workflow folder %s\n", code, uuid, abs);
    free(who); free(obj); free(note); free(marker); free(home_abs); free(mpath); free(abs); free(code);
    strlist_free(&categories);
    return 0;
}

int cmd_collection(int argc, char **argv)
{
    const char *given = NULL;
    int i = 0;
    while (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) {
        given = argv[i + 1];
        i += 2;
    }
    if (i >= argc) return 2;
    const char *action = argv[i++];
    if (!strcmp(action, "init")) return init(argc - i, argv + i, given);
    if (strcmp(action, "list") && strcmp(action, "show") && strcmp(action, "keep")) return 2;
    arv_home h;
    home_find(&h, given, NULL);
    archive cat;
    archive_load(&cat, h.rec_path);
    if (!strcmp(action, "keep")) {          /* an edition that is never offered for retiring */
        if (i + 2 != argc) return 2;
        const rec_record *c = archive_collection(&cat, argv[i]);
        if (!c) die("no collection %s", argv[i]);
        rec_record *r = NULL;
        for (size_t k = 0; k < cat.revisions.n; k++)
            if (!strcmp(get_or(cat.revisions.v[k], "Collection", ""), get_or(c, "Uuid", ""))
                && !strcmp(get_or(cat.revisions.v[k], "Edition", ""), argv[i + 1]))
                r = cat.revisions.v[k];
        if (!r) die("no edition %s of that collection", argv[i + 1]);
        if (rec_get(r, "Keep")) {
            printf("%s/%s is already kept\n", get_or(c, "Code", ""), argv[i + 1]);
            return 0;
        }
        rec_add(r, "Keep", "yes");          /* not part of Node: the revision's hash is unchanged */
        char *who = person(), *obj = xprintf("collection:%s", get_or(c, "Code", "")),
             *note = xprintf("edition %s kept: never offered for retiring", argv[i + 1]);
        rec_record *e = new_event(obj, "metadata modification", "success", who, "human", note);
        free(e->fields[0].name);
        e->fields[0].name = xstrdup("Object");
        recs_add(&cat.events, e);
        archive_save(&cat, h.rec_path);
        printf("%s/%s: kept (logged)\n", get_or(c, "Code", ""), argv[i + 1]);
        free(who); free(obj); free(note);
        return 0;
    }
    if (!strcmp(action, "list")) {
        if (i != argc) return 2;
        for (size_t k = 0; k < cat.collections.n; k++) list_line(&cat, cat.collections.v[k]);
        return 0;
    }
    if (i + 1 != argc) return 2;
    const rec_record *c = archive_collection(&cat, argv[i]);
    if (!c) die("no collection %s", argv[i]);
    list_line(&cat, c);
    if (rec_get(c, "Description")) printf("  %s\n", rec_get(c, "Description"));
    printf("  uuid %s; set %s; access %s\n", get_or(c, "Uuid", ""), get_or(c, "Set", "-"), get_or(c, "Access", "private"));
    const char *uuid = get_or(c, "Uuid", "");
    for (size_t k = 0; k < cat.revisions.n; k++) {
        const rec_record *r = cat.revisions.v[k];
        if (!rec_get(r, "Collection") || strcmp(rec_get(r, "Collection"), uuid)) continue;
        sbuf vols = { 0 };
        sb_puts(&vols, "");
        for (size_t f = 0; f < r->nfields; f++)
            if (!strcmp(r->fields[f].name, "Volume")) sb_printf(&vols, "%s%s", vols.len ? " " : "", r->fields[f].value);
        char *what = rec_get(r, "Edition") ? xprintf("edition %s", rec_get(r, "Edition")) : xstrdup("checkpoint");
        printf("  %.12s  %s  %-10s  %-4s  %s  %s%s%s\n", get_or(r, "Node", ""), get_or(r, "Date", ""), what,
               rec_get(r, "Keep") ? "kept" : "", get_or(r, "Changes", ""), vols.s, rec_get(r, "Message") ? "  " : "",
               get_or(r, "Message", ""));
        free(what);
        free(vols.s);
    }
    return 0;
}
