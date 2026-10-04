/*
 * fake-llm: a stand-in OpenAI-compatible server for arv-assist's checks, on 127.0.0.1.
 *
 *   fake-llm PORT-FILE LOG-FILE TEXT-REPLY-FILE IMAGE-REPLY-FILE
 *
 * Writes the port it listens on to PORT-FILE, then serves until killed:
 *   GET  /v1/models             {"data": [{"id": "fake-model"}]}
 *   POST /v1/chat/completions   the content of IMAGE-REPLY-FILE when the request shows an image,
 *                               else of TEXT-REPLY-FILE (both read afresh for each request)
 *   POST /v1/embeddings         three-number vectors: "cat" in the text, "code" or "python", 0.3
 * Every request body is appended to LOG-FILE, one per line.
 */
#define _XOPEN_SOURCE 700
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static char *slurp(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return strdup("");
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    s[fread(s, 1, (size_t)n, fp)] = 0;
    fclose(fp);
    return s;
}

/* s as a JSON string */
static char *quoted(const char *s)
{
    size_t n = strlen(s);
    char *o = malloc(6 * n + 3), *p = o;
    *p++ = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { *p++ = '\\'; *p++ = (char)c; }
        else if (c == '\n') { *p++ = '\\'; *p++ = 'n'; }
        else if (c < 0x20) p += sprintf(p, "\\u%04x", c);
        else *p++ = (char)c;
    }
    *p++ = '"';
    *p = 0;
    return o;
}

static void reply(int fd, const char *body)
{
    char head[256];
    int n = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                                        "Connection: close\r\n\r\n", strlen(body));
    if (write(fd, head, (size_t)n) < 0 || write(fd, body, strlen(body)) < 0) return;
}

/* the texts of an embeddings request's "input" list, each turned into a vector */
static char *embeddings(const char *body)
{
    const char *in = strstr(body, "\"input\"");
    size_t cap = 4096, len = 0;
    char *out = malloc(cap);
    len += (size_t)sprintf(out, "{\"data\": [");
    int index = 0;
    for (const char *p = in ? strchr(in, '[') : NULL; p && *p && *p != ']';) {
        const char *q = strchr(p, '"');
        if (!q) break;
        const char *e = q + 1;
        while (*e && *e != '"') e += *e == '\\' ? 2 : 1;
        char *text = strndup(q + 1, (size_t)(e - q - 1));
        for (char *t = text; *t; t++) *t = (char)(*t >= 'A' && *t <= 'Z' ? *t + 32 : *t);
        double cat = strstr(text, "cat") ? 3.0 : 0.0, code = strstr(text, "code") || strstr(text, "python") ? 3.0 : 0.0;
        free(text);
        if (len + 200 > cap) out = realloc(out, cap *= 2);
        len += (size_t)sprintf(out + len, "%s{\"index\": %d, \"embedding\": [%.1f, %.1f, 0.3]}", index ? ", " : "", index, cat, code);
        index++;
        p = e + 1;
        while (*p == ' ' || *p == ',' || *p == '\n') p++;
    }
    sprintf(out + len, "]}");
    return out;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fputs("usage: fake-llm PORT-FILE LOG-FILE TEXT-REPLY-FILE IMAGE-REPLY-FILE\n", stderr);
        return 2;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 16) || getsockname(s, (struct sockaddr *)&a, &al)) {
        perror("fake-llm");
        return 1;
    }
    FILE *pf = fopen(argv[1], "w");
    fprintf(pf, "%d\n", ntohs(a.sin_port));
    fclose(pf);
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        size_t cap = 1 << 16, len = 0;
        char *req = malloc(cap);
        long want = -1;
        for (;;) {               /* the headers, then Content-Length bytes */
            if (len + 4096 > cap) req = realloc(req, cap *= 2);
            ssize_t r = read(c, req + len, cap - len - 1);
            if (r <= 0) break;
            len += (size_t)r;
            req[len] = 0;
            char *end = strstr(req, "\r\n\r\n");
            if (end && want < 0) {
                char *cl = strstr(req, "Content-Length: ");
                want = (long)(end + 4 - req) + (cl && cl < end ? atol(cl + 16) : 0);
            }
            if (want >= 0 && (long)len >= want) break;
        }
        char *body = strstr(req, "\r\n\r\n");
        body = body ? body + 4 : req + len;
        if (!strncmp(req, "GET", 3)) {
            reply(c, "{\"data\": [{\"id\": \"fake-model\"}]}");
        } else {
            FILE *log = fopen(argv[2], "a");
            if (log) {
                fprintf(log, "%s\n", body);
                fclose(log);
            }
            if (strstr(req, "/embeddings")) {
                char *out = embeddings(body);
                reply(c, out);
                free(out);
            } else {
                char *content = slurp(strstr(body, "image_url") ? argv[4] : argv[3]), *q = quoted(content);
                size_t n = strlen(q) + 100;
                char *out = malloc(n);
                snprintf(out, n, "{\"choices\": [{\"message\": {\"role\": \"assistant\", \"content\": %s}}]}", q);
                reply(c, out);
                free(out);
                free(q);
                free(content);
            }
        }
        free(req);
        close(c);
    }
}
