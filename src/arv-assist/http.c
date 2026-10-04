/* A small HTTP/1.1 client, for local model servers (Ollama, llama.cpp's llama-server, LM Studio):
 * http:// only (they speak plain HTTP on loopback), one request per connection, no proxies. */
#define _XOPEN_SOURCE 700
#include "assist.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct {
    char host[256], port[8], *path;
} url_parts;

/* http://host[:port]/path, [::1] for IPv6 */
static int parse_url(const char *url, url_parts *u, char **err)
{
    memset(u, 0, sizeof *u);
    if (!strncmp(url, "https://", 8)) {
        *err = xprintf("%s: https is not supported; local model servers speak http", url);
        return -1;
    }
    if (strncmp(url, "http://", 7)) {
        *err = xprintf("%s: not an http:// address", url);
        return -1;
    }
    const char *h = url + 7, *end = h + strcspn(h, "/"), *colon = NULL;
    if (*h == '[') {
        const char *close = strchr(h, ']');
        if (!close || close > end) { *err = xprintf("%s: bad address", url); return -1; }
        snprintf(u->host, sizeof u->host, "%.*s", (int)(close - h - 1), h + 1);
        if (close[1] == ':') colon = close + 1;
    } else {
        colon = memchr(h, ':', (size_t)(end - h));
        snprintf(u->host, sizeof u->host, "%.*s", (int)((colon ? colon : end) - h), h);
    }
    if (colon) snprintf(u->port, sizeof u->port, "%.*s", (int)(end - colon - 1), colon + 1);
    else strcpy(u->port, "80");
    u->path = xstrdup(*end ? end : "/");
    if (!*u->host) { *err = xprintf("%s: no host", url); free(u->path); return -1; }
    return 0;
}

int url_is_loopback(const char *url)
{
    url_parts u;
    char *err = NULL;
    if (parse_url(url, &u, &err)) { free(err); return 0; }
    free(u.path);
    if (!strcmp(u.host, "localhost")) return 1;
    struct addrinfo hints = { 0 }, *res, *a;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(u.host, NULL, &hints, &res)) return 0;
    int all = 1;
    for (a = res; a; a = a->ai_next) {
        if (a->ai_family == AF_INET) {
            const unsigned char *ip = (const unsigned char *)&((struct sockaddr_in *)a->ai_addr)->sin_addr;
            all &= ip[0] == 127;
        } else if (a->ai_family == AF_INET6) {
            const unsigned char *ip = ((struct sockaddr_in6 *)a->ai_addr)->sin6_addr.s6_addr;
            static const unsigned char one[16] = { [15] = 1 };
            all &= !memcmp(ip, one, 16);
        } else {
            all = 0;
        }
    }
    freeaddrinfo(res);
    return all;
}

static int send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* a chunked body, decoded in place; its new length */
static size_t unchunk(char *body, size_t len)
{
    char *in = body, *out = body, *end = body + len;
    while (in < end) {
        char *line_end = strstr(in, "\r\n");
        if (!line_end) break;
        unsigned long n = strtoul(in, NULL, 16);
        in = line_end + 2;
        if (!n || in + n > end) break;
        memmove(out, in, n);
        out += n;
        in += n + 2;
    }
    *out = 0;
    return (size_t)(out - body);
}

int http_request(const char *url, const char *method, const char *body, int timeout, int *status, char **reply, char **err)
{
    url_parts u;
    *reply = NULL;
    *status = 0;
    if (parse_url(url, &u, err)) return -1;
    struct addrinfo hints = { 0 }, *res, *a;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(u.host, u.port, &hints, &res), fd = -1;
    if (rc) {
        *err = xprintf("cannot find %s (%s)", u.host, gai_strerror(rc));
        free(u.path);
        return -1;
    }
    for (a = res; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = { timeout, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, a->ai_addr, a->ai_addrlen)) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd < 0) {
        *err = xprintf("cannot connect to %s:%s (%s)", u.host, u.port, strerror(errno));
        free(u.path);
        return -1;
    }
    sbuf req = { 0 };
    int v6 = strchr(u.host, ':') != NULL;
    sb_printf(&req, "%s %s HTTP/1.1\r\nHost: %s%s%s:%s\r\nAccept: application/json\r\nConnection: close\r\n", method, u.path,
              v6 ? "[" : "", u.host, v6 ? "]" : "", u.port);
    if (body) sb_printf(&req, "Content-Type: application/json\r\nContent-Length: %zu\r\n", strlen(body));
    sb_puts(&req, "\r\n");
    if (body) sb_puts(&req, body);
    free(u.path);
    if (send_all(fd, req.s, req.len)) {
        *err = xprintf("cannot send the request (%s)", strerror(errno));
        free(req.s);
        close(fd);
        return -1;
    }
    free(req.s);
    sbuf in = { 0 };
    char buf[65536];
    for (;;) {
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) {
            *err = xprintf("no answer (%s)", errno == EAGAIN || errno == EWOULDBLOCK ? "timed out" : strerror(errno));
            free(in.s);
            close(fd);
            return -1;
        }
        if (!r) break;
        sb_add(&in, buf, (size_t)r);
    }
    close(fd);
    char *head_end = in.s ? strstr(in.s, "\r\n\r\n") : NULL;
    if (!head_end || strncmp(in.s, "HTTP/1.", 7)) {
        *err = xstrdup("not an HTTP reply");
        free(in.s);
        return -1;
    }
    *status = atoi(in.s + 9);
    *head_end = 0;
    char *payload = head_end + 4;
    size_t len = in.len - (size_t)(payload - in.s);
    char *head = lower(in.s), *te = strstr(head, "\r\ntransfer-encoding:");     /* the headers */
    int chunked = 0;
    if (te) {
        char *eol = strstr(te + 2, "\r\n");
        if (eol) *eol = 0;
        chunked = strstr(te, "chunked") != NULL;
    }
    free(head);
    if (chunked) len = unchunk(payload, len);
    *reply = xmalloc(len + 1);
    memcpy(*reply, payload, len);
    (*reply)[len] = 0;
    free(in.s);
    return 0;
}
