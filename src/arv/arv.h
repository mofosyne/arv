/* arv: declarations shared by its modules (see arv.c). */
#ifndef ARV_H
#define ARV_H

#include "discid.h"
#include "edtf.h"
#include "rec.h"
#include "../bagit/bagit.h"
#include "vocab.h"

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

/* a growable string */
typedef struct {
    char *s;
    size_t len, cap;
} sbuf;

void sb_add(sbuf *b, const char *s, size_t n);
void sb_puts(sbuf *b, const char *s);
void sb_printf(sbuf *b, const char *fmt, ...);
char *xprintf(const char *fmt, ...);
unsigned long utf8_next(const char **s);
size_t utf8_chars(const char *s);
struct tm;
void today_tm(struct tm *tm);
void today_iso(char out[11]);
void uuid4(char out[37]);
int mkdirs(const char *path);
void write_text(const char *path, const char *text);
char *read_text(const char *path);
void copy_file(const char *from, const char *to);
void copy_tree(const char *from, const char *to, const char *const *skip);
void remove_tree(const char *path);
int run(char *const argv[], char **output);
int on_path(const char *program);
extern const char *arv_argv0;
char *exe_dir(void);
char *abs_path(const char *p);

/* home.c: where the home catalogue is */
typedef struct {
    char *path, *catalog_dir, *config_dir, *drafts_dir, *cache_dir, *rec_path;
    char *how;              /* how it was found, in the Python arv's words */
} arv_home;
void home_find(arv_home *h, const char *given, const char *source);   /* stops, saying how to make one, when none */
int home_try(arv_home *h, const char *given, const char *source);     /* -1 when there is none */
extern const char NO_HOME[];                                            /* what home_find says then */
extern const char *home_archive_name;   /* --archive NAME: a home from the machine config */
void home_at(arv_home *h, const char *path);
void home_ensure(const arv_home *h);
char *home_volume_file(const arv_home *h, const char *disc_id, const char *name);
void layout_notes(const arv_home *h, const char *abs, FILE *out, const char *prefix);   /* status.c */
char *home_root(const arv_home *h);                     /* the folder holding the home */
extern const char *const HOME_VOCABULARIES[];           /* config/'s files that discs carry: sets.rec, tags.rec */
char *path_rel(const char *root, const char *abs);      /* relative to root when inside it, else as it is */
char *path_abs(const char *root, const char *stored);   /* back to absolute */
int cmd_init(int argc, char **argv);
int cmd_tags(int argc, char **argv);
void tags_canonical(const arv_home *h, strlist *tags);
int cmd_keywords(int argc, char **argv);
int cmd_make(int argc, char **argv);
long medium_budget(const char *medium, const char **label);
long file_sectors(uint64_t size, const char *path);

int cmd_check(int argc, char **argv);
int cmd_burned(int argc, char **argv);
int cmd_note(int argc, char **argv);
int cmd_locate(int argc, char **argv);
int cmd_access(int argc, char **argv);
int cmd_location(int argc, char **argv);
int cmd_selection(int argc, char **argv);
int cmd_collection(int argc, char **argv);
int cmd_status(int argc, char **argv);
int cmd_checkpoint(int argc, char **argv);
int cmd_log(int argc, char **argv);
int cmd_diff(int argc, char **argv);
int cmd_link(int argc, char **argv);
int cmd_todo(int argc, char **argv);
int cmd_retire(int argc, char **argv);
int cmd_stored(int argc, char **argv);
int cmd_appraise(int argc, char **argv);
int cmd_sets(int argc, char **argv);
int cmd_names(int argc, char **argv);
int cmd_where(int argc, char **argv);
int cmd_rebuild(int argc, char **argv);

