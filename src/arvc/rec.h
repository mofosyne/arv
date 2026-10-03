/* Reading GNU recutils recfiles, as the arv catalogue writes them (docs/smart-archive-format.md):
 * "Name: value" fields, "+" continuation lines, "#" comments, records separated by blank lines,
 * and %rec descriptors that give the records after them their type. */
#ifndef ARV_REC_H
#define ARV_REC_H

#include <stddef.h>

typedef struct {
    char *name, *value;
} rec_field;

typedef struct {
    const char *type;       /* from the last %rec descriptor before it; NULL if none */
    int descriptor;         /* a %rec descriptor itself */
    rec_field *fields;
    size_t nfields;
} rec_record;

typedef struct {
    rec_record *records;
    size_t nrecords;
} rec_file;

/* Reads a recfile; returns 0, or -1 with errno set (file errors) or EINVAL (a line that is
 * not a field, with *bad_line set to its number). */
int rec_read(const char *path, rec_file *out, int *bad_line);
/* The same from text in memory. */
int rec_parse(const char *text, rec_file *out, int *bad_line);
void rec_free(rec_file *f);

/* Building records. A new record's type must outlive it (a string literal, or a descriptor's). */
rec_record *rec_new(rec_file *f, const char *type);       /* appended to f; pointers into f move */
void rec_add(rec_record *r, const char *name, const char *value);
void rec_set(rec_record *r, const char *name, const char *value);   /* replace the first, drop the rest */
void rec_copy(rec_record *dst, const rec_record *src);  /* dst gets src's fields */
void rec_clear(rec_record *r);
/* Writes records as recfile.py does: fields "Name: value" ("+ " continuation lines), records
 * separated by a blank line. Returns 0, or -1 with errno set. */
int rec_write(const char *path, rec_record *const *records, size_t n);
/* The same as text appended to *buf (*len bytes so far). */
void rec_format(const rec_record *r, char **buf, size_t *len);

/* The first value of a field, or NULL. */
const char *rec_get(const rec_record *r, const char *name);
/* The first record of a type (not a descriptor), or NULL. */
const rec_record *rec_first(const rec_file *f, const char *type);

#endif
