/* Small helpers shared by the arv commands. */
#define _XOPEN_SOURCE 700
#include "arv.h"

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

void die2(const char *fmt, const char *a, const char *b)
{
    fprintf(stderr, "Error: ");
    fprintf(stderr, fmt, a, b);
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

/* today's date as broken-down time: local, or the day of $SOURCE_DATE_EPOCH (UTC) when set, for
 * reproducible discs and tests (catalog.today_date) */
void today_tm(struct tm *tm)
{
    const char *epoch = getenv("SOURCE_DATE_EPOCH");
    if (epoch && *epoch && strspn(epoch, "0123456789") == strlen(epoch)) {
        time_t t = (time_t)strtoll(epoch, NULL, 10);
        gmtime_r(&t, tm);
        return;
    }
    time_t t = time(NULL);
    localtime_r(&t, tm);
}

void today_iso(char out[11])
{
    struct tm tm;
    today_tm(&tm);
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
    if (!mkdir(path, 0755) || errno == EEXIST) return 0;   /* the usual case: its parent is there */
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
const char *arv_argv0;     /* set by main */

/* the folder this program is in: from argv[0] (a path, or a name found on PATH), else
 * /proc/self/exe (which, for an Actually Portable Executable, may name its loader instead) */
char *exe_dir(void)
{
    char *p = NULL;
    if (arv_argv0 && strchr(arv_argv0, '/')) {
        p = realpath(arv_argv0, NULL);
    } else if (arv_argv0 && getenv("PATH")) {
        char *copy = xstrdup(getenv("PATH"));
        for (char *dir = strtok(copy, ":"); dir && !p; dir = strtok(NULL, ":")) {
            char *cand = join(*dir ? dir : ".", arv_argv0);
            if (!access(cand, X_OK)) p = realpath(cand, NULL);
            free(cand);
        }
        free(copy);
    }
    if (!p) p = realpath("/proc/self/exe", NULL);
    if (!p) return NULL;
    char *slash = strrchr(p, '/');
    if (slash) *slash = 0;
    return p;
}

/* os.path.normpath of an absolute path: no ".", "..", or doubled or trailing "/" */
static char *normalise(const char *abs)
{
    char *copy = xstrdup(abs), *save = NULL;
    sbuf out = { 0 };
    size_t *ends = xmalloc((strlen(abs) + 1) * sizeof *ends), n = 0;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) {
            if (n) out.len = ends[--n];
            if (out.s) out.s[out.len] = 0;
            continue;
        }
        ends[n++] = out.len;
        sb_puts(&out, "/");
        sb_puts(&out, part);
    }
    free(ends);
    free(copy);
    if (!out.len) sb_puts(&out, "/");
    return out.s;
}

/* os.path.abspath: the path from the current folder, normalised (links are not followed) */
char *abs_path(const char *p)
{
    if (p[0] == '/') return normalise(p);
    char *cwd = getcwd(NULL, 4096), *joined = join(cwd ? cwd : ".", p), *out = normalise(joined);
    free(cwd);
    free(joined);
    return out;
}

/* a helper program: $ENV, next to this arv (an installed bin/), or in the checkout this arv was
 * built in (src/arv/build/arv -> src/<name>/...); NULL: look on PATH */
char *helper_program(const char *env, const char *name, const char *in_checkout)
{
    if (getenv(env) && *getenv(env)) return xstrdup(getenv(env));
    char *dir = exe_dir();
    if (!dir) return NULL;
    char *beside = join(dir, name), *built = xprintf("%s/../../%s", dir, in_checkout);
    char *found = !access(beside, X_OK) ? beside : !access(built, X_OK) ? built : NULL;
    found = found ? xstrdup(found) : NULL;
    free(beside);
    free(built);
    free(dir);
    return found;
}

/* ------------------------------------------------------------------ text folded, for keys and comparing */

/* Latin letters with accents as ASCII (NULL: a letter kept as it is; "": not a letter): Latin-1,
 * Extended-A and -B, and Extended Additional (Vietnamese) */
