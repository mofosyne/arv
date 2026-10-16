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
char *age_cutoff(const char *ago);
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
    char *how;              /* how it was found */
} arv_home;
void home_find(arv_home *h, const char *given, const char *source);   /* stops, saying how to make one, when none */
int home_try(arv_home *h, const char *given, const char *source);     /* -1 when there is none */
extern const char NO_HOME[];                                            /* what home_find says then */
extern const char *home_archive_name;   /* --archive NAME: a home from the machine config */
void home_at(arv_home *h, const char *path);
void home_ensure(const arv_home *h);
void home_guard(const char *arv_dir);      /* stops a change to a copy of the catalogue (config/paths.rec) */
int home_here(const char *arv_dir);        /* records this place as the catalogue's; 1 if new */
void home_paths(const char *arv_dir, strlist *out);
char *home_volume_file(const arv_home *h, const char *disc_id, const char *name);
void layout_notes(const arv_home *h, const char *abs, FILE *out, const char *prefix);   /* status.c */
char *path_manifest(const arv_home *h, const char *path, size_t *files);                /* status.c */
char *home_root(const arv_home *h);                     /* the folder holding the home */
extern const char *const HOME_VOCABULARIES[];           /* config/'s files that discs carry: sets.rec, tags.rec */
char *path_rel(const char *root, const char *abs);      /* relative to root when inside it, else as it is */
char *path_abs(const char *root, const char *stored);   /* back to absolute */
int cmd_init(int argc, char **argv);
int cmd_tags(int argc, char **argv);
void tags_canonical(const arv_home *h, strlist *tags);
int tag_namespaces(const arv_home *h, strlist *one, strlist *declared, int *date_from_file);
typedef struct {
    strlist names, alias_from, alias_to, ns_names, ns_desc, ns_one, rule_tag, rule_glob;
} tag_words_t;
int tag_words(const arv_home *h, tag_words_t *out);   /* the tag vocabulary's words (tags.c) */

/* known.c: known names, entries of the tag vocabulary (authority control; research/plan.md) */
typedef struct {
    char *key, *label, *qualifier, *basis, *retired;   /* retired: why, or NULL */
    strlist aliases, related, same;
    int private_;
} known_entry;
size_t known_load(const arv_home *h, known_entry **out);
void known_free(known_entry *v, size_t n);
char *known_label_of(const char *key);      /* person:anne-smith -> "Anne Smith" */
int known_field(const arv_home *h, const char *key, const char *field, const char *value, int add);
char *known_strip_private(const char *text);   /* tags.rec without its Private: yes entries */
int known_cmd(const char *sub, int argc, char **argv, const char *given);
int cmd_keywords(int argc, char **argv);
int cmd_make(int argc, char **argv);
long medium_budget(const char *medium, const char **label);
long file_sectors(uint64_t size, const char *path);

int cmd_check(int argc, char **argv);
int cmd_burned(int argc, char **argv);
int cmd_note(int argc, char **argv);
int cmd_rename(int argc, char **argv);
int cmd_retag(int argc, char **argv);
int cmd_same(int argc, char **argv);
int cmd_locate(int argc, char **argv);
int cmd_access(int argc, char **argv);
int cmd_location(int argc, char **argv);
int cmd_collection(int argc, char **argv);
int cmd_status(int argc, char **argv);
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
int cmd_audit(int argc, char **argv);
int cmd_identify(int argc, char **argv);
int cmd_relabel(int argc, char **argv);
int cmd_inventory(int argc, char **argv);

