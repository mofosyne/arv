/*
 * arv-assist: arv's optional local-AI helpers (arv describe and arv tag), in C99 and POSIX. It
 * links arv's own modules (src/arv) for the catalogue, drafts and tags. Nothing here is
 * needed to make, read, check or repair a disc; everything runs against local models only, unless
 * a remote server is explicitly allowed.
 */
#ifndef ASSIST_H
#define ASSIST_H

#include "../arv/arv.h"

#include <stdint.h>
#include <time.h>

/* ------------------------------------------------------------------ files of a folder or disc */

typedef struct {
    char *path;             /* relative to the folder (or to data/ on a disc) */
    uint64_t size;
    time_t mtime;
} item;

typedef struct {
    item *v;
    size_t n;
} items;

void items_of_folder(const char *src, items *out);                 /* describe.folder_entries */
void items_of_disc(const arv_home *h, const char *disc_id, items *out);   /* describe.disc_entries */
void items_free(items *l);

/* folder -> tags (and an optional caption): insertion order kept, as Python dicts keep it */
typedef struct {
    char **folder;
    strlist *tags;
    char **caption;         /* NULL entries: none */
    size_t n;
} ftags;

strlist *ftags_get(ftags *f, const char *folder, int create);
void ftags_set_caption(ftags *f, const char *folder, const char *caption);
void ftags_free(ftags *f);

/* ------------------------------------------------------------------ http.c: a small HTTP/1.1 client */

int url_is_loopback(const char *url);
/* method "GET" or "POST" (body: JSON). 0 with *status and *reply (NUL-terminated), or -1 with *err */
int http_request(const char *url, const char *method, const char *body, int timeout, int *status, char **reply, char **err);

/* ------------------------------------------------------------------ json helpers */

void json_dump(sbuf *b, const jv *v, int indent);      /* indent < 0: compact, as json.dumps() */
jv *jnum(const char *literal);                          /* a number, true, false or null as written */
jv *jstrings(const strlist *l);
char *jstr_of(const jv *v);                             /* a string's text, else NULL */

/* ------------------------------------------------------------------ llm.c: OpenAI-compatible servers */

typedef struct {
    char *url;              /* without a trailing slash, e.g. http://127.0.0.1:11434/v1 */
    char *model;            /* NULL until resolved */
    int timeout;
} llm_client;

#define LLM_DEFAULT_URL "http://127.0.0.1:11434/v1"

int llm_open(llm_client *c, const char *url, const char *model, int allow_remote, char **err);
const char *llm_model(llm_client *c, char **err);
jv *llm_call(llm_client *c, const char *path, const jv *body, char **err);     /* body NULL: GET */
char *llm_chat(llm_client *c, jv *messages, const char *temperature, int json_mode, char **err);
char *llm_agent(const llm_client *c);

typedef struct {
    char *title, *description;
    strlist subjects, questions;
    ftags folders;          /* folder tags, and captions from images */
} suggestion;

char *llm_inventory(const items *l, const char *name, const rec_record *existing, const char *text_root);
void llm_folders(const items *l, strlist *out);         /* llm.folders_of */
int llm_suggest(llm_client *c, const char *inventory, const strlist *answers, const suggestion *previous,
                int max_questions, const strlist *folders, suggestion *out, char **err);
int llm_parse(const char *content, const strlist *folders, suggestion *out, char **err);
jv *suggestion_json(const suggestion *s, int with_questions);
void suggestion_free(suggestion *s);

/* ------------------------------------------------------------------ vision.c: sample images, local only */

typedef struct {
    char *folder, *caption;
    strlist tags;
    size_t images;
} seen_folder;

typedef struct {
    seen_folder *v;
    size_t n;
} seen;

int vision_analyse(llm_client *c, const char *root, const items *l, int per_folder, int max_total, seen *out, char **err);
void vision_sample(const items *l, int per_folder, int max_total, items *out);
void vision_parse_reply(const char *content, char **caption, strlist *tags);
char *vision_inventory_section(const seen *s);
void vision_merge(suggestion *s, const seen *v);        /* describe.with_vision */
jv *seen_json(const seen *s);
void seen_from_json(const jv *v, seen *out);
void seen_free(seen *s);

/* ------------------------------------------------------------------ the commands */

int assist_describe(int argc, char **argv);
int assist_suggest(int argc, char **argv);              /* for arv-gui: one round, JSON in and out */
int assist_llm_status(int argc, char **argv);
int assist_tag(int argc, char **argv);

/* shared by describe and tag */
const char *kind_of(const char *ext);           /* tag.c: ".jpg" -> "photos" ("files": none of the kinds) */
int generic_name(const char *base);              /* tag.c: a camera's or phone's name: DSC_4001.JPG */
char *ask(const char *prompt);
/* words.c: which words are rare in the archive's names, and which go with which tags (counted) */
#define WORD_MIN 3              /* a word says a tag once seen in this many tagged paths ... */
#define WORD_SHARE 80           /* ... and with the tag in this many percent of them */
typedef struct word_stats word_stats;
word_stats *word_stats_load(const arv_home *h, const archive *cat);   /* cache/words.tsv, and the answers kept */
void word_stats_free(word_stats *ws);
void name_word_list(const char *name, strlist *out);  /* a name's words, and each two in a row */
double word_rarity(const word_stats *ws, const char *w);
int word_common(const word_stats *ws, const char *w);  /* in a fifth of the archive's folders or more */
void word_tags(const word_stats *ws, const strlist *names, const strlist *text, strlist *tags, strlist *why);
void word_counts(const word_stats *ws, const char *w, const char *tag, size_t *with, size_t *all);
void text_clues(const word_stats *ws, const char *root, const items *l, strlist *out, strlist *titles);
void word_example_add(const arv_home *h, const strlist *tags, const strlist *names, const strlist *text);
int word_rule_add(const arv_home *h, const char *tag, const char *word);

/* ask.c: a suggestion without a model: the files' clues, and with interactive, the person's answers
 * to questions over the vocabulary (per_folder: offer to ask about each first-level folder too) */
void ask_suggest(const arv_home *h, const archive *cat, const items *l, const char *name, const char *root, int interactive,
                 int per_folder, suggestion *out);                          /* a line from the terminal; "" at the end */
void read_tags_file(const char *path, ftags *out);      /* catalog.read_tag_info */
void read_tags_now(const archive *cat, const char *disc_id, const char *burned, ftags *out);   /* as burned, and since */
size_t record_tag_changes(archive *cat, rec_record *ev, const char *disc_id, const ftags *before, const ftags *after);
void save_json(const char *path, const jv *doc);

#endif