static const char *const FOLD_LATIN[] = {   /* U+00C0 to U+024F */
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "y",
    "a", "a", "a", "a", "a", "a", "c", "c", "c", "c", "c", "c", "c", "c", "d", "d",
    "d", "d", "e", "e", "e", "e", "e", "e", "e", "e", "e", "e", "g", "g", "g", "g",
    "g", "g", "g", "g", "h", "h", "h", "h", "i", "i", "i", "i", "i", "i", "i", "i",
    "i", "i", "ij", "ij", "j", "j", "k", "k", "k", "l", "l", "l", "l", "l", "l", "l",
    "l", "l", "l", "n", "n", "n", "n", "n", "n", "n", "n", "n", "o", "o", "o", "o",
    "o", "o", "oe", "oe", "r", "r", "r", "r", "r", "r", "s", "s", "s", "s", "s", "s",
    "s", "s", "t", "t", "t", "t", "t", "t", "u", "u", "u", "u", "u", "u", "u", "u",
    "u", "u", "u", "u", "w", "w", "y", "y", "y", "z", "z", "z", "z", "z", "z", "s",
    "b", "b", NULL, NULL, NULL, NULL, NULL, "c", "c", NULL, "d", NULL, NULL, NULL, NULL, NULL,
    NULL, "f", "f", "g", NULL, NULL, NULL, "i", "k", "k", "l", NULL, NULL, "n", "n", "o",
    "o", "o", NULL, NULL, "p", "p", NULL, NULL, NULL, NULL, NULL, "t", "t", "t", "t", "u",
    "u", NULL, "v", "y", "y", "z", "z", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, "dz", "dz", "dz", "lj", "lj", "lj", "nj", "nj", "nj", "a", "a", "i",
    "i", "o", "o", "u", "u", "u", "u", "u", "u", "u", "u", "u", "u", NULL, "a", "a",
    "a", "a", NULL, NULL, NULL, NULL, "g", "g", "k", "k", "o", "o", "o", "o", NULL, NULL,
    "j", "dz", "dz", "dz", "g", "g", NULL, NULL, "n", "n", "a", "a", NULL, NULL, NULL, NULL,
    "a", "a", "a", "a", "e", "e", "e", "e", "i", "i", "i", "i", "o", "o", "o", "o",
    "r", "r", "r", "r", "u", "u", "u", "u", "s", "s", "t", "t", NULL, NULL, "h", "h",
    NULL, "d", NULL, NULL, NULL, NULL, "a", "a", "e", "e", "o", "o", "o", "o", "o", "o",
    "o", "o", "y", "y", "l", "n", "t", "j", NULL, NULL, "a", "c", "c", "l", "t", "s",
    "z", NULL, NULL, "b", "u", NULL, "e", "e", "j", "j", "q", "q", "r", "r", "y", "y",
};
static const char *const FOLD_LATIN_MORE[] = {   /* U+1E00 to U+1EFF */
    "a", "a", "b", "b", "b", "b", "b", "b", "c", "c", "d", "d", "d", "d", "d", "d",
    "d", "d", "d", "d", "e", "e", "e", "e", "e", "e", "e", "e", "e", "e", "f", "f",
    "g", "g", "h", "h", "h", "h", "h", "h", "h", "h", "h", "h", "i", "i", "i", "i",
    "k", "k", "k", "k", "k", "k", "l", "l", "l", "l", "l", "l", "l", "l", "m", "m",
    "m", "m", "m", "m", "n", "n", "n", "n", "n", "n", "n", "n", "o", "o", "o", "o",
    "o", "o", "o", "o", "p", "p", "p", "p", "r", "r", "r", "r", "r", "r", "r", "r",
    "s", "s", "s", "s", "s", "s", "s", "s", "s", "s", "t", "t", "t", "t", "t", "t",
    "t", "t", "u", "u", "u", "u", "u", "u", "u", "u", "u", "u", "v", "v", "v", "v",
    "w", "w", "w", "w", "w", "w", "w", "w", "w", "w", "x", "x", "x", "x", "y", "y",
    "z", "z", "z", "z", "z", "z", "h", "t", "w", "y", "a", "s", NULL, NULL, "ss", NULL,
    "a", "a", "a", "a", "a", "a", "a", "a", "a", "a", "a", "a", "a", "a", "a", "a",
    "a", "a", "a", "a", "a", "a", "a", "a", "e", "e", "e", "e", "e", "e", "e", "e",
    "e", "e", "e", "e", "e", "e", "e", "e", "i", "i", "i", "i", "o", "o", "o", "o",
    "o", "o", "o", "o", "o", "o", "o", "o", "o", "o", "o", "o", "o", "o", "o", "o",
    "o", "o", "o", "o", "u", "u", "u", "u", "u", "u", "u", "u", "u", "u", "u", "u",
    "u", "u", "y", "y", "y", "y", "y", "y", "y", "y", NULL, NULL, NULL, NULL, NULL, NULL,
};

