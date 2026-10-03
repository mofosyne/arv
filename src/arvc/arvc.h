/* arvc: declarations shared by its modules (see arvc.c). */
#ifndef ARVC_H
#define ARVC_H

#include "discid.h"
#include "edtf.h"
#include "rec.h"
#include "sha256.h"

#include <stddef.h>
#include <stdint.h>

#define VERSION "arvc 0.3"

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
} disc;

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
void open_disc(const char *root, disc *d);
char *volume_file(const disc *d, const char *name);
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
