/*
 * File format identification with Siegfried (src/arv/formats.py): PRONOM ids for every file, in
 * catalog/volumes/<disc-id>/formats.csv, so it stays clear which format (and version) each file is,
 * even when its extension is wrong. Optional: used when `sf` is on PATH.
 */
#define _XOPEN_SOURCE 700
#include "arv.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *const COLUMNS[] = { "path", "puid", "format", "version", "mime", "basis", "warning" };
static const char *const SF_COLUMNS[] = { NULL, "id", "format", "version", "mime", "basis", "warning" };

/* runs argv with stdout in *out and stderr in *err; returns the exit status, -1 if it cannot start */
static int run_split(char *const argv[], const char *workdir, char **out, char **err)
{
    char *errpath = xprintf("%s/sf-stderr-XXXXXX", workdir);
    int errfd = mkstemp(errpath), fds[2];
    if (errfd < 0 || pipe(fds)) die("cannot run %s", argv[0]);
    pid_t pid = fork();
    if (pid < 0) die("cannot run %s", argv[0]);
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, 0);
        dup2(fds[1], 1);
        dup2(errfd, 2);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    sbuf b = { 0 };
    char buf[65536];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) sb_add(&b, buf, (size_t)n);
    close(fds[0]);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    close(errfd);
    *out = b.s ? b.s : xstrdup("");
    *err = read_text(errpath);
    if (!*err) *err = xstrdup("");
    unlink(errpath);
    free(errpath);
    return WIFEXITED(status) && WEXITSTATUS(status) != 127 ? WEXITSTATUS(status) : -1;
}

/* one CSV record (RFC 4180, as Python's csv module reads it) from *s; NULL at the end */
static strlist *csv_record(const char **s, strlist *fields)
{
    if (!**s) return NULL;
    memset(fields, 0, sizeof *fields);
    sbuf f = { 0 };
    const char *p = *s;
    int quoted = 0, done = 0;
    while (!done) {
        char c = *p;
        if (quoted) {
            if (!c) { done = 1; continue; }
            if (c == '"' && p[1] == '"') { sb_add(&f, "\"", 1); p += 2; continue; }
            if (c == '"') { quoted = 0; p++; continue; }
            sb_add(&f, p++, 1);
            continue;
        }
        if (c == '"') { quoted = 1; p++; continue; }
        if (c == ',' || c == '\n' || c == '\r' || !c) {
            strlist_add(fields, f.s ? f.s : "");
            free(f.s);
            memset(&f, 0, sizeof f);
            if (c == ',') { p++; continue; }
            if (c == '\r') p++;
            if (*p == '\n') p++;
            done = 1;
            continue;
        }
        sb_add(&f, p++, 1);
    }
    free(f.s);
    *s = p;
    return fields;
}

void formats_free(formats *f)
{
    free(f->header);
    for (size_t i = 0; i < f->n; i++)
        for (int k = 0; k < 7; k++) free(f->rows[i][k]);
    free(f->rows);
    memset(f, 0, sizeof *f);
}