/* archive.c: the catalogue as arv writes it */
typedef struct {
    rec_record **v;
    size_t n;
} recs;
typedef struct {
    rec_file file;          /* what was read; new records are allocated one by one */
    recs homes, discs, bindings, locations, collections, revisions, objects, events, appraisals;
} archive;
void recs_add(recs *l, rec_record *r);
int recs_has(const recs *l, const rec_record *r);
rec_record *rec_alloc(const char *type);
rec_record *descriptor(const char *type);
void archive_load(archive *a, const char *path);
void archive_records(const archive *a, recs *out);
void archive_save(archive *a, const char *path);
void archive_same_objects(archive *a);
/* a disc's events, in the catalogue's order (indexed once a list; do not keep it across an append) */
recs disc_events(const archive *a, const char *disc_id);
/* a lineage's object records (one Uuid), in the catalogue's order (indexed; not kept across a change) */
recs object_records(const archive *a, const char *uuid);
recs disc_objects(const archive *a, const char *disc_id);     /* a disc's object records, likewise */     /* objects said to be one (arv same) read as one; on every load */
const char *archive_home_uuid(archive *a);           /* this home's identity, made on first use */
void write_records(const char *path, const recs *r);
rec_record *archive_disc(const archive *a, const char *id);
rec_record *archive_location(const archive *a, const char *code);
long archive_next_number(const archive *a, const char *set);
const char *disc_access(const rec_record *d);
void archive_locations_for(const archive *a, const recs *discs, recs *out);
void archive_copy_locations(const archive *a, const char *disc_id, strlist *out);
rec_record *sealed_view(const rec_record *d);
void archive_shared_subset(const archive *a, const strlist *ids, archive *out);
rec_record *object_disc_view(const rec_record *o);
char *archive_where(const archive *a, const rec_record *d);
rec_record *new_event(const char *disc_id, const char *type, const char *outcome, const char *agent,
                      const char *authorship, const char *note);
char *person(void);
char *place(const archive *a, const char *text);
void place_check(const archive *a, const char *text);
/* copies: replication events, each named by a letter (Copy: A, B ...) */
int is_copy_event(const rec_record *e, const char *disc_id);
size_t disc_copies(const archive *a, const char *disc_id);
int copy_exists(const archive *a, const char *disc_id, const char *letter);
int event_names_copy(const char *disc_id, const rec_record *e, const rec_record *rep);
const char *copy_location(const archive *a, const char *disc_id, const rec_record *copy);
const char *copy_letter(const archive *a, const char *disc_id, const rec_record *rep);   /* its letter now */
const char *letter_of(const archive *a, const rec_record *rep);     /* the same, "?" when it has none */
size_t copies_lettered(const archive *a, const char *disc_id, const char *letter, const rec_record **first);
int copy_letter_ok(const char *s);
char *copy_next(const archive *a, const char *disc_id);
void event_content_id(const rec_record *e, char out[33]);   /* from its fields, all but EventId */
void event_key(const rec_record *e, char out[33]);          /* EventId, or an empty string if none */
int copy_read_back(const archive *a, const char *disc_id, const rec_record *copy);
const char *disc_last_check(const archive *a, const char *disc_id);
const char *copy_last_check(const archive *a, const char *disc_id, const rec_record *copy);   /* its date, or NULL */
char *identifier_normal(const char *text, const char **why);   /* "scheme:value" checked, or NULL and why */
int identifier_strong(const char *id);                           /* bca: and barcode: name one physical disc */
void copy_identifiers(const archive *a, const char *disc_id, const rec_record *copy, strlist *out);
const rec_record *copy_by_identifier(const archive *a, const char *id);   /* the copy's replication, or NULL */
const char *copy_path(const archive *a, const char *disc_id, const rec_record *copy);  /* a stored copy's newest */
const char *copy_carrier(const archive *a, const char *disc_id, const rec_record *copy);   /* its tape or drive */
const rec_record *copy_shares_carrier(const archive *a, const char *disc_id, const rec_record *copy);
const rec_record *copy_same_as(const archive *a, const char *disc_id, const rec_record *copy);  /* recorded before */
size_t identity_report(const archive *a);
void field_heads(const archive *a, const char *thing, const char *field, strlist *heads);
void sets_field(const archive *a, rec_record *e, const char *thing, const char *field);   /* Sets, Replaces */
/* a path's tags or caption changed after burning (field "Tags" or "Caption"), on an event about the
 * disc; and the disc's tags.tsv now (burned: the file as burned, or NULL), NULL when there are none */
void tag_change(const archive *a, rec_record *ev, const char *disc_id, const char *field, const char *path, const char *value);
char *disc_tags_text(const archive *a, const char *disc_id, const char *burned);
FILE *disc_tags_open(const archive *a, const char *disc_id, const char *burned);
/* a data object's field now (Tags, Slug), from events about it (Object: object:UUID), or NULL; its name
 * on discs now (Slug, else its Name) */