/* archive.c: the catalogue as arv writes it */
typedef struct {
    rec_record **v;
    size_t n;
} recs;
typedef struct {
    rec_file file;          /* what was read; new records are allocated one by one */
    recs homes, discs, bindings, locations, selections, collections, revisions, objects, events, appraisals;
} archive;
void recs_add(recs *l, rec_record *r);
int recs_has(const recs *l, const rec_record *r);
rec_record *rec_alloc(const char *type);
rec_record *descriptor(const char *type);
void archive_load(archive *a, const char *path);
void archive_records(const archive *a, recs *out);
void archive_save(archive *a, const char *path);
const char *archive_home_uuid(archive *a);           /* this home's identity, made on first use */
void write_records(const char *path, const recs *r);
rec_record *archive_disc(const archive *a, const char *id);
rec_record *archive_location(const archive *a, const char *code);
long archive_next_number(const archive *a, const char *set);
const char *disc_access(const rec_record *d);
void archive_locations_for(const archive *a, const recs *discs, recs *out);
void archive_selections_for(const archive *a, const strlist *disc_ids, recs *out);
/* "selection:CODE" -> CODE; else NULL */
const char *selection_target(const char *target);
rec_record *sealed_view(const rec_record *d);
void archive_shared_subset(const archive *a, const strlist *ids, archive *out);
rec_record *object_disc_view(const rec_record *o);
char *archive_where(const archive *a, const rec_record *d);
rec_record *new_event(const char *disc_id, const char *type, const char *outcome, const char *agent,
                      const char *authorship, const char *note);
char *person(void);
char *place(const archive *a, const char *text);
void place_check(const archive *a, const char *text);
/* hot, warm or cold: the place's Temperature, or the nearest place above it that has one; NULL */
const char *place_temperature(const archive *a, const char *code);
int temperature_ok(const char *t);
rec_record *new_appraisal(const char *target, const strlist *importance, const char *basis, const char *review);
int disc_importance(const archive *cat, const char *id, char **why);
size_t archive_merge(archive *home, const archive *other, int prefer_other, strlist *added, strlist *updated);

/* bag.c: the payload, BagIt tag files and the listing */
typedef struct {
    char *path;             /* relative to data/ */
    char *kind;             /* listing words: "file", "file executable", "link copied", ... */
    char *link;             /* a link's target as written ("" for plain files) */
    char *source;           /* where the bytes are read from */
    uint64_t size;
    time_t mtime;
    int via_folder;         /* reached through a copied folder link */
    int bin;                /* arv make --plan: which disc of the plan (0 = the first) */
    char sha256[65], sha512[129];
} entry;
typedef struct {
    entry *v;
    size_t n;
} entries;

/* plan.c: disc plans, discs composed by hand from files and folders anywhere (arv plan) */
typedef struct {
    int disc;               /* 1, 2, ... */
    char *source;           /* absolute: a file or folder, read when the discs are made */
    char *path;             /* where it goes under data/ ("." : a folder's contents at the top) */
    char *origin;           /* --copy: where it was copied from (source is then the plan's copy); else NULL */
    char *seen;             /* "FILES BYTES STAMP" when planned (names, sizes, dates), or NULL */
} plan_item;
typedef struct {
    char *file, *name;
    char *root;             /* sources are kept relative to it: the home's root, or the plan's folder */
    int discs;
    plan_item *v;
    size_t n;
    rec_file rec;           /* the plan as read: the Plan record gives make's defaults */
} disc_plan;
int cmd_plan(int argc, char **argv);
void plan_load(const char *file, disc_plan *out);
void plan_scan(const disc_plan *p, const char *links, entries *files, entries *noted, size_t *left_out);
void plan_made(const char *file, const strlist *disc_ids, const char *date);
void text_sha256(const char *text, char hex[65]);
void json_str(sbuf *b, const char *s);     /* a JSON string, quoted */
void plan_objects(const disc_plan *dp, int disc, const entries *files, const archive *cat, const recs *made_now,
                  const char *disc_id, const char *today, recs *out, strlist *manifests);

