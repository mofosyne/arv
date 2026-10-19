/*
 * fake-llm: a stand-in OpenAI-compatible server for arv-assist's checks, on 127.0.0.1.
 *
 *   fake-llm PORT-FILE LOG-FILE TEXT-REPLY-FILE IMAGE-REPLY-FILE
 *
 * Writes the port it listens on to PORT-FILE, then serves until killed:
 *   GET  /v1/models             {"data": [{"id": "fake-model"}]}
 *   POST /v1/chat/completions   the content of IMAGE-REPLY-FILE when the request shows an image,
 *                               else of TEXT-REPLY-FILE (both read afresh for each request); a
 *                               TEXT-REPLY-FILE in parts, separated by lines "===", gives its first
 *                               part to the first request, its second to the second, and so on
 *                               (the last part once they run out), for a conversation; counted
 *                               afresh whenever the file changes
 *                               A part's first line "logprob: -0.1" is not sent: with "logprobs"
 *                               asked for, the reply has them, every 4 characters a token of that
 *                               log-probability
 *   POST /v1/embeddings         bag-of-words vectors: each word (a-z) adds 1 to one of 64 numbers,
 *                               chosen by a hash of the word; then L2-normalised
 * Every request body is appended to LOG-FILE, one per line, and an Authorization header as
 * "auth: VALUE" before it.
 */
#define _XOPEN_SOURCE 700
#include <arpa/inet.h>
#include <math.h>
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
        double v[64] = { 0 }, norm = 0;
        for (char *t = text; *t;) {             /* each word a-z, lowercased, adds 1 at its hash */
            while (*t && !((*t >= 'a' && *t <= 'z') || (*t >= 'A' && *t <= 'Z'))) t++;
            unsigned long h = 0;
            int any = 0;
            for (; (*t >= 'a' && *t <= 'z') || (*t >= 'A' && *t <= 'Z'); t++, any = 1)
                h = (h * 31 + (unsigned long)(*t >= 'A' && *t <= 'Z' ? *t + 32 : *t)) % 1000003;
            if (any) v[h % 64] += 1;
        }
        free(text);
        for (int k = 0; k < 64; k++) norm += v[k] * v[k];
        norm = norm > 0 ? sqrt(norm) : 1;
        if (len + 64 * 12 + 64 > cap) out = realloc(out, cap = (cap + 64 * 12 + 64) * 2);
        len += (size_t)sprintf(out + len, "%s{\"index\": %d, \"embedding\": [", index ? ", " : "", index);
        for (int k = 0; k < 64; k++) len += (size_t)sprintf(out + len, "%s%.6f", k ? ", " : "", v[k] / norm);
        len += (size_t)sprintf(out + len, "]}");
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
        char *auth = strstr(req, "\r\nAuthorization: ");
        if (auth && auth < body) {
            FILE *log = fopen(argv[2], "a");
            if (log) {
                fprintf(log, "auth: %.*s\n", (int)strcspn(auth + 17, "\r\n"), auth + 17);
                fclose(log);
            }
        }
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
                static int asked;               /* text requests since the file changed: which part */
                static char *before;
                int image = strstr(body, "image_url") != NULL;
                char *content = slurp(image ? argv[4] : argv[3]);
                if (!image && (!before || strcmp(before, content))) {
                    free(before);
                    before = strdup(content);
                    asked = 0;
                }
                if (!image) {
                    char *part = content;
                    for (int k = 0; k < asked; k++) {
                        char *next = strstr(part, "\n===\n");
                        if (!next) break;
                        part = next + 5;
                    }
                    char *end = strstr(part, "\n===\n");
                    if (end) *end = 0;
                    memmove(content, part, strlen(part) + 1);
                    asked++;
                }
                double lp = 0;
                int have_lp = !strncmp(content, "logprob: ", 9);
                if (have_lp) {
                    lp = atof(content + 9);
                    char *nl = strchr(content, '\n');
                    memmove(content, nl ? nl + 1 : content + strlen(content), strlen(nl ? nl + 1 : "") + 1);
                }
                char *q = quoted(content), *probs = strdup("");
                if (have_lp && strstr(body, "\"logprobs\": true")) {
                    size_t clen = strlen(content), at = 0, m = 64;
                    free(probs);
                    probs = malloc(m);
                    strcpy(probs, ", \"logprobs\": {\"content\": [");
                    for (size_t i = 0; i < clen; i += 4) {
                        char piece[5] = { 0 };
                        memcpy(piece, content + i, clen - i < 4 ? clen - i : 4);
                        char *t = quoted(piece);
                        size_t need = strlen(probs) + strlen(t) + 64;
                        if (need > m) probs = realloc(probs, m = 2 * need);
                        at = strlen(probs);
                        snprintf(probs + at, m - at, "%s{\"token\": %s, \"logprob\": %g}", i ? ", " : "", t, lp);
                        free(t);
                    }
                    strcat(probs, "]}");
                }
                size_t n = strlen(q) + strlen(probs) + 100;
                char *out = malloc(n);
                snprintf(out, n, "{\"choices\": [{\"message\": {\"role\": \"assistant\", \"content\": %s}%s}]}", q, probs);
                free(probs);
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
