/* Disc ids (scheme set-seq-coverage/1; tests/fixtures/disc-id-*.tsv and check-chars.tsv define
 * the contract): SET-SEQ_COVERAGE_CHECK, e.g. PHOTOS-07_2015-2024_Q. */
#ifndef ARV_DISCID_H
#define ARV_DISCID_H

#include <stddef.h>

#define DISCID_SCHEME "set-seq-coverage/1"
#define DISCID_MAX 32

/* Luhn mod 36 over the letters and digits of payload (case ignored; others skipped). */
char discid_check(const char *payload);
/* Coverage as the user typed it -> EDTF. 0, or -1 if invalid. */
int discid_to_edtf(const char *coverage, char *out, size_t n);
/* EDTF coverage -> its form in ids: 2015/2024 -> 2015-2024, 2019-07 -> 201907. 0, or -1. */
int discid_compact(const char *coverage, char *out, size_t n);
/* The id for these fields. 0, or -1 when they cannot make one. */
int discid_compose(const char *set, long sequence, const char *coverage, char *out, size_t n);

typedef struct {
    const char *scheme;     /* DISCID_SCHEME */
    char set[64], coverage[32];
    long sequence;
    char check;
    int valid;              /* the check character is right */
} discid_parts;

/* 1 and the parts when text is an id of the current scheme, else 0. */
int discid_parse(const char *text, discid_parts *p);

#endif
