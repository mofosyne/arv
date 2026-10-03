/* Small helpers shared by the arvc commands. */
#define _POSIX_C_SOURCE 200809L
#include "arvc.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

void die(const char *fmt, const char *arg)
{
    fprintf(stderr, "arvc: ");
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
    exit(2);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("%s", "out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) die("%s", "out of memory");
    return p;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    return memcpy(xmalloc(n), s, n);
}

char *join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    char *p = xmalloc(la + lb + 2);
    memcpy(p, a, la);
    p[la] = '/';
    memcpy(p + la + 1, b, lb + 1);
    return p;
}
