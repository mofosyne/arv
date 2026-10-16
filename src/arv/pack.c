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

/* ------------------------------------------------------------------ reading */

static ssize_t read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

/* the decompressor, reading the pack and writing a pipe; returns the pipe's end to read from */
static int start_reader(const struct packing *pk, const char *file, pid_t *pid)
{
    int in = open(file, O_RDONLY);
    if (in < 0) return -1;
    if (!pk->argv[0] || pk->kind == ZIP) {
        *pid = 0;
        return in;
    }
    int fds[2];
    if (pipe(fds)) {
        close(in);
        return -1;
    }
    *pid = fork();
    if (*pid < 0) return -1;
    if (*pid == 0) {
        dup2(in, 0);
        dup2(fds[1], 1);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 2);
        close(fds[0]);
        close(fds[1]);
        close(in);
        execlp(pk->argv[0], pk->argv[0], "-dc", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    close(in);
    return fds[0];
}

/* 0, or why not: the program missing, or it failed */
static const char *end_reader(int fd, pid_t pid)
{
    char sink[65536];
    while (read(fd, sink, sizeof sink) > 0) {}   /* what is left (the tar's padding) */
    close(fd);
    if (!pid) return NULL;
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) == 127) return "is not on PATH";
    return WEXITSTATUS(status) ? "failed: the pack is damaged" : NULL;
}

static uint64_t octal(const char *f, size_t n)
{
    if ((unsigned char)f[0] & 0x80) {           /* base-256, for large sizes */
        uint64_t v = (unsigned char)f[0] & 0x7f;
        for (size_t i = 1; i < n; i++) v = v << 8 | (unsigned char)f[i];
        return v;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n && f[i]; i++)
        if (f[i] >= '0' && f[i] <= '7') v = v * 8 + (uint64_t)(f[i] - '0');
    return v;
}

/* one member's bytes from fd: hashed, and written to out (-1: not) */
static int member_bytes(int fd, uint64_t size, int out, char hex[65])
{
    static unsigned char buf[1 << 16];
    sha256_ctx c;
    unsigned char digest[32];
    sha256_init(&c);
    for (uint64_t left = size; left;) {
        size_t want = left < sizeof buf ? (size_t)left : sizeof buf;
        if (read_full(fd, buf, want) != (ssize_t)want) return -1;
        sha256_update(&c, buf, want);
        if (out >= 0 && write_all(out, buf, want)) return -2;
        left -= want;
    }
    sha256_final(&c, digest);
    sha256_hex(digest, hex);
    if (size % 512) {
        char pad_[512];
        if (read_full(fd, pad_, 512 - size % 512) != (ssize_t)(512 - size % 512)) return -1;
    }
    return 0;
}

/* writes a member under dir (never over a file there); returns the open file, or -1 with *why */
static int member_file(const char *dir, const char *name, const char **why)
{
    if (!safe_rel(name)) {
        *why = "not a safe path";
        return -1;
    }
    int fd = -1;
    char *at = join(dir, name);
    if (make_parents(dir, name) || (fd = open(at, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644)) < 0) *why = strerror(errno);
    free(at);
    return fd;
}

static void member_done(const char *dir, const char *name, int executable, time_t mtime)
{
    char *at = join(dir, name);
    chmod(at, executable ? 0755 : 0644);
    struct timespec ts[2] = { { mtime, 0 }, { mtime, 0 } };
    utimensat(AT_FDCWD, at, ts, AT_SYMLINK_NOFOLLOW);
    free(at);
}

