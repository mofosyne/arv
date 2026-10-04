/* arvc: declarations shared by its modules (see arvc.c). */
#ifndef ARVC_H
#define ARVC_H

#include "discid.h"
#include "edtf.h"
#include "rec.h"
#include "sha256.h"
#include "sha512.h"
#include "vocab.h"

#include <stddef.h>
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
char *exe_dir(void);

/* home.c: where the home catalogue is */
typedef struct {
    char *path, *catalog_dir, *config_dir, *drafts_dir, *cache_dir, *rec_path;
    char *how;              /* how it was found, in the Python arv's words */
} arv_home;
void home_find(arv_home *h, const char *given, const char *source);
void home_at(arv_home *h, const char *path);
void home_ensure(const arv_home *h);
char *home_volume_file(const arv_home *h, const char *disc_id, const char *name);
int cmd_init(int argc, char **argv);
int cmd_make(int argc, char **argv);
int cmd_check(int argc, char **argv);
int cmd_burned(int argc, char **argv);
int cmd_note(int argc, char **argv);
int cmd_locate(int argc, char **argv);
int cmd_access(int argc, char **argv);
int cmd_location(int argc, char **argv);
int cmd_collection(int argc, char **argv);
int cmd_appraise(int argc, char **argv);
int cmd_sets(int argc, char **argv);
int cmd_names(int argc, char **argv);
int cmd_where(int argc, char **argv);

/* archive.c: the catalogue as arv writes it */
typedef struct {
    rec_record **v;
    size_t n;
} recs;
typedef struct {
    rec_file file;          /* what was read; new records are allocated one by one */
    recs discs, bindings, locations, collections, events, appraisals;
} archive;
void recs_add(recs *l, rec_record *r);
int recs_has(const recs *l, const rec_record *r);
rec_record *rec_alloc(const char *type);
rec_record *descriptor(const char *type);
void archive_load(archive *a, const char *path);
void archive_records(const archive *a, recs *out);
void archive_save(const archive *a, const char *path);
void write_records(const char *path, const recs *r);
rec_record *archive_disc(const archive *a, const char *id);
rec_record *archive_location(const archive *a, const char *code);
long archive_next_number(const archive *a, const char *set);
const char *disc_access(const rec_record *d);
void archive_locations_for(const archive *a, const recs *discs, recs *out);
void archive_collections_for(const archive *a, const strlist *disc_ids, recs *out);
rec_record *sealed_view(const rec_record *d);
void archive_shared_subset(const archive *a, const strlist *ids, archive *out);
char *archive_where(const archive *a, const rec_record *d);
rec_record *new_event(const char *disc_id, const char *type, const char *outcome, const char *agent,
                      const char *authorship, const char *note);
char *person(void);
char *place(const archive *a, const char *text);
rec_record *new_appraisal(const char *target, const strlist *importance, const char *basis, const char *review);

/* bag.c: the payload, BagIt tag files and the listing */
typedef struct {
    char *path;             /* relative to data/ */
    char *kind;             /* listing words: "file", "file executable", "link copied", ... */
    char *link;             /* a link's target as written ("" for plain files) */
    char *source;           /* where the bytes are read from */
    uint64_t size;
    time_t mtime;
    int via_folder;         /* reached through a copied folder link */
    char sha256[65], sha512[129];
} entry;
typedef struct {
    entry *v;
    size_t n;
} entries;
void scan_payload(const char *src, const char *policy, entries *files, entries *noted);
char *link_summary(const entries *files, const entries *noted, const char *policy);
void write_listing(const char *path, const entries *files, const entries *noted);
void write_manifest(const char *path, const entries *files, int sha512);
void write_bag_tags(const char *stage, const entries *files, const strlist *info);
void write_tagmanifests(const char *stage);

/* html.c: index.html */
void human_size(uint64_t n, char out[32]);
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
void names_check(char *const *paths, size_t n, int udf250, name_issues *out);
void names_free(name_issues *x);
char *volume_label(const char *disc_id, const char *text, int udf250);

#define VERSION "arvc 0.4"

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
    size_t ok, failed, missing, extra;
    int verbose;
} tally;

typedef struct {
    char *dir;              /* the folder with archive.rec and volumes/ */
    rec_file rec;
} catalogue;

void die(const char *fmt, const char *arg);
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
void check_manifest(const char *root, manifest *m, tally *t);
void find_extra(const char *root, const char *rel, const manifest *m, tally *t);
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