/* Siegfried over src; 0, or -1 with *error set to its last line of complaint */
int formats_identify(const char *src, const char *sf_home, const char *workdir, formats *out, char **error)
{
    memset(out, 0, sizeof *out);
    char *argv[12], *version, *err, *csv, *cpus = xprintf("%ld", sysconf(_SC_NPROCESSORS_ONLN) > 0 ? sysconf(_SC_NPROCESSORS_ONLN) : 1L);
    int n = 0;
    argv[n++] = "sf";
    if (sf_home) { argv[n++] = "-home"; argv[n++] = (char *)sf_home; }
    int base = n;
    argv[n++] = "-version";
    argv[n] = NULL;
    run_split(argv, workdir, &version, &err);
    free(err);
    n = base;
    argv[n++] = "-csv";
    argv[n++] = "-multi";
    argv[n++] = cpus;
    argv[n++] = (char *)src;
    argv[n] = NULL;
    run_split(argv, workdir, &csv, &err);
    free(cpus);
    if (strncmp(csv, "filename,", 9)) {
        const char *msg = *err ? err : csv;
        char *copy = xstrdup(msg), *e = copy + strlen(copy);
        while (e > copy && (e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\r' || e[-1] == '\t')) *--e = 0;
        char *last = strrchr(copy, '\n');
        *error = *copy ? xstrdup(last ? last + 1 : copy) : xstrdup("sf produced no output");
        free(copy);
        free(csv);
        free(err);
        free(version);
        return -1;
    }
    free(err);
    /* the header: every line of `sf -version` (stripped as a whole) as a comment */
    {
        char *v = version, *e = v + strlen(v);
        while (*v == ' ' || *v == '\n' || *v == '\t' || *v == '\r') v++;
        while (e > v && (e[-1] == ' ' || e[-1] == '\n' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
        sbuf h = { 0 };
        for (char *line = v; *v && line;) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            sb_printf(&h, "# %s\n", line);
            line = nl ? nl + 1 : NULL;
        }
        out->header = h.s ? h.s : xstrdup("");
    }
    free(version);
    const char *s = csv;
    strlist head, rec;
    int idx[7];
    csv_record(&s, &head);
    for (int k = 0; k < 7; k++) {
        idx[k] = -1;
        for (size_t j = 0; j < head.n; j++)
            if (!strcmp(head.v[j], k ? SF_COLUMNS[k] : "filename")) idx[k] = (int)j;
    }
    size_t srclen = strlen(src);
    while (csv_record(&s, &rec)) {
        if (rec.n == 0 || (rec.n == 1 && !*rec.v[0])) {     /* a blank line */
            strlist_free(&rec);
            continue;
        }
        const char *name = idx[0] >= 0 && (size_t)idx[0] < rec.n ? rec.v[idx[0]] : "";
        const char *rel = !strncmp(name, src, srclen) && name[srclen] == '/' ? name + srclen + 1 : name;
        size_t at = out->n;
        for (size_t i = 0; i < out->n; i++)
            if (!strcmp(out->rows[i][0], rel)) at = i;
        if (at == out->n) {
            out->rows = xrealloc(out->rows, (out->n + 1) * sizeof *out->rows);
            out->n++;
        } else {
            for (int k = 0; k < 7; k++) free(out->rows[at][k]);
        }
        out->rows[at][0] = xstrdup(rel);
        for (int k = 1; k < 7; k++)
            out->rows[at][k] = xstrdup(idx[k] >= 0 && (size_t)idx[k] < rec.n ? rec.v[idx[k]] : "");
        strlist_free(&rec);
    }
    strlist_free(&head);
    free(csv);
    return 0;
}

static char *const *row_for(const formats *f, const char *path)
{
    for (size_t i = 0; i < f->n; i++)
        if (!strcmp(f->rows[i][0], path)) return f->rows[i];
    return NULL;
}

size_t formats_unknown(const formats *f, const entries *files)
{
    size_t n = 0;
    for (size_t i = 0; i < files->n; i++) {
        char *const *r = row_for(f, files->v[i].path);
        n += !r || !strcmp(r[1], "UNKNOWN");
    }
    return n;
}

/* the agent of the format identification event: the first line of the header */
char *formats_agent(const formats *f)
{
    if (!f->header || !*f->header) return xstrdup("siegfried");
    const char *s = f->header;
    while (*s == '#' || *s == ' ') s++;
    const char *nl = strchr(s, '\n');
    return xprintf("%.*s", (int)(nl ? nl - s : (long)strlen(s)), s);
}

static void csv_field(sbuf *b, const char *v)
{
    if (strpbrk(v, ",\"\r\n")) {
        sb_puts(b, "\"");
        for (const char *p = v; *p; p++) {
            if (*p == '"') sb_puts(b, "\"");       /* doubled */
            sb_add(b, p, 1);
        }
        sb_puts(b, "\"");
    } else {
        sb_puts(b, v);
    }
}

/* formats.csv for one disc: only its files, in their order */
void formats_write(const char *path, const formats *f, const entries *files)
{
    sbuf b = { 0 };
    sb_puts(&b, "# PRONOM format identification (https://www.nationalarchives.gov.uk/PRONOM/)\n");
    sb_puts(&b, f->header ? f->header : "");
    for (int k = 0; k < 7; k++) sb_printf(&b, "%s%s", k ? "," : "", COLUMNS[k]);
    sb_puts(&b, "\n");
    for (size_t i = 0; i < files->n; i++) {
        char *const *r = row_for(f, files->v[i].path);
        if (r) {
            for (int k = 0; k < 7; k++) {
                if (k) sb_puts(&b, ",");
                csv_field(&b, r[k]);
            }
        } else {
            csv_field(&b, files->v[i].path);
            sb_puts(&b, ",UNKNOWN,,,,,");
        }
        sb_puts(&b, "\n");
    }
    write_text(path, b.s);
    free(b.s);
}
