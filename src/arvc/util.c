/* Small helpers shared by the arvc commands. */
#define _XOPEN_SOURCE 700
#include "arvc.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void die(const char *fmt, const char *arg)
{
    fprintf(stderr, "Error: ");
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
    exit(1);
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

/* ------------------------------------------------------------------ text, files, processes */

void sb_add(sbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->s = xrealloc(b->s, b->cap);
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

void sb_puts(sbuf *b, const char *s)
{
    sb_add(b, s, strlen(s));
}

void sb_printf(sbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (b->len + (size_t)n + 1 > b->cap) {
        b->cap = (b->len + (size_t)n + 1) * 2;
        b->s = xrealloc(b->s, b->cap);
    }
    va_start(ap, fmt);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

char *xprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *p = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return p;
}

/* One UTF-8 character at *s: its code point, advancing *s (an invalid byte counts as itself). */
unsigned long utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned long c = p[0];
    int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (n <= 0) {
        *s += 1;
        return c;
    }
    c &= 0x3F >> n;
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *s += 1;
            return p[0];
        }
        c = c << 6 | (p[i] & 0x3F);
    }
    *s += n + 1;
    return c;
}

size_t utf8_chars(const char *s)
{
    size_t n = 0;
    while (*s) {
        utf8_next(&s);
        n++;
    }
    return n;
}

void today_iso(char out[11])
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, 11, "%Y-%m-%d", &tm);
}

void uuid4(char out[37])
{
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, b, 16) != 16) die("%s", "cannot read /dev/urandom for a UUID");
    close(fd);
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
             b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

int mkdirs(const char *path)
{
    char *p = xstrdup(path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(p, 0755) && errno != EEXIST) { free(p); return -1; }
        *s = '/';
    }
    int rc = mkdir(p, 0755) && errno != EEXIST ? -1 : 0;
    free(p);
    return rc;
}

void write_text(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");
    if (!fp || fputs(text, fp) == EOF || fclose(fp)) die("cannot write %s", path);
}

char *read_text(const char *path)
{
    FILE *fp = fopen(path, "rb");
    sbuf b = { 0 };
    char buf[65536];
    size_t n;
    if (!fp) return NULL;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) sb_add(&b, buf, n);
    fclose(fp);
    if (!b.s) sb_puts(&b, "");
    return b.s;
}

void copy_file(const char *from, const char *to)
{
    static char buf[1 << 16];
    int in = open(from, O_RDONLY), out;
    ssize_t n;
    if (in < 0) die("cannot read %s", from);
    if ((out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) die("cannot write %s", to);
    while ((n = read(in, buf, sizeof buf)) > 0)
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(out, buf + done, (size_t)(n - done));
            if (w < 0) die("cannot write %s", to);
            done += w;
        }
    if (n < 0) die("cannot read %s", from);
    close(in);
    if (close(out)) die("cannot write %s", to);
}

/* Copies a folder tree (files and folders; links are copied as what they point to), leaving out
 * names in `skip` (NULL-terminated). */
void copy_tree(const char *from, const char *to, const char *const *skip)
{
    DIR *d = opendir(from);
    struct dirent *e;
    if (!d) die("cannot read %s", from);
    if (mkdirs(to)) die("cannot create %s", to);
    while ((e = readdir(d))) {
        int skipped = !strcmp(e->d_name, ".") || !strcmp(e->d_name, "..");
        for (int i = 0; skip && skip[i] && !skipped; i++)
            skipped = !fnmatch(skip[i], e->d_name, 0);
        if (skipped) continue;
        char *a = join(from, e->d_name), *b = join(to, e->d_name);
        struct stat st;
        if (!stat(a, &st)) {
            if (S_ISDIR(st.st_mode)) copy_tree(a, b, skip);
            else if (S_ISREG(st.st_mode)) {
                copy_file(a, b);
                chmod(b, st.st_mode & 0777);
            }
        }
        free(a);
        free(b);
    }
    closedir(d);
}

void remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st)) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char *p = join(path, e->d_name);
            remove_tree(p);
            free(p);
        }
        if (d) closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

/* Runs a program (argv[0] looked up on PATH) and returns its exit status, with stdout and
 * stderr together in *output (may be NULL); -1 when it cannot be started. */
int run(char *const argv[], char **output)
{
    int fds[2];
    if (pipe(fds)) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    sbuf b = { 0 };
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) sb_add(&b, buf, (size_t)n);
    close(fds[0]);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!b.s) sb_puts(&b, "");
    if (output) *output = b.s; else free(b.s);
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status) == 127 ? -1 : WEXITSTATUS(status);
}

int on_path(const char *program)
{
    const char *path = getenv("PATH");
    if (!path) return 0;
    char *copy = xstrdup(path);
    int found = 0;
    for (char *dir = strtok(copy, ":"); dir && !found; dir = strtok(NULL, ":")) {
        char *p = join(*dir ? dir : ".", program);
        found = !access(p, X_OK);
        free(p);
    }
    free(copy);
    return found;
}

/* The folder this program is in (Linux: /proc/self/exe), or NULL. */
char *exe_dir(void)
{
    char *p = realpath("/proc/self/exe", NULL);
    if (!p) return NULL;
    char *slash = strrchr(p, '/');
    if (slash) *slash = 0;
    return p;
}
