/*
 * Packed data objects: an object kept on a disc as one file (a tar, through a compressor or not,
 * or a zip) instead of as its files (research/plan.md, 2026-10-15). arv writes a tar itself, the
 * same bytes for the same files every time, and pipes it through the compressor program on PATH;
 * a zip is the zip program's. A pack's member paths are relative to the folder the pack is in, so
 * unpacking it where it is puts each object at its Path.
 *
 * It also says what a file is packed as when it was put on a disc as it is (a .zip or .tar.zst
 * someone made): its name names the packing, and its first bytes must agree.
 */
#define _XOPEN_SOURCE 700
#include "arv.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum { COMPRESSED, TAR, ZIP, FOUND_ONLY };

static const struct packing {
    const char *name, *ext;
    int kind;
    const char *argv[7];        /* the compressor: stdin to stdout; the same bytes for the same input (zstd at any
                                 * thread count, xz in blocks with two threads or more, lzip and gzip -n always) */
    const char *magic;          /* its first bytes (a tar's: "ustar" at 257) */
    size_t mlen;
} PACKINGS[] = {
    { "tar", ".tar", TAR, { NULL }, "ustar", 5 },
    { "tar+zstd", ".tar.zst", TAR, { "zstd", "-12", "-T0", "-q", "-c", NULL }, "\x28\xb5\x2f\xfd", 4 },
    { "tar+xz", ".tar.xz", TAR, { "xz", "-6", "-T0", "--block-size=64MiB", "-q", "-c", NULL }, "\xfd" "7zXZ\0", 6 },
    { "tar+gzip", ".tar.gz", TAR, { "gzip", "-n", "-9", "-c", NULL }, "\x1f\x8b", 2 },
    { "tar+lzip", ".tar.lz", TAR, { "lzip", "-9", "-q", "-c", NULL }, "LZIP", 4 },
    { "tar+bzip2", ".tar.bz2", TAR, { "bzip2", "-9", "-c", NULL }, "BZh", 3 },
    { "zip", ".zip", ZIP, { "zip", NULL }, "PK\x03\x04", 4 },
    { "zstd", ".zst", COMPRESSED, { "zstd", "-12", "-T0", "-q", "-c", NULL }, "\x28\xb5\x2f\xfd", 4 },
    { "xz", ".xz", COMPRESSED, { "xz", "-6", "-T0", "--block-size=64MiB", "-q", "-c", NULL }, "\xfd" "7zXZ\0", 6 },
    { "gzip", ".gz", COMPRESSED, { "gzip", "-n", "-9", "-c", NULL }, "\x1f\x8b", 2 },
    { "lzip", ".lz", COMPRESSED, { "lzip", "-9", "-q", "-c", NULL }, "LZIP", 4 },
    { "bzip2", ".bz2", COMPRESSED, { "bzip2", "-9", "-c", NULL }, "BZh", 3 },
    { "7z", ".7z", FOUND_ONLY, { NULL }, "7z\xbc\xaf\x27\x1c", 6 },
};
#define NPACKINGS (sizeof PACKINGS / sizeof *PACKINGS)

/* the short names a tar through a compressor also goes by */
static const char *const SHORT_EXT[][2] = { { ".tgz", "tar+gzip" }, { ".tzst", "tar+zstd" }, { ".txz", "tar+xz" },
                                            { ".tlz", "tar+lzip" }, { ".tbz2", "tar+bzip2" }, { ".tbz", "tar+bzip2" } };

static const struct packing *by_name(const char *name)
{
    for (size_t i = 0; name && i < NPACKINGS; i++)
        if (!strcmp(PACKINGS[i].name, name)) return &PACKINGS[i];
    return NULL;
}

const char *packing_problem(const char *name, int for_folder)
{
    const struct packing *pk = by_name(name);
    if (!pk || pk->kind == FOUND_ONLY)
        return "a packing is tar, tar+zstd, tar+xz, tar+gzip, tar+lzip, tar+bzip2 or zip (a file also zstd, xz, gzip, "
               "lzip or bzip2), or none";
    if (for_folder && pk->kind == COMPRESSED) return "a compressor alone packs one file: a folder needs tar+... or zip";
    return NULL;
}

const char *packing_word(const char *name)
{
    const struct packing *pk = by_name(name);
    return pk ? pk->name : NULL;
}

const char *packing_ext(const char *name)
{
    const struct packing *pk = by_name(name);
    return pk ? pk->ext : "";
}

const char *packing_tool(const char *name)
{
    const struct packing *pk = by_name(name);
    return pk ? pk->argv[0] : NULL;
}

static int ends_with(const char *s, const char *end)
{
    size_t a = strlen(s), b = strlen(end);
    return a > b && !strcasecmp(s + a - b, end);
}