const char *object_field(const archive *a, const char *uuid, const char *field);
const char *object_slug(const archive *a, const rec_record *o);
const rec_record *object_at_uuid(const archive *a, const char *uuid);    /* its newest record, or NULL */
/* its tags now: set on it (retag, an apply), else what its disc gives its path (its folders' too) */
char *object_tags_now(const arv_home *h, const archive *a, const rec_record *o);
rec_record *object_event_like(const rec_record *ev, const char *uuid);   /* an event about it, as ev is */
const rec_record *object_at(const archive *a, const char *disc_id, const char *path);   /* the object at path on a disc */
void known_made(const arv_home *h, archive *cat, const char *key, const char *label, const char *basis);   /* known.c */
/* the Uuid of the data object a word names (its Uuid or 8+ characters of it, or its name); dies
 * when it names none or several (listed) */
const char *object_by_word(const archive *a, const char *text);
size_t field_conflicts(const archive *a);    /* a field changed apart in two catalogues: printed, counted */    /* recorded twice, on two discs, two BCAs: printed, counted */
/* the BCA serial of the disc in a drive (Linux): 32 hex digits; -1 when there is none (drive.c) */
int drive_bca(const char *device, char out[33]);
void drive_list(strlist *out);          /* the optical drives (Linux: /dev/sr0 ...); none found elsewhere */
char *read_volume_label(const char *path);   /* an image's or drive's volume label, or NULL */
int cmd_drives(int argc, char **argv);
int info_image(const char *path, const char *given, int as_json);   /* arv info IMAGE.iso: which disc, which copy */
int bca_normal(const char *text, char out[33]);     /* a BCA serial as given: its first 16 bytes, in hex */
/* hot, warm or cold: the place's Temperature, or the nearest place above it that has one; NULL */
const char *place_temperature(const archive *a, const char *code);
int temperature_ok(const char *t);
rec_record *new_appraisal(const char *target, const strlist *importance, const char *basis, const char *review);
int disc_importance(const archive *cat, const char *id, char **why);
size_t archive_merge(archive *home, const archive *other, int prefer_other, strlist *added, strlist *updated,
                     strlist *clashes);   /* clashes: disc ids that name another image there: not merged */

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
    char *origin;           /* arv make --plan: where a laid-out file would be by hand (NULL: at path) */
    char *pack;             /* arv make --plan: the pack it goes into, under data/ (NULL: on the disc as it is) */
    const char *packing;    /* and how (pack.c) */
    char *member;           /* its name in the pack */
    int is_pack;            /* a pack itself, made from the members above */
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
    char *seen;             /* "FILES BYTES STAMP" when planned (names, sizes, dates), or NULL */
    char *placed;           /* plan_scan: where the disc's layout put it, whole; NULL: at path */
    char *of;               /* the data object (Uuid) it continues, as said with plan add --of; or NULL */
    char *packing;          /* as said with plan pack: tar+zstd, zip, ... or none; NULL: its disc's */
} plan_item;
typedef struct {
    char *file, *name;
    char *root;             /* sources are kept relative to it: the home's root, or the plan's folder */
    int discs;
    plan_item *v;
    size_t n;
    rec_file rec;           /* the plan as read: the Plan record gives make's defaults */
    strlist tag_src, tag_val;   /* tags given in the plan: a source file or folder (absolute), its tags */
} disc_plan;
int cmd_plan(int argc, char **argv);
void plan_load(const char *file, disc_plan *out);
/* the files of a plan, each where it goes (on a disc with a layout, each item placed whole by its
 * tags: it->placed, and each file's origin says where it was arranged); tagged: "PATH\tTAGS" for
 * each file the plan gives tags to; clashes: two files given one path by a layout */
const char *plan_layout(const disc_plan *p, int disc);
size_t duplicates_report(const entries *files, int by_bin, size_t limit, FILE *out, uint64_t *bytes);
void plan_scan(const arv_home *h, disc_plan *p, const char *links, entries *files, entries *noted, size_t *left_out,
               strlist *tagged, strlist *clashes);
void plan_made(const char *file, const strlist *disc_ids, const char *date);
const char *plan_packing(const disc_plan *p, const plan_item *it, int *together);   /* NULL: not packed */

/* pack.c: data objects packed into one file (a tar, compressed or not, or a zip) */
const char *packing_problem(const char *name, int for_folder);   /* NULL: a packing arv writes */
const char *packing_ext(const char *name);          /* ".tar.zst" */
const char *packing_word(const char *name);         /* the name, kept for good (NULL: not one) */
const char *packing_tool(const char *name);         /* the program it needs, or NULL */
const char *packing_found(const char *path);        /* what a file is packed as, by name and bytes, or NULL */
void pack_write(const char *out, const char *packing, const entry *const *members, char *const *names, size_t n,
                const char *workdir);
