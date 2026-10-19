/* arv-gui (arv gui): arv's interface in your web browser, served on 127.0.0.1. C99 and POSIX.
 *
 * It serves one page (gui.html) and runs what the page asks: arv, or arv-assist for the local
 * model's suggestions, to its end or as a job whose output the page follows. It also lists folders
 * for the page's file pickers. It knows no feature of arv's: the page builds every command
 * (README.md has the routes). It links arv's own modules for their JSON reader and strings.
 *
 * Only this machine and this session can drive it: it listens on 127.0.0.1, the Host header must
 * name that address, and the page and every request carry a token made when it starts. Commands
 * run without a shell, and their first word (after --home PATH) must be a command, never an option
 * or gui. Each connection is served by a process of its own; a job keeps its output in files, in a
 * folder of this server's under $TMPDIR, removed when it stops. HTTP/1.0 and nothing more: the
 * traffic never leaves the machine, so there is no TLS to do. */
#include "../arv/arv.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef ARV_GUI_EMBEDDED
extern const char GUI_PAGE[];            /* build/page.c: gui.html as built in, by dev-tools/embed.c */
#endif

/* the page built in, or NULL when arv-gui was built by hand without build/page.c */
static const char *built_in_page(void)
{
#ifdef ARV_GUI_EMBEDDED
    return GUI_PAGE;
#else
    return NULL;
#endif
}

#define MAX_HEAD (64 * 1024)
#define MAX_BODY (64L * 1024 * 1024)     /* a draft on standard input, or a pasted library listing */

static char token[33];
static int port, listen_fd = -1, conn_fd = -1;
static char *home_given, *start_dir, *workdir, *page_file, *prog_arv, *prog_assist;
static strlist llm_options;              /* --llm-url URL ..., given to arv-assist */
static volatile sig_atomic_t stopping;
static long idle_minutes;               /* --idle: stop after this long unused, with no job running (0: never) */

/* ------------------------------------------------------------------ small helpers */

static int write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void read_fd(int fd, sbuf *b)
{
    char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) != 0) {
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) break;
        sb_add(b, buf, (size_t)n);
    }
}

static int read_file(const char *path, sbuf *b)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    read_fd(fd, b);
    close(fd);
    return 0;
}

static int write_file(const char *path, const char *s, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int rc = write_all(fd, s, n);
    return close(fd) || rc ? -1 : 0;
}

/* a JSON string from any bytes: what a command printed need not be UTF-8, and JSON must be, so a
 * byte that is not part of a valid character becomes U+FFFD */
static void json_bytes(sbuf *b, const char *s, size_t n)
{
    const unsigned char *p = (const unsigned char *)s, *end = p + n;
    sb_puts(b, "\"");
    while (p < end) {
        unsigned c = *p;
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        int ok = len && (size_t)(end - p) >= len;
        for (size_t k = 1; ok && k < len; k++) ok = (p[k] & 0xC0) == 0x80;
        if (ok && len > 1) {
            unsigned long cp = c & (0x7F >> len);
            for (size_t k = 1; k < len; k++) cp = cp << 6 | (p[k] & 0x3F);
            ok = len == 2 ? cp >= 0x80 : len == 3 ? cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF) : cp >= 0x10000 && cp <= 0x10FFFF;
        }
        if (!ok) {
            sb_puts(b, "\xEF\xBF\xBD");
            p++;
            continue;
        }
        if (c == '"' || c == '\\') sb_printf(b, "\\%c", (int)c);
        else if (c == '\n') sb_puts(b, "\\n");
        else if (c == '\t') sb_puts(b, "\\t");
        else if (c < 0x20) sb_printf(b, "\\u%04x", c);
        else sb_add(b, (const char *)p, len);
        p += len;
    }
    sb_puts(b, "\"");
}

static void json_text(sbuf *b, const char *s)
{
    if (s) json_bytes(b, s, strlen(s));
    else sb_puts(b, "null");
}