const char *packing_found(const char *path)
{
    const struct packing *pk = NULL;
    size_t best = 0;
    for (size_t i = 0; i < NPACKINGS; i++)          /* the longest ending: .tar.gz before .gz */
        if (ends_with(path, PACKINGS[i].ext) && strlen(PACKINGS[i].ext) > best) {
            pk = &PACKINGS[i];
            best = strlen(pk->ext);
        }
    for (size_t i = 0; i < sizeof SHORT_EXT / sizeof *SHORT_EXT; i++)
        if (ends_with(path, SHORT_EXT[i][0])) pk = by_name(SHORT_EXT[i][1]);
    if (!pk) return NULL;
    unsigned char head[512];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t n = read(fd, head, sizeof head);
    close(fd);
    size_t at = pk->kind == TAR && !pk->argv[0] ? 257 : 0;
    if (n < 0 || (size_t)n < at + pk->mlen || memcmp(head + at, pk->magic, pk->mlen)) return NULL;
    return pk->name;
}

/* ------------------------------------------------------------------ writing */

static void put_octal(char *field, size_t len, uint64_t v)
{
    char digits[32];
    snprintf(digits, sizeof digits, "%0*llo", (int)(len - 1), (unsigned long long)v);
    memcpy(field, digits + strlen(digits) - (len - 1), len - 1);    /* NUL-ended: the header is zeroed */
}

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int header(int fd, const char *name, char type, uint64_t size, uint64_t mode, time_t mtime)
{
    char h[512];
    memset(h, 0, sizeof h);
    size_t n = strlen(name);
    if (n <= 100) {
        memcpy(h, name, n);
    } else {                                    /* prefix/name, split at a slash */
        const char *cut = NULL;
        for (const char *s = name; *s; s++)
            if (*s == '/' && (size_t)(s - name) <= 155 && strlen(s + 1) <= 100 && s > name) cut = s;
        if (!cut) return 1;                     /* the caller wrote a pax path first */
        memcpy(h, cut + 1, strlen(cut + 1));
        memcpy(h + 345, name, (size_t)(cut - name));
    }
    put_octal(h + 100, 8, mode);
    put_octal(h + 108, 8, 0);
    put_octal(h + 116, 8, 0);
    put_octal(h + 124, 12, size < 077777777777ULL ? size : 0);
    put_octal(h + 136, 12, mtime > 0 ? (uint64_t)mtime : 0);
    h[156] = type;
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += (unsigned char)h[i];
    snprintf(h + 148, 8, "%06o", sum);
    return write_all(fd, h, sizeof h) ? -1 : 0;
}

static int pad(int fd, uint64_t size)
{
    static const char zero[512];
    return size % 512 ? write_all(fd, zero, 512 - size % 512) : 0;
}

/* a pax record: "LEN key=value\n", LEN counting itself */
static void pax_record(sbuf *b, const char *key, const char *value)
{
    size_t body = strlen(key) + strlen(value) + 3, len = body + 1;
    while (snprintf(NULL, 0, "%zu", len) + body != len) len++;
    sb_printf(b, "%zu %s=%s\n", len, key, value);
}

/* one member: a regular file, its bytes checked against the hash it had when it was scanned */
static uint64_t tar_member(int fd, const entry *e, const char *name)
{
    uint64_t mode = has_word(e->kind, "executable") ? 0755 : 0644;
    sbuf pax = { 0 };
    uint64_t written = 512 + (e->size + 511) / 512 * 512;
    int split_ok = strlen(name) <= 100;
    for (const char *s = name; !split_ok && *s; s++)
        split_ok = *s == '/' && s > name && (size_t)(s - name) <= 155 && strlen(s + 1) <= 100;
    if (!split_ok) pax_record(&pax, "path", name);
    if (e->size >= 077777777777ULL) {
        char sz[32];
        snprintf(sz, sizeof sz, "%llu", (unsigned long long)e->size);
        pax_record(&pax, "size", sz);
    }
    if (pax.len) {
        if (header(fd, "PaxHeaders/member", 'x', pax.len, 0644, e->mtime) || write_all(fd, pax.s, pax.len)
            || pad(fd, pax.len))
            die("cannot write the pack (%s)", strerror(errno));
        written += 512 + (pax.len + 511) / 512 * 512;
    }
    free(pax.s);
    const char *short_name = split_ok ? name : strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
    char cut[101];
    snprintf(cut, sizeof cut, "%s", short_name);
    if (header(fd, split_ok ? name : cut, '0', e->size, mode, e->mtime)) die("cannot write the pack (%s)", strerror(errno));
    char hex[65];
    uint64_t bytes;
    int rc = hash_file(e->source, hex, fd, &bytes);
    if (rc == -1) die("cannot read %s", e->source);
    if (rc == -2) die("cannot write the pack (%s)", strerror(errno));
    if (bytes != e->size || strcmp(hex, e->sha256))
        die("%s changed while the discs were being made: make them again", e->source);
    if (pad(fd, bytes)) die("cannot write the pack (%s)", strerror(errno));
    return written;
}

/* the compressor, reading a pipe and writing out; returns the pipe's end to write to */
static int start_filter(const struct packing *pk, const char *out, pid_t *pid)
{
    int ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) die("cannot create %s", out);
    if (!pk->argv[0]) {
        *pid = 0;
        return ofd;
    }
    int fds[2];
    if (pipe(fds)) die("%s", "cannot start the compressor");
    *pid = fork();
    if (*pid < 0) die("%s", "cannot start the compressor");
    if (*pid == 0) {
        dup2(fds[0], 0);
        dup2(ofd, 1);
        close(fds[0]);
        close(fds[1]);
        close(ofd);
        execvp(pk->argv[0], (char *const *)pk->argv);
        _exit(127);
    }
    close(fds[0]);
    close(ofd);
    return fds[1];
}