const char *pack_read(const char *file, const char *packing, const char *single, const char *out_dir, strlist *names,
                      strlist *hashes)
{
    static char why[512];
    const struct packing *pk = by_name(packing);
    if (!pk || pk->kind == FOUND_ONLY) return "not a packing arv reads";
    void (*was)(int) = signal(SIGPIPE, SIG_IGN);
    const char *problem = NULL;
    if (pk->kind == ZIP) {                      /* the zip program: its names, then each member */
        char *list = NULL;
        char *const la[] = { "unzip", "-Z1", (char *)file, NULL };
        int rc = run(la, &list);
        if (rc == -1) problem = "unzip is not on PATH";
        else if (rc) problem = "unzip cannot read it: the pack is damaged";
        if (!problem && out_dir) {              /* unzip writes them, with their times; never over a file */
            char *out = NULL;
            char *const xa[] = { "unzip", "-q", "-n", "-X", (char *)file, "-d", (char *)out_dir, NULL };
            if (mkdirs(out_dir) || run(xa, &out)) problem = "unzip could not write every file";
            free(out);
        }
        for (char *line = list, *next; !problem && line && *line; line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            size_t n = strlen(line);
            if (!n || line[n - 1] == '/') continue;
            char hex[65];
            if (out_dir) {
                char *at = join(out_dir, line);
                if (!safe_rel(line) || hash_file(at, hex, -1, NULL)) {
                    snprintf(why, sizeof why, "%s was not unpacked", line);
                    problem = why;
                }
                free(at);
            } else {
                sbuf pat = { 0 };               /* unzip reads a name as a pattern: its wildcards escaped */
                for (const char *c = line; *c; c++) {
                    if (strchr("*?[]\\", *c)) sb_add(&pat, "\\", 1);
                    sb_add(&pat, c, 1);
                }
                int fds[2];
                if (pipe(fds)) problem = "cannot run unzip";
                pid_t pid = problem ? -1 : fork();
                if (pid == 0) {
                    dup2(fds[1], 1);
                    close(fds[0]);
                    close(fds[1]);
                    execlp("unzip", "unzip", "-p", file, pat.s, (char *)NULL);
                    _exit(127);
                }
                if (pid > 0) {
                    close(fds[1]);
                    sha256_ctx c;
                    unsigned char buf[65536], digest[32];
                    ssize_t r;
                    sha256_init(&c);
                    while ((r = read(fds[0], buf, sizeof buf)) > 0) sha256_update(&c, buf, (size_t)r);
                    close(fds[0]);
                    sha256_final(&c, digest);
                    sha256_hex(digest, hex);
                    int status;
                    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
                        snprintf(why, sizeof why, "unzip cannot read %s: the pack is damaged", line);
                        problem = why;
                    }
                }
                free(pat.s);
            }
            if (!problem) {
                strlist_add(names, line);
                strlist_add(hashes, hex);
            }
        }
        free(list);
        signal(SIGPIPE, was);
        return problem;
    }
    pid_t pid;
    int fd = start_reader(pk, file, &pid);
    if (fd < 0) {
        signal(SIGPIPE, was);
        return "cannot read it";
    }
    if (pk->kind == COMPRESSED) {               /* one file, compressed */
        char hex[65];
        const char *wrong = NULL;
        int out = out_dir ? member_file(out_dir, single, &wrong) : -1;
        if (out_dir && out < 0) {
            snprintf(why, sizeof why, "%s: %s", single, wrong);
            problem = why;
        } else {
            sha256_ctx c;
            unsigned char buf[65536], digest[32];
            ssize_t r;
            sha256_init(&c);
            while ((r = read(fd, buf, sizeof buf)) > 0) {
                sha256_update(&c, buf, (size_t)r);
                if (out >= 0 && write_all(out, buf, (size_t)r)) problem = "cannot write";
            }
            sha256_final(&c, digest);
            sha256_hex(digest, hex);
            if (out >= 0) close(out);
            strlist_add(names, single);
            strlist_add(hashes, hex);
        }
    } else {                                    /* a tar: header, bytes, padding; pax for long names and sizes */
        char h[512], *long_name = NULL;
        uint64_t long_size = 0;
        int have_size = 0;
        while (!problem) {
            if (read_full(fd, h, 512) != 512) {
                problem = "it ends too soon: the pack is damaged";
                break;
            }
            int zero = 1;
            for (int i = 0; i < 512 && zero; i++) zero = !h[i];
            if (zero) break;
            uint64_t size = octal(h + 124, 12);
            char type = h[156];
            if (type == 'x' || type == 'L') {   /* pax extended header, or GNU long name: for the next */
                char *body = xmalloc((size_t)size + 1);
                if (read_full(fd, body, (size_t)size) != (ssize_t)size) {
                    free(body);
                    problem = "it ends too soon: the pack is damaged";
                    break;
                }
                body[size] = 0;
                if (size % 512) {
                    char pad_[512];
                    read_full(fd, pad_, 512 - size % 512);
                }
                if (type == 'L') {
                    free(long_name);
                    long_name = xstrdup(body);
                } else {
                    for (char *r = body; r < body + size;) {    /* "LEN key=value\n" */
                        char *sp = strchr(r, ' '), *eq = sp ? strchr(sp, '=') : NULL;
                        long len = strtol(r, NULL, 10);
                        if (!sp || !eq || len <= 0 || r + len > body + size) break;
                        char *end = r + len - 1;
                        if (!strncmp(sp + 1, "path=", 5)) {
                            free(long_name);
                            long_name = xprintf("%.*s", (int)(end - eq - 1), eq + 1);
                        } else if (!strncmp(sp + 1, "size=", 5)) {
                            long_size = strtoull(eq + 1, NULL, 10);
                            have_size = 1;
                        }
                        r += len;
                    }
                }
                free(body);
                continue;
            }
            if (have_size) size = long_size;
            char name[257];
            if (long_name) snprintf(name, sizeof name, "%s", long_name);
            else if (h[345]) snprintf(name, sizeof name, "%.155s/%.100s", h + 345, h);
            else snprintf(name, sizeof name, "%.100s", h);
            char *full = long_name ? long_name : xstrdup(name);
            long_name = NULL;
            have_size = 0;
            if (type != '0' && type != 0) {     /* folders and anything else: not files arv packed */
                char hex[65];
                if (member_bytes(fd, size, -1, hex)) problem = "it ends too soon: the pack is damaged";
                free(full);
                continue;
            }
            const char *wrong = NULL;
            int out = out_dir ? member_file(out_dir, full, &wrong) : -1;
            if (out_dir && out < 0) {
                snprintf(why, sizeof why, "%s: %s", full, wrong);
                problem = why;
                free(full);
                break;
            }
            char hex[65];
            int rc = member_bytes(fd, size, out, hex);
            if (out >= 0) {
                close(out);
                member_done(out_dir, full, (int)(octal(h + 100, 8) & 0111), (time_t)octal(h + 136, 12));
            }
            if (rc == -1) problem = "it ends too soon: the pack is damaged";
            else if (rc == -2) problem = "cannot write";
            else {
                strlist_add(names, full);
                strlist_add(hashes, hex);
            }
            free(full);
        }
        free(long_name);
    }
    const char *end = end_reader(fd, pid);
    if (!problem && end) {
        snprintf(why, sizeof why, "%s %s", pk->argv[0], end);
        problem = why;
    }
    signal(SIGPIPE, was);
    return problem;
}