/* a path as os.path.abspath gives it: absolute (from where this started), ., .. and // resolved */
static char *normal_path(const char *path)
{
    char *full = path[0] == '/' ? xstrdup(path) : join(start_dir, path);
    strlist parts = { 0 };
    for (char *s = strtok(full, "/"); s; s = strtok(NULL, "/")) {
        if (!strcmp(s, ".")) continue;
        if (!strcmp(s, "..")) {
            if (parts.n) free(parts.v[--parts.n]);
            continue;
        }
        strlist_add(&parts, s);
    }
    sbuf b = { 0 };
    for (size_t i = 0; i < parts.n; i++) sb_printf(&b, "/%s", parts.v[i]);
    free(full);
    strlist_free(&parts);
    return b.s ? b.s : xstrdup("/");
}

static char *parent_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash && slash != path ? xprintf("%.*s", (int)(slash - path), path) : xstrdup("/");
}

/* ~ and ~/... as a shell gives them: the person's home folder */
static char *tilde(const char *s)
{
    const char *home = getenv("HOME");
    if (home && (!strcmp(s, "~") || !strncmp(s, "~/", 2))) return xprintf("%s%s", home, s + 1);
    return xstrdup(s);
}

/* ------------------------------------------------------------------ HTTP */

typedef struct {
    char method[8];
    char *path, *query, *host, *token, *body;
    size_t body_len;
} request;

static const char *reason(int code)
{
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    default: return "Internal Server Error";
    }
}

static void respond(int code, const char *type, const char *body, size_t len)
{
    char *head = xprintf("HTTP/1.0 %d %s\r\nContent-Type: %s; charset=utf-8\r\nContent-Length: %zu\r\n"
                         "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
                         code, reason(code), type, len);
    if (!write_all(conn_fd, head, strlen(head))) write_all(conn_fd, body, len);
    free(head);
}

static void respond_json(int code, sbuf *b)
{
    respond(code, "application/json", b->s ? b->s : "", b->len);
    free(b->s);
    *b = (sbuf){ 0 };
}

static void respond_error(int code, const char *message)
{
    sbuf b = { 0 };
    sb_puts(&b, "{\"error\": ");
    json_text(&b, message);
    sb_puts(&b, "}");
    respond_json(code, &b);
}