static void end_filter(const struct packing *pk, int fd, pid_t pid)
{
    if (close(fd)) die("cannot write the pack (%s)", strerror(errno));
    if (!pid) return;
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) == 127)
        die("%s is not on PATH: install it, or pack another way (arv plan pack)", pk->argv[0]);
    if (WEXITSTATUS(status)) die("%s failed", pk->argv[0]);
}

static int by_member(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static char *const *sort_names;
static int by_index(const void *a, const void *b)
{
    return strcmp(sort_names[*(const size_t *)a], sort_names[*(const size_t *)b]);
}

void pack_write(const char *out, const char *packing, const entry *const *members, char *const *names, size_t n,
                const char *workdir)
{
    const struct packing *pk = by_name(packing);
    if (!pk || pk->kind == FOUND_ONLY) die("%s: not a packing arv writes", packing);
    void (*was)(int) = signal(SIGPIPE, SIG_IGN);    /* a compressor that stops: an error, not the end of arv */
    if (pk->kind == COMPRESSED) {               /* one file, compressed as it is */
        if (n != 1) die("%s packs one file", packing);
        pid_t pid;
        int fd = start_filter(pk, out, &pid);
        char hex[65];
        uint64_t bytes;
        int rc = hash_file(members[0]->source, hex, fd, &bytes);
        if (rc == -1) die("cannot read %s", members[0]->source);
        if (rc == -2) die("cannot write the pack (%s)", strerror(errno));
        end_filter(pk, fd, pid);
        if (bytes != members[0]->size || strcmp(hex, members[0]->sha256))
            die("%s changed while the discs were being made: make them again", members[0]->source);
    } else if (pk->kind == TAR) {
        size_t *order = xmalloc((n + 1) * sizeof *order);   /* by name: the same files, the same bytes */
        for (size_t i = 0; i < n; i++) order[i] = i;
        sort_names = names;
        qsort(order, n, sizeof *order, by_index);
        pid_t pid;
        int fd = start_filter(pk, out, &pid);
        uint64_t written = 0;
        for (size_t i = 0; i < n; i++) written += tar_member(fd, members[order[i]], names[order[i]]);
        static const char zero[10240];          /* two zero blocks, then whole records of 20 blocks */
        uint64_t end = written + 1024;
        end += (10240 - end % 10240) % 10240;
        if (write_all(fd, zero, 1024) || ((end - written - 1024) && write_all(fd, zero, (size_t)(end - written - 1024))))
            die("cannot write the pack (%s)", strerror(errno));
        end_filter(pk, fd, pid);
        free(order);
    } else {                                    /* zip: the zip program, over links named as the members */
        char *farm = xprintf("%s/zip-XXXXXX", workdir), *list = NULL;
        if (!mkdtemp(farm)) die("cannot create %s", farm);
        sbuf names_in = { 0 };
        char **sorted = xmalloc((n + 1) * sizeof *sorted);
        for (size_t i = 0; i < n; i++) {
            char *at = join(farm, names[i]), *slash = strrchr(at, '/');
            *slash = 0;
            if (mkdirs(at)) die("cannot create %s", at);
            *slash = '/';
            if (symlink(members[i]->source, at)) die("cannot create %s", at);
            free(at);
            sorted[i] = names[i];
        }
        qsort(sorted, n, sizeof *sorted, by_member);
        for (size_t i = 0; i < n; i++) sb_printf(&names_in, "%s\n", sorted[i]);
        list = join(workdir, "zip-names");
        write_text(list, names_in.s ? names_in.s : "");
        unlink(out);
        pid_t pid = fork();
        if (pid < 0) die("%s", "cannot start zip");
        if (pid == 0) {
            int in = open(list, O_RDONLY), devnull = open("/dev/null", O_WRONLY);
            if (in < 0 || chdir(farm)) _exit(126);
            dup2(in, 0);
            if (devnull >= 0) dup2(devnull, 1);
            char *const argv[] = { "zip", "-X", "-q", "-9", (char *)out, "-@", NULL };
            execvp(argv[0], argv);
            _exit(127);
        }
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        if (!WIFEXITED(status) || WEXITSTATUS(status) == 127)
            die("%s", "zip is not on PATH: install it, or pack another way (arv plan pack)");
        if (WEXITSTATUS(status)) die("%s", "zip failed");
        for (size_t i = 0; i < n; i++) {        /* what went in is what was hashed */
            char hex[65];
            uint64_t bytes;
            if (hash_file(members[i]->source, hex, -1, &bytes) || bytes != members[i]->size
                || strcmp(hex, members[i]->sha256))
                die("%s changed while the discs were being made: make them again", members[i]->source);
        }
        remove_tree(farm);
        unlink(list);
        free(farm);
        free(list);
        free(sorted);
        free(names_in.s);
    }
    signal(SIGPIPE, was);
}
