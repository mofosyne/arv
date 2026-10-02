/* the parts of NetBSD's libutil that makefs uses */
#ifndef COMPAT_UTIL_H
#define COMPAT_UTIL_H
#include <stdlib.h>
#include <string.h>
#include <err.h>
static inline void *emalloc(size_t n) { void *p = malloc(n); if (!p && n) err(1, "malloc"); return p; }
static inline void *ecalloc(size_t a, size_t b) { void *p = calloc(a, b); if (!p && a && b) err(1, "calloc"); return p; }
static inline void *erealloc(void *o, size_t n) { void *p = realloc(o, n); if (!p && n) err(1, "realloc"); return p; }
static inline char *estrdup(const char *s) { char *p = strdup(s); if (!p) err(1, "strdup"); return p; }
static inline char *estrndup(const char *s, size_t n) { char *p = strndup(s, n); if (!p) err(1, "strndup"); return p; }

#include <stdint.h>
long long strsuftoll(const char *, const char *, long long, long long);
int snprintb(char *, size_t, const char *, uint64_t);
#endif