/* collection.c: collections kept over time, their workflow folders and revisions */
char *marker_path(const char *folder);
char *marker_collection(const char *folder);            /* the Uuid the folder's .arv marker names, or NULL */
rec_record *archive_collection(const archive *a, const char *code_or_uuid);
rec_record *collection_head(const archive *a, const char *uuid);
int collection_editions(const archive *a, const char *uuid);
char *tree_manifest(const entries *files);
void revision_hashes(const char *text, const char *parent, const char *date, const char *message,
                     char tree[65], char node[65]);
char *revision_manifest_path(const arv_home *h, const char *node);
char *manifest_changes(const char *before, const char *after);
rec_record *folder_collection(const archive *cat, const char *abs, const char **how);
const char *collection_folder(const archive *cat, const rec_record *c);   /* its workflow folder now, or NULL */
int cmd_objects(int argc, char **argv);
void hash_cache_note(const arv_home *h, const entries *files);   /* after arv make */

/* gitrepo.c: git repositories in a folder being archived */
typedef struct {
    char *path;             /* the repository's folder, relative to the source ("" for the source itself) */
    char *tsv;              /* its rows of git.tsv (NULL: git not on PATH) */
    char *note;             /* for the ingestion event */
} gitrepo;
typedef struct {
    gitrepo *v;
    size_t n;
} gitrepos;
void git_prepare(const char *src, const char *workdir, const char *since, entries *files, gitrepos *out);
char *git_tsv_for(const gitrepos *g, const entries *disc_files);
int git_internal(const char *path);
const char *disc_read_back(const archive *cat, const char *id);   /* upkeep.c: a copy known good */
int edition_safe(const archive *cat, const rec_record *rev);

/* formats.c: Siegfried (PRONOM) format identification */
typedef struct {
    char *header;           /* `sf -version`, each line as a "# " comment */
    char *(*rows)[7];       /* path, puid, format, version, mime, basis, warning */
    size_t n;
} formats;
int formats_identify(const char *src, const char *sf_home, const char *workdir, formats *out, char **error);
void formats_write(const char *path, const formats *f, const entries *files);
size_t formats_unknown(const formats *f, const entries *files);
char *formats_agent(const formats *f);
void formats_free(formats *f);

/* json.c: a small JSON tree */
typedef struct jv {
    char kind;                  /* 'o' object, 'a' array, 's' string, 't' number or boolean, 'n' null */
    char *str;
    char **keys;                /* objects: in insertion order, as Python dicts keep them */
    struct jv **vals;
    size_t n;
} jv;
jv *jstr(const char *s);
jv *jobj(void);
jv *jarr(void);
void jput(jv *o, const char *key, jv *val);
void jpush(jv *a, jv *val);
jv *jref(const char *id);
void jfree(jv *v);
void jdump(sbuf *b, const jv *v, int depth);
jv *json_parse(const char *text);
jv *json_get(const jv *o, const char *key);

/* draft.c: the JSON drafts arv describe and arv tag write */
typedef struct {
    char *title, *description, *agent, *authorship;
    strlist subjects, notes;
    size_t nft, ncap;
    char **ft_folder, **cap_folder, **cap_text;
    strlist *ft_tags;
} draft;
int is_model(const char *agent);
void draft_load(const char *path, draft *d, int accept);
rec_record *reviewed_event(const char *disc_id, const char *agent, const char *how, const char *note);
char *tag_normalise(const char *tag);       /* tags.c: 'Place : Kyoto ' -> 'place:kyoto' */

/* rocrate.c: --ro-crate */
char *rocrate_metadata(const rec_record *disc, const entries *files, const formats *fmt);
char *rocrate_preview(const rec_record *disc, const entries *files);

void scan_payload(const char *src, const char *policy, entries *files, entries *noted);
void scan_file(const char *path, const char *as, entry *out);   /* one file, as data/<as> */
char *link_summary(const entries *files, const entries *noted, const char *policy);
void write_listing(const char *path, const entries *files, const entries *noted);
void write_manifest(const char *path, const entries *files, int sha512);
void write_bag_tags(const char *stage, const entries *files, const strlist *info);
void write_tagmanifests(const char *stage);