/* the request: its line, the headers this needs, and its body; 0 when it is not one */
static int read_request(request *r, int *too_big)
{
    sbuf in = { 0 };
    char buf[8192], *end = NULL;
    memset(r, 0, sizeof *r);
    *too_big = 0;
    while (!end) {
        ssize_t n = read(conn_fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        sb_add(&in, buf, (size_t)n);
        end = strstr(in.s, "\r\n\r\n");
        if (!end && in.len > MAX_HEAD) return 0;
    }
    *end = 0;
    size_t have = in.len - (size_t)(end + 4 - in.s);
    char *line = in.s, *next = strstr(line, "\r\n");
    if (next) *next = 0;
    char target[4096];
    if (sscanf(line, "%7s %4095s", r->method, target) != 2) return 0;
    char *q = strchr(target, '?');
    if (q) *q++ = 0;
    r->path = xstrdup(target);
    r->query = xstrdup(q ? q : "");
    long length = 0;
    for (char *h = next ? next + 2 : NULL; h && *h; h = next ? next + 2 : NULL) {
        next = strstr(h, "\r\n");
        if (next) *next = 0;
        char *colon = strchr(h, ':');
        if (!colon) continue;
        *colon = 0;
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (!strcasecmp(h, "Host")) r->host = xstrdup(v);
        else if (!strcasecmp(h, "X-Archive-Token")) r->token = xstrdup(v);
        else if (!strcasecmp(h, "Content-Length")) length = atol(v);
    }
    if (length < 0 || length > MAX_BODY) {
        *too_big = 1;
        return 0;
    }
    sbuf body = { 0 };
    sb_add(&body, end + 4, have < (size_t)length ? have : (size_t)length);
    while (body.len < (size_t)length) {
        ssize_t n = read(conn_fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        sb_add(&body, buf, (size_t)n);
    }
    r->body = body.s ? body.s : xstrdup("");
    r->body_len = (size_t)length;
    free(in.s);
    return 1;
}

static int hex(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* a query parameter, decoded; NULL when it is missing or empty (as Python's parse_qs) */
static char *param(const char *query, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = query; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, name, n) || p[n] != '=') continue;
        sbuf b = { 0 };
        for (const char *s = p + n + 1; *s && *s != '&'; s++) {
            if (*s == '+') sb_add(&b, " ", 1);
            else if (*s == '%' && hex(s[1]) >= 0 && hex(s[2]) >= 0) {
                char c = (char)(hex(s[1]) * 16 + hex(s[2]));
                sb_add(&b, &c, 1);
                s += 2;
            } else sb_add(&b, s, 1);
        }
        if (b.len) return b.s;
        free(b.s);
    }
    return NULL;
}

/* the token, compared in a time that does not depend on where it differs */
static int token_ok(const char *given)
{
    size_t n = strlen(token), diff = !given || strlen(given) != n;
    for (size_t i = 0; given && !diff && i < n; i++) diff |= (unsigned char)(given[i] ^ token[i]);
    return !diff;
}

/* ------------------------------------------------------------------ running arv */

/* the program and its arguments, checked; NULL with a reason when they may not run */
static char **command_line(const char *program, const jv *args, const char **why)
{
    int assist = !strcmp(program, "arv-assist");
    if (!assist && strcmp(program, "arv")) return *why = "unknown program", NULL;
    if (!args || args->kind != 'a') return *why = "argv: a list of strings", NULL;
    for (size_t i = 0; i < args->n; i++)
        if (args->vals[i]->kind != 's') return *why = "argv: a list of strings", NULL;
    size_t at = args->n && (!strcmp(args->vals[0]->str, "--home") || !strcmp(args->vals[0]->str, "-C")) ? 2 : 0;
    while (!assist && at + 1 < args->n && (!strcmp(args->vals[at]->str, "--agent") || !strcmp(args->vals[at]->str, "--authorship")))
        at += 2;                                /* who a change is by: the assistant's (arv checks the values) */
    const char *word = at < args->n ? args->vals[at]->str : "";
    int command = word[0] >= 'a' && word[0] <= 'z';
    for (const char *c = word; command && *c; c++) command = (*c >= 'a' && *c <= 'z') || *c == '-';
    if (!command || (!assist && !strcmp(word, "gui"))) return *why = "not a command", NULL;
    char **av = xmalloc((args->n + llm_options.n + 2) * sizeof *av);
    size_t n = 0;
    av[n++] = assist ? prog_assist : prog_arv;
    for (size_t i = 0; i < args->n; i++) av[n++] = tilde(args->vals[i]->str);
    for (size_t i = 0; assist && i < llm_options.n; i++) av[n++] = llm_options.v[i];
    av[n] = NULL;
    return av;
}

/* in a child about to run a command: these as its standard input, output and error, and no others */
static void child_fds(int in, int out, int err)
{
    if (in >= 0) dup2(in, STDIN_FILENO);
    if (out >= 0) dup2(out, STDOUT_FILENO);
    if (err >= 0) dup2(err, STDERR_FILENO);
    if (in > STDERR_FILENO) close(in);
    if (out > STDERR_FILENO && out != in) close(out);
    if (err > STDERR_FILENO && err != out && err != in) close(err);
}

static int exit_code(int status)
{
    return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? -WTERMSIG(status) : -1;
}

/* a file of its own for a command's input or output, gone once closed */
static int scratch(void)
{
    char *path = xprintf("%s/runXXXXXX", workdir);
    int fd = mkstemp(path);
    if (fd >= 0) unlink(path);
    free(path);
    return fd;
}

/* run to its end: its exit status, and what it printed on standard output and error */
static int run_to_end(char **av, const char *in, sbuf *out, sbuf *err)
{
    int fi = in ? scratch() : open("/dev/null", O_RDONLY), fo = scratch(), fe = scratch();
    if (fi < 0 || fo < 0 || fe < 0) die("%s", "cannot make a scratch file for a command");
    if (in) {
        write_all(fi, in, strlen(in));
        lseek(fi, 0, SEEK_SET);
    }
    pid_t pid = fork();
    if (pid == 0) {
        child_fds(fi, fo, fe);
        execvp(av[0], av);
        fprintf(stderr, "cannot run %s: %s\n", av[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    lseek(fo, 0, SEEK_SET);
    lseek(fe, 0, SEEK_SET);
    read_fd(fo, out);
    read_fd(fe, err);
    close(fi);
    close(fo);
    close(fe);
    return pid > 0 ? exit_code(status) : -1;
}

/* ------------------------------------------------------------------ jobs: a folder each, jobs/N/ with
 * argv (the command, as JSON), in (its input), out (what it printed) and rc (its exit status, once done) */

static char *job_file(const char *id, const char *name)
{
    return xprintf("%s/jobs/%s/%s", workdir, id, name);
}

static int job_id_ok(const char *id)
{
    if (!id || !*id || strlen(id) > 9) return 0;
    for (const char *c = id; *c; c++)
        if (*c < '0' || *c > '9') return 0;
    return 1;
}

/* the job as the page follows it: its lines from since on (only whole ones, while it runs) */
static int job_json(const char *id, long since, int with_lines, sbuf *b)
{
    char *argv_path = job_file(id, "argv"), *out_path = job_file(id, "out"), *rc_path = job_file(id, "rc");
    sbuf argv = { 0 }, out = { 0 }, rc = { 0 };
    int known = !read_file(argv_path, &argv);
    int done = known && !read_file(rc_path, &rc);
    if (known && with_lines) read_file(out_path, &out);
    free(argv_path);
    free(out_path);
    free(rc_path);
    if (!known) return 0;
    sb_puts(b, "{\"id\": ");
    json_text(b, id);
    sb_printf(b, ", \"argv\": %s, \"done\": %s, \"returncode\": ", argv.s ? argv.s : "[]", done ? "true" : "false");
    if (done) sb_printf(b, "%d", atoi(rc.s ? rc.s : "-1"));
    else sb_puts(b, "null");
    if (with_lines) {
        long count = 0;
        sb_puts(b, ", \"lines\": [");
        int first = 1;
        for (size_t at = 0; at < out.len;) {
            const char *nl = memchr(out.s + at, '\n', out.len - at);
            if (!nl && !done) break;                       /* a line still being written */
            size_t len = nl ? (size_t)(nl - (out.s + at)) : out.len - at;
            if (count++ >= since) {
                if (!first) sb_puts(b, ", ");
                first = 0;
                json_bytes(b, out.s + at, len);
            }
            at += len + (nl ? 1 : 0);
        }
        sb_printf(b, "], \"next\": %ld", count);
    }
    sb_puts(b, "}");
    free(argv.s);
    free(out.s);
    free(rc.s);
    return 1;
}

/* a new job: its folder, then a process that runs the command and writes its exit status when
 * done, which outlives this connection */
static char *start_job(char **av, const char *in)
{
    char *id = NULL;
    for (int n = 1; !id; n++) {
        char *dir = xprintf("%s/jobs/%d", workdir, n);
        if (!mkdir(dir, 0700)) id = xprintf("%d", n);
        else if (errno != EEXIST) die("cannot make %s", dir);
        free(dir);
    }
    sbuf argv = { 0 };
    sb_puts(&argv, "[");
    for (size_t i = 1; av[i]; i++) {
        if (i > 1) sb_puts(&argv, ", ");
        json_text(&argv, av[i]);
    }
    sb_puts(&argv, "]");
    char *argv_path = job_file(id, "argv"), *in_path = job_file(id, "in"), *out_path = job_file(id, "out"),
         *rc_path = job_file(id, "rc"), *rc_tmp = job_file(id, "rc.tmp");
    if (write_file(out_path, "", 0) || (in && write_file(in_path, in, strlen(in))) || write_file(argv_path, argv.s, argv.len))
        die("cannot write the job's files in %s", workdir);
    pid_t runner = fork();
    if (runner == 0) {
        close(conn_fd);
        pid_t pid = fork();
        if (pid == 0) {
            int fi = open(in ? in_path : "/dev/null", O_RDONLY), fo = open(out_path, O_WRONLY | O_APPEND);
            child_fds(fi, fo, fo);
            execvp(av[0], av);
            fprintf(stderr, "cannot run %s: %s\n", av[0], strerror(errno));
            _exit(127);
        }
        int status = 0;
        while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        char *rc = xprintf("%d\n", pid > 0 ? exit_code(status) : -1);
        if (!write_file(rc_tmp, rc, strlen(rc))) rename(rc_tmp, rc_path);
        _exit(0);
    }
    free(argv.s);
    free(argv_path);
    free(in_path);
    free(out_path);
    free(rc_path);
    free(rc_tmp);
    return id;
}

/* ------------------------------------------------------------------ the routes */

/* the page: its file when there is one (so it can be changed without building anything), else the
 * copy built in (so arv-gui on its own still has one) */
static void route_page(void)
{
    sbuf page = { 0 };
    if ((!page_file || read_file(page_file, &page)) && built_in_page()) sb_puts(&page, built_in_page());
    if (!page.s) {
        respond_error(500, "the page gui.html cannot be read");
        return;
    }
    sbuf b = { 0 };
    for (const char *p = page.s ? page.s : "", *t; *p; p = t + 9) {
        if (!(t = strstr(p, "__TOKEN__"))) {
            sb_puts(&b, p);
            break;
        }
        sb_add(&b, p, (size_t)(t - p));
        sb_puts(&b, token);
    }
    respond(200, "text/html", b.s ? b.s : "", b.len);
    free(b.s);
    free(page.s);
}

static void route_setup(void)
{
    sbuf b = { 0 };
    sb_puts(&b, "{\"home\": ");
    json_text(&b, home_given);
    sb_puts(&b, ", \"cwd\": ");
    json_text(&b, start_dir);
    sb_puts(&b, "}");
    respond_json(200, &b);
}

static void route_run(const request *r)
{
    jv *body = json_parse(r->body_len ? r->body : "{}");
    const char *why = NULL;
    if (!body || body->kind != 'o') {
        respond_error(400, "bad request: a JSON object");
        return;
    }
    const jv *program = json_get(body, "program"), *args = json_get(body, "argv"), *in = json_get(body, "stdin"),
             *job = json_get(body, "job");
    if (program && program->kind != 's' && program->kind != 'n') why = "program: a name";
    if (in && in->kind != 's' && in->kind != 'n') why = "stdin: a string";
    char **av = why ? NULL : command_line(program && program->kind == 's' ? program->str : "arv", args, &why);
    if (!av) {
        char *message = xprintf("bad request: %s", why);
        respond_error(400, message);
        free(message);
        jfree(body);
        return;
    }
    const char *stdin_text = in && in->kind == 's' ? in->str : NULL;
    sbuf b = { 0 };
    if (job && job->kind == 't' && !strcmp(job->str, "true")) {
        char *id = start_job(av, stdin_text);
        job_json(id, 0, 1, &b);
        free(id);
    } else {
        sbuf out = { 0 }, err = { 0 };
        int rc = run_to_end(av, stdin_text, &out, &err);
        sb_printf(&b, "{\"returncode\": %d, \"out\": ", rc);
        json_bytes(&b, out.s ? out.s : "", out.len);
        sb_puts(&b, ", \"err\": ");
        json_bytes(&b, err.s ? err.s : "", err.len);
        sb_puts(&b, "}");
        free(out.s);
        free(err.s);
    }
    respond_json(200, &b);
    jfree(body);
}

static void route_job(const request *r)
{
    char *id = param(r->query, "id"), *since = param(r->query, "since");
    sbuf b = { 0 };
    if (job_id_ok(id) && job_json(id, since ? atol(since) : 0, 1, &b)) respond_json(200, &b);
    else respond_error(404, "no such job");
    free(id);
    free(since);
}

/* whether a job is still running: one with no exit status yet */
static int jobs_running(void)
{
    char *dir = xprintf("%s/jobs", workdir);
    DIR *d = opendir(dir);
    int running = 0;
    for (struct dirent *e; d && !running && (e = readdir(d));)
        if (job_id_ok(e->d_name)) {
            char *rc = job_file(e->d_name, "rc");
            running = access(rc, F_OK) != 0;
            free(rc);
        }
    if (d) closedir(d);
    free(dir);
    return running;
}

static int by_number(const void *a, const void *b)
{
    return atoi(*(char *const *)a) - atoi(*(char *const *)b);
}

static void route_jobs(void)
{
    char *dir = xprintf("%s/jobs", workdir);
    strlist ids = { 0 };
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d));)
        if (job_id_ok(e->d_name)) strlist_add(&ids, e->d_name);
    if (d) closedir(d);
    if (ids.n) qsort(ids.v, ids.n, sizeof *ids.v, by_number);
    sbuf b = { 0 };
    sb_puts(&b, "{\"jobs\": [");
    for (size_t i = 0; i < ids.n; i++) {
        if (i) sb_puts(&b, ", ");
        job_json(ids.v[i], 0, 0, &b);
    }
    sb_puts(&b, "]}");
    respond_json(200, &b);
    strlist_free(&ids);
    free(dir);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* a folder's folders, and with files=1 its files and their sizes; hidden ones left out */
static void route_browse(const request *r)
{
    char *given = param(r->query, "path"), *wanted = tilde(given ? given : "~"), *path = normal_path(wanted);
    struct stat st;
    if (stat(path, &st) || !S_ISDIR(st.st_mode)) {
        char *up = parent_of(path);
        free(path);
        path = up;
    }
    char *parent = parent_of(path), *files = param(r->query, "files");
    sbuf b = { 0 };
    sb_puts(&b, "{\"path\": ");
    json_text(&b, path);
    sb_puts(&b, ", \"parent\": ");
    json_text(&b, parent);
    DIR *d = opendir(path);
    if (!d) {
        sb_puts(&b, ", \"dirs\": [], \"files\": [], \"error\": ");
        json_text(&b, strerror(errno));
    } else {
        strlist names = { 0 };
        for (struct dirent *e; (e = readdir(d));)
            if (e->d_name[0] != '.') strlist_add(&names, e->d_name);
        closedir(d);
        if (names.n) qsort(names.v, names.n, sizeof *names.v, by_name);
        sbuf dirs = { 0 }, plain = { 0 };
        for (size_t i = 0; i < names.n; i++) {
            char *full = join(path, names.v[i]);
            struct stat ls;
            if (!stat(full, &st) && S_ISDIR(st.st_mode)) {
                sb_puts(&dirs, dirs.len ? ", " : "");
                json_text(&dirs, names.v[i]);
            } else if (files && !lstat(full, &ls) && !S_ISLNK(ls.st_mode) && S_ISREG(ls.st_mode)) {
                sb_puts(&plain, plain.len ? ", {\"name\": " : "{\"name\": ");
                json_text(&plain, names.v[i]);
                sb_printf(&plain, ", \"bytes\": %lld}", (long long)ls.st_size);
            }
            free(full);
        }
        sb_printf(&b, ", \"dirs\": [%s]", dirs.s ? dirs.s : "");
        if (files) sb_printf(&b, ", \"files\": [%s]", plain.s ? plain.s : "");
        free(dirs.s);
        free(plain.s);
        strlist_free(&names);
    }
    sb_puts(&b, "}");
    respond_json(200, &b);
    free(given);
    free(wanted);
    free(path);
    free(parent);
    free(files);
}

static void serve_connection(void)
{
    request r;
    int too_big = 0;
    if (!read_request(&r, &too_big)) {
        respond_error(too_big ? 413 : 400, "bad request");
        return;
    }
    char *here = xprintf("127.0.0.1:%d", port), *named = xprintf("localhost:%d", port);
    int local = r.host && (!strcmp(r.host, here) || !strcmp(r.host, named));
    char *t = strncmp(r.path, "/api/", 5) ? param(r.query, "t") : NULL;
    int allowed = local && token_ok(strncmp(r.path, "/api/", 5) ? t : r.token);
    free(here);
    free(named);
    free(t);
    if (!allowed) {
        respond_error(403, "forbidden");
        return;
    }
    if (!strcmp(r.method, "GET")) {
        if (!strcmp(r.path, "/")) route_page();
        else if (!strcmp(r.path, "/api/setup")) route_setup();
        else if (!strcmp(r.path, "/api/job")) route_job(&r);
        else if (!strcmp(r.path, "/api/jobs")) route_jobs();
        else if (!strcmp(r.path, "/api/browse")) route_browse(&r);
        else respond_error(404, "not found");
    } else if (!strcmp(r.method, "POST")) {
        if (!strcmp(r.path, "/api/run")) route_run(&r);
        else respond_error(404, "not found");
    } else respond_error(405, "GET or POST");
}

/* ------------------------------------------------------------------ starting and stopping */

static void stop(int sig)
{
    (void)sig;
    stopping = 1;
}

static char *found_file(const char *env, const char *const *candidates, int executable)
{
    const char *e = getenv(env);
    if (e && *e) return xstrdup(e);
    char *dir = exe_dir();
    for (int i = 0; dir && candidates[i]; i++) {
        char *p = xprintf("%s/%s", dir, candidates[i]);
        if (!access(p, executable ? X_OK : R_OK)) {
            free(dir);
            return p;
        }
        free(p);
    }
    free(dir);
    return NULL;
}

static void make_token(void)
{
    unsigned char raw[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, raw, sizeof raw) != (ssize_t)sizeof raw) die("%s", "cannot read /dev/urandom for the session's token");
    close(fd);
    for (size_t i = 0; i < sizeof raw; i++) sprintf(token + 2 * i, "%02x", raw[i]);
}

static void open_browser(const char *url)
{
    if (fork() == 0) {
        int null = open("/dev/null", O_WRONLY);
        child_fds(-1, null, null);
#ifdef __APPLE__
        execlp("open", "open", url, (char *)NULL);
#else
        execlp("xdg-open", "xdg-open", url, (char *)NULL);
#endif
        _exit(127);
    }
}

static int usage(FILE *to)
{
    fputs("usage: arv-gui [--home HOME] [--port N] [--no-browser] [--idle MINUTES] [--llm-url URL] [--llm-model NAME]\n"
          "               [--llm-allow-remote] [--vision-url URL] [--vision-model NAME]\n"
          "arv's interface in your web browser, on 127.0.0.1; arv gui runs it with the home it finds.\n"
          "--idle: stop once the page has been closed that long and no job is running (started from a menu)\n", to);
    return to == stdout ? 0 : 2;
}

int main(int argc, char **argv)
{
    arv_argv0 = argv[0];
    int browser = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) return usage(stdout);
        else if (!strcmp(a, "--no-browser")) browser = 0;
        else if (!strcmp(a, "--llm-allow-remote")) strlist_add(&llm_options, a);
        else if (!strcmp(a, "--vision")) continue;                     /* accepted, as before; the page asks */
        else if (i + 1 < argc && !strcmp(a, "--home")) home_given = argv[++i];
        else if (i + 1 < argc && !strcmp(a, "--port")) port = atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(a, "--idle")) idle_minutes = atol(argv[++i]);
        else if (i + 1 < argc && (!strcmp(a, "--llm-url") || !strcmp(a, "--llm-model") || !strcmp(a, "--vision-url")
                                  || !strcmp(a, "--vision-model"))) {
            strlist_add(&llm_options, a);
            strlist_add(&llm_options, argv[++i]);
        } else return usage(stderr);
    }
    char *here = getcwd(NULL, 0);
    if (!here) die("%s", "cannot tell the current folder");
    start_dir = here;
    if (home_given) home_given = normal_path(home_given);
    static const char *const arv_at[] = { "arv", "../../arv/build/arv", NULL };
    static const char *const assist_at[] = { "arv-assist", "../../arv-assist/build/arv-assist", NULL };
    static const char *const page_at[] = { "../gui.html", "../share/arv/src/arv-gui/gui.html", NULL };
    prog_arv = found_file("ARV", arv_at, 1);
    prog_assist = found_file("ARV_ASSIST", assist_at, 1);
    if (!prog_arv) prog_arv = "arv";                                   /* on PATH */
    if (!prog_assist) prog_assist = "arv-assist";
    page_file = found_file("ARV_GUI_PAGE", page_at, 0);
    if (!page_file && !built_in_page())
        die("%s", "arv-gui: its page, gui.html, was not found beside it or in share/arv (or set ARV_GUI_PAGE)");
    make_token();

    const char *tmp = getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    workdir = xprintf("%s/arv-gui-XXXXXX", tmp);
    char *jobs = NULL;
    if (!mkdtemp(workdir) || mkdir(jobs = join(workdir, "jobs"), 0700)) die("cannot make a folder for jobs in %s", tmp);
    free(jobs);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);
    int yes = 1;
    socklen_t len = sizeof addr;
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0 || setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes)
        || bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) || listen(listen_fd, 16)
        || getsockname(listen_fd, (struct sockaddr *)&addr, &len)) {
        remove_tree(workdir);
        die("cannot listen on 127.0.0.1: %s", strerror(errno));
    }
    fcntl(listen_fd, F_SETFD, FD_CLOEXEC);
    port = ntohs(addr.sin_port);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = stop;                                    /* no SA_RESTART: accept returns, and it stops */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);                                /* a connection's process is gone when done */

    char *url = xprintf("http://127.0.0.1:%d/?t=%s", port, token);
    printf("arv's interface is running at %s  (Ctrl+C to stop)\n", url);
    fflush(stdout);
    if (browser) open_browser(url);
    time_t last = time(NULL);                               /* the page asks every minute while it is open */
    while (!stopping) {
        if (idle_minutes > 0) {
            struct pollfd pf = { listen_fd, POLLIN, 0 };
            int ready = poll(&pf, 1, 30000);
            if (!ready && time(NULL) - last >= idle_minutes * 60 && !jobs_running()) {
                printf("Unused for %ld minute%s, with no job running: stopped.\n", idle_minutes, idle_minutes == 1 ? "" : "s");
                break;
            }
            if (ready <= 0) continue;
        }
        conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) continue;
        last = time(NULL);
        fcntl(conn_fd, F_SETFD, FD_CLOEXEC);
        if (fork() == 0) {
            signal(SIGCHLD, SIG_DFL);                        /* it waits for the commands it runs */
            close(listen_fd);
            serve_connection();
            close(conn_fd);
            _exit(0);
        }
        close(conn_fd);
    }
    remove_tree(workdir);
    free(url);
    return 0;
}