/* ------------------------------------------------------------------ a disc's packed.tsv */

size_t packed_rows(const char *file, packed_row **out)
{
    *out = NULL;
    char *text = read_text(file);
    if (!text) return 0;
    size_t n = 0, cap = 0;
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (line[0] == '#' || !*line) continue;
        char *c[4];
        if (split_tabs(line, c, 4) != 4) continue;
        if (n == cap) {
            cap = cap ? 2 * cap : 64;
            *out = xrealloc(*out, cap * sizeof **out);
        }
        packed_row r = { xstrdup(c[0]), xstrdup(c[1]), xstrdup(c[2]), xstrdup(c[3]) };
        (*out)[n++] = r;
    }
    free(text);
    return n;
}

/* one pack against the rows naming it: read (and with out_dir, unpacked there), each file's hash
 * compared; every finding a line in msgs. NULL, or why it could not be read at all */
const char *pack_compare(const char *pack_file, const packed_row *rows, size_t n, const char *pack, const char *out_dir,
                         size_t *same, size_t *differ, size_t *gone, uint64_t *bytes, sbuf *msgs)
{
    const char *packing = NULL, *single = NULL;
    size_t mine = 0;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(rows[i].pack, pack)) {
            packing = rows[i].packing;
            single = strrchr(rows[i].path, '/') ? strrchr(rows[i].path, '/') + 1 : rows[i].path;
            mine++;
        }
    if (!packing) return "no files are listed for it";
    char *folder = xstrdup(pack), *slash = strrchr(folder, '/');
    if (slash) *slash = 0;
    else *folder = 0;
    strlist names = { 0 }, hashes = { 0 };
    const char *wrong = pack_read(pack_file, packing, single, out_dir, &names, &hashes);
    strlist seen = { 0 };
    for (size_t k = 0; k < names.n; k++) {
        char *path = *folder ? join(folder, names.v[k]) : xstrdup(names.v[k]);
        const packed_row *r = NULL;
        for (size_t i = 0; i < n && !r; i++)
            if (!strcmp(rows[i].pack, pack) && (!strcmp(rows[i].path, path) || mine == 1)) r = &rows[i];
        if (!r) sb_printf(msgs, "EXTRA    %s (in data/%s, not in its list)\n", path, pack);
        else if (strcmp(r->sha256, hashes.v[k])) {
            sb_printf(msgs, "DAMAGED  %s (in data/%s: its checksum differs)\n", r->path, pack);
            (*differ)++;
        } else {
            (*same)++;
            struct stat st;
            char *at = out_dir ? join(out_dir, names.v[k]) : NULL;
            if (at && bytes && !stat(at, &st)) *bytes += (uint64_t)st.st_size;
            free(at);
        }
        if (r) strlist_add(&seen, r->path);
        free(path);
    }
    for (size_t i = 0; i < n; i++)
        if (!strcmp(rows[i].pack, pack) && !strlist_has(&seen, rows[i].path)) {
            sb_printf(msgs, "MISSING  %s (not read from data/%s)\n", rows[i].path, pack);
            (*gone)++;
        }
    strlist_free(&names);
    strlist_free(&hashes);
    strlist_free(&seen);
    free(folder);
    return wrong;
}