/* html.c: index.html */
void human_size(uint64_t n, char out[32]);
void html_esc(sbuf *b, const char *s);     /* html.escape */
void url_quote(sbuf *b, const char *s);    /* urllib.parse.quote */
void hash_both(const char *path, char sha256[65], char sha512[129]);
char *render_index(const rec_record *disc, const rec_record *binding, const entries *files,
                   const archive *snapshot, char *(*where_fn)(const archive *, const rec_record *));

/* names.c: file names a disc image cannot keep exactly; volume labels */
typedef struct {
    struct name_issue {
        char *path, *problem;
        int error;          /* 1: the image cannot hold it; 0: some systems show another name */
    } *v;
    size_t n;
} name_issues;
void names_check(char *const *paths, size_t n, name_issues *out);
void names_free(name_issues *x);
char *volume_label(const char *disc_id, const char *text);

#define VERSION "arv 0.4"
#define FORMAT_VERSION "0.5"   /* the disc format (docs/spec/smart-archive-format.md) */
/* arv's source tree (for tools/), and "arv@<commit>" from it (make.c) */
char *find_source(const char *given);
char *software_version(const char *source, int *is_git);
void progress_line(uint64_t done, uint64_t total);   /* a percentage on a terminal (rs03_progress) */

typedef struct {
    char *path;     /* as in the manifest: data/... or a tag file */
    char hex[65];
    int seen;
} mentry;

typedef struct {
    mentry *e;
    size_t n;
} manifest;

typedef struct {
    char *root;
    rec_file cat;           /* catalog.rec */
    const rec_record *disc;
    const char *id;
} ondisc;

typedef struct {
    long long size;         /* -1: only noted (not in data/) */
    char *modified, *kind, *link, *path;
} lrow;

typedef struct {
    lrow *r;
    size_t n;
} listing;

typedef struct {
    char *dir;              /* the folder with archive.rec and volumes/ */
    rec_file rec;
} catalogue;

void die(const char *fmt, const char *arg);
void die2(const char *fmt, const char *a, const char *b);
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *join(const char *a, const char *b);
int by_path(const void *a, const void *b);
int read_manifest(const char *file, manifest *m);
mentry *find_entry(const manifest *m, const char *path);
void free_manifest(manifest *m);
int hash_file(const char *path, char hex[65], int out_fd, uint64_t *bytes);
void open_disc(const char *root, ondisc *d);
char *volume_file(const ondisc *d, const char *name);
int split_tabs(char *line, char **cols, int want);
int read_listing(const char *file, listing *l);
void free_listing(listing *l);
int has_word(const char *kind, const char *word);
long long parse_utc(const char *s);
void print_record(const rec_record *r);
int cmd_info(int argc, char **argv);
int cmd_verify(int argc, char **argv);
int cmd_ls(int argc, char **argv);
int safe_rel(const char *p);
int make_parents(const char *dest, const char *rel);
char *resolve_link(const char *link_path, const char *target);
void set_time(const char *path, const char *modified);
int cmd_restore(int argc, char **argv);
int is_file(const char *dir, const char *name);
char *catalogue_in(const char *path);
int find_catalogue(const char *given, catalogue *c, int required);
void open_catalogue(const char *given, catalogue *c);
int is_type(const rec_record *r, const char *type);
const rec_record *location(const catalogue *c, const char *code);
void location_path(const catalogue *c, const char *code, char **out, size_t *len);
char *where(const catalogue *c, const rec_record *d);
char *lower(const char *s);
int matches(const char *pattern_lower, int glob, const char *text);
const char *access_of(const rec_record *d);
char *catalogue_volume(const catalogue *c, const char *id, const char *name);
int cmd_find(int argc, char **argv);
const rec_record *find_disc(const catalogue *c, const char *id);
int cmd_id(int argc, char **argv);
int disc_in(const rec_record *d, const char *code);
int place_under(const catalogue *c, const char *code, const char *at);
char *upper_trim(const char *s);
int cmd_list(int argc, char **argv);

#endif