/* reads a pack: each file's name in it (relative to the pack's folder) and SHA-256, and with out_dir
 * writes them there too (never over a file); single: a compressed file's name. NULL, or what is wrong */
const char *pack_read(const char *file, const char *packing, const char *single, const char *out_dir, strlist *names,
                      strlist *hashes);
typedef struct {
    char *sha256, *path, *pack, *packing;  /* a file in a pack: its hash and path as unpacked, under data/ */
} packed_row;
size_t packed_rows(const char *file, packed_row **out);   /* a disc's catalog/volumes/ID/packed.tsv */
const char *pack_compare(const char *pack_file, const packed_row *rows, size_t n, const char *pack, const char *out_dir,
                         size_t *same, size_t *differ, size_t *gone, uint64_t *bytes, sbuf *msgs);
void text_sha256(const char *text, char hex[65]);
void json_str(sbuf *b, const char *s);     /* a JSON string, quoted */
/* every JSON output starts {"format": "arv-json 1", ...}: whose and which shape (fields are only
 * added within 1), as the TSV outputs' first line "# arv todo 1" (research/plan.md, issue #31) */
#define JSON_FORMAT "\"format\": \"arv-json 1\""
void json_record_fields(sbuf *b, const rec_record *r, int *first, const rec_record *skip);
/* a command that changes the record, given --json: the events it made, collected as it saves them */
extern int json_events_wanted;
extern sbuf json_events;
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
void git_prepare(const char *src, const char *workdir, entries *files, gitrepos *out);
char *git_tsv_for(const gitrepos *g, const entries *disc_files);
int git_internal(const char *path);
/* about.c: what a data object says about itself, by rules (README, manifests, licence, git) */
typedef struct {
    char *title, *description, *coverage, *rights, *origin;
} about_info;
void about_read(const char *dir, int git, about_info *out);
int about_any(const about_info *a);
void about_free(about_info *a);
char *about_origin(const char *url);                    /* a remote URL as host/path; NULL for a local path */
extern const char *const ABOUT_FIELDS[];                /* Title, Description, Coverage, Rights, Origin */
void about_fields(const about_info *a, const char *values[5]);
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
    char **keys;                /* object keys, in insertion order */
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
    char *path, *title, *description;  /* objects: the object (its path on the disc, name or Uuid) */
    strlist notes;
} draft_object;
typedef struct {
    char *title, *description, *agent, *authorship;
    strlist subjects, notes, questions;
    size_t nft, ncap, nobj;
    char **ft_folder, **cap_folder, **cap_text;
    strlist *ft_tags;
    draft_object *obj;
} draft;
int is_model(const char *agent);
void draft_load(const char *path, draft *d, int accept);
rec_record *reviewed_event(const char *disc_id, const char *agent, const char *how, const char *note);
char *tag_normalise(const char *tag);       /* tags.c: 'Place : Kyoto ' -> 'place:kyoto' */

/* questions.c: questions kept until answered (<home>/drafts/questions.rec, home only) */
typedef struct {
    char *about;            /* a disc id, or object:UUID */
    char *text, *asked, *agent;
    char *skipped;          /* the date it was skipped, or NULL: open */
} question;
typedef struct {
    question *v;
    size_t n;
    char *path;
} question_list;
void questions_load(const arv_home *h, question_list *q);
void questions_save(const question_list *q);
void questions_free(question_list *q);
size_t questions_open(const question_list *q, const char *about);
size_t questions_add(question_list *q, const archive *cat, const char *about, const strlist *texts, const char *agent,
                     const char *today);                /* how many were new (at most 5 open each) */
void questions_close(question_list *q, const char *about, const char *text, const char *skipped);
char *question_about_name(const archive *cat, const char *about);


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
void progress_line(uint64_t done, uint64_t total);
extern const char MAKE_HELP[];                       /* arv make --help (make.c) */   /* a percentage on a terminal (rs03_progress) */

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
    archive records;        /* the same catalogue grouped for copy/location views */
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
int bag_check(const char *root, int verbose, sbuf *out);
char *volume_file(const ondisc *d, const char *name);
int split_tabs(char *line, char **cols, int want);
int read_listing(const char *file, listing *l);
void free_listing(listing *l);
int listing_has_file(const char *listing_file, const char *rel);
int has_word(const char *kind, const char *word);
long long parse_utc(const char *s);
void print_record(const rec_record *r);
int cmd_info(int argc, char **argv);
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