static void put_cp(sbuf *b, unsigned cp)
{
    char u[4];
    size_t n = 0;
    if (cp < 0x80) u[n++] = (char)cp;
    else if (cp < 0x800) u[n++] = (char)(0xC0 | (cp >> 6)), u[n++] = (char)(0x80 | (cp & 0x3F));
    else u[n++] = (char)(0xE0 | (cp >> 12)), u[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)), u[n++] = (char)(0x80 | (cp & 0x3F));
    sb_add(b, u, n);
}

/* "Zürich", "Zu\u0308rich", "ZÜRICH" -> "zurich"; "Ålice" -> "alice"; "Αθήνα" -> "αθηνα"; "京都" as it is.
 * Lower case for Latin, Greek and Cyrillic; Latin accents folded to ASCII, and combining accents
 * dropped (so a decomposed form, as macOS writes names, is the same as a composed one); other
 * scripts kept. Not full Unicode normalisation: Hangul in jamo is not composed. */
char *text_fold(const char *s)
{
    sbuf b = { 0 };
    sb_puts(&b, "");
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    while (*p) {
        unsigned cp = *p;
        size_t len = 1;
        if (*p >= 0xC2 && *p < 0xE0 && (p[1] & 0xC0) == 0x80) cp = ((unsigned)(p[0] & 0x1F) << 6) | (p[1] & 0x3F), len = 2;
        else if (*p >= 0xE0 && *p < 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
            cp = ((unsigned)(p[0] & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6) | (p[2] & 0x3F), len = 3;
        const char *plain = NULL;
        if (cp >= 0xC0 && cp < 0x250) plain = FOLD_LATIN[cp - 0xC0];
        else if (cp >= 0x1E00 && cp < 0x1F00) plain = FOLD_LATIN_MORE[cp - 0x1E00];
        if (cp < 0x80) {
            char c = (char)tolower((int)cp);
            sb_add(&b, &c, 1);
        } else if (plain) {
            sb_puts(&b, plain);
        } else if (cp >= 0x300 && cp < 0x370) {
            ;                               /* a combining accent: dropped */
        } else if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) {
            put_cp(&b, cp + 0x20);          /* Greek capitals */
        } else if (cp >= 0x386 && cp <= 0x38F) {
            static const unsigned greek_tonos[] = { 0x3B1, 0, 0x3B5, 0x3B7, 0x3B9, 0, 0x3BF, 0, 0x3C5, 0x3C9 };
            unsigned to = greek_tonos[cp - 0x386];
            if (to) put_cp(&b, to);
            else sb_add(&b, (const char *)p, len);
        } else if (cp >= 0x3AC && cp <= 0x3CE) {
            static const unsigned greek_lower[] = { 0x3B1, 0x3B5, 0x3B7, 0x3B9 };   /* ά έ ή ί; ΰ..ώ below */
            unsigned to = cp <= 0x3AF ? greek_lower[cp - 0x3AC] : cp == 0x3CC ? 0x3BF : cp == 0x3CD ? 0x3C5 : cp == 0x3CE ? 0x3C9
                        : cp == 0x3C2 ? 0x3C3 : cp == 0x3B0 ? 0x3C5 : cp;
            put_cp(&b, to);                 /* tonos dropped, final sigma as sigma */
        } else if (cp >= 0x410 && cp <= 0x42F) {
            put_cp(&b, cp + 0x20);          /* Cyrillic capitals */
        } else if (cp >= 0x400 && cp <= 0x40F) {
            put_cp(&b, cp + 0x50);
        } else {
            sb_add(&b, (const char *)p, len);
        }
        p += len;
    }
    return b.s;
}
