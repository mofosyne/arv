/* Coverage dates: the EDTF subset arv uses (docs/smart-archive-format.md, "Coverage";
 * tests/fixtures/coverage.tsv and covers.tsv are the contract). Days are YYYYMMDD numbers;
 * an open end is 00010101 or 99991231. */
#ifndef ARV_EDTF_H
#define ARV_EDTF_H

#define EDTF_MIN 10101L
#define EDTF_MAX 99991231L

/* First and last day a coverage value spans. Returns 1, 0 when it says nothing (empty, "..",
 * "/"), or -1 when it is not valid. */
int edtf_span(const char *coverage, long *first, long *last);
/* 1 if the coverage overlaps the query (a date or a range), 0 if not, -1 if either is invalid. */
int edtf_covers(const char *coverage, const char *query);

#endif
