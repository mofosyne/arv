/*
 * Optional: a local vision model looks at a few sample images per folder (and one frame per
 * sampled video, with ffmpeg), so the text model and the folder tags can use what they show
 * (vision.py). Local servers only, whatever the options say: these are private pictures.
 * Images are scaled down by ffmpeg when it is installed; otherwise JPEG, PNG, WebP and GIF files
 * up to 4 MiB are sent as they are.
 */
#define _XOPEN_SOURCE 700
#include "assist.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_PASSTHROUGH (4 * 1024 * 1024)
#define MAX_SIDE 768

static const char PROMPT[] =
    "Describe this image for a personal archive catalogue. Only describe "
    "what is visible; do not guess names of people or places unless readable text "
    "in the image says so.\n\n"
    "Answer in exactly two lines, like this example:\n"
    "Caption: Two children building a sandcastle on a sunny beach.\n"
    "Tags: beach, children, sandcastle, summer";

/* text a small model may copy from the instructions instead of answering */
static const char *const ECHOES[] = { "two children building a sandcastle", "one short sentence", "short lowercase tags", NULL };

static const char *const IMAGE_EXTS[] = { ".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp", ".tif", ".tiff", ".heic", NULL };
static const char *const VIDEO_EXTS[] = { ".mp4", ".mov", ".mkv", ".avi", ".m4v", ".mts", ".webm", NULL };

static const char *ext(const char *path)
{
    const char *b = strrchr(path, '/'), *dot;
    b = b ? b + 1 : path;
    dot = strrchr(b, '.');
    return dot && dot != b ? dot : "";
}

static int in(const char *e, const char *const *list)
{
    for (int i = 0; list[i]; i++)
        if (!strcasecmp(e, list[i])) return 1;
    return 0;
}

/* ------------------------------------------------------------------ sampling */

/* up to per_folder evenly spaced images (and videos) per folder, max_total in all */
void vision_sample(const items *l, int per_folder, int max_total, items *out)
{
    memset(out, 0, sizeof *out);
    /* the candidates, by path (items are already in walk order; sort a copy by path) */
    size_t *idx = xmalloc((l->n + 1) * sizeof *idx), n = 0;
    for (size_t i = 0; i < l->n; i++) {
        const char *e = ext(l->v[i].path);
        if ((in(e, IMAGE_EXTS) || in(e, VIDEO_EXTS)) && l->v[i].size >= 1024) idx[n++] = i;
    }
    for (size_t i = 1; i < n; i++)          /* insertion sort by path: sample lists are small */
        for (size_t j = i; j > 0 && strcmp(l->v[idx[j - 1]].path, l->v[idx[j]].path) > 0; j--) {
            size_t t = idx[j];
            idx[j] = idx[j - 1];
            idx[j - 1] = t;
        }
    /* folders in order of first appearance, each with its files in path order */
    strlist folders = { 0 };
    for (size_t i = 0; i < n; i++) {
        const char *p = l->v[idx[i]].path, *slash = strrchr(p, '/');
        char *f = slash ? xprintf("%.*s", (int)(slash - p), p) : xstrdup(".");
        if (!strlist_has(&folders, f)) strlist_add(&folders, f);
        free(f);
    }
    int total = 0;
    size_t *mine = xmalloc((n + 1) * sizeof *mine);
    for (size_t fi = 0; fi < folders.n && total < max_total; fi++) {
        size_t count = 0;
        for (size_t i = 0; i < n; i++) {
            const char *p = l->v[idx[i]].path, *slash = strrchr(p, '/');
            size_t flen = slash ? (size_t)(slash - p) : 0;
            int here = slash ? strlen(folders.v[fi]) == flen && !strncmp(p, folders.v[fi], flen) : !strcmp(folders.v[fi], ".");
            if (here) mine[count++] = idx[i];
        }
        int take = per_folder < (int)count ? per_folder : (int)count;
        if (take > max_total - total) take = max_total - total;
        double step = (double)count / take;
        for (int k = 0; k < take; k++) {
            const item *it = &l->v[mine[(size_t)(k * step)]];
            out->v = xrealloc(out->v, (out->n + 1) * sizeof *out->v);
            out->v[out->n].path = xstrdup(it->path);
            out->v[out->n].size = it->size;
            out->v[out->n].mtime = it->mtime;
            out->n++;
        }
        total += take;
    }
    free(mine);
    strlist_free(&folders);
    free(idx);
}

/* ------------------------------------------------------------------ image bytes */

/* a program's standard output, as bytes (stdin and stderr: /dev/null); 0 when it succeeded */
static int run_bytes(char *const argv[], unsigned char **data, size_t *len)
{
    int fds[2];
    *data = NULL;
    *len = 0;
    if (pipe(fds)) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (!pid) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(null, 2);
        dup2(fds[1], 1);
        close(fds[0]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    sbuf b = { 0 };
    char buf[65536];
    ssize_t r;
    while ((r = read(fds[0], buf, sizeof buf)) > 0) sb_add(&b, buf, (size_t)r);
    close(fds[0]);
    int st;
    waitpid(pid, &st, 0);
    *data = (unsigned char *)b.s;
    *len = b.len;
    return WIFEXITED(st) && !WEXITSTATUS(st) && b.len ? 0 : -1;
}

static int ffmpeg_frame(const char *path, int video, unsigned char **data, size_t *len)
{
    if (!on_path("ffmpeg")) return -1;
    char scale[64];
    snprintf(scale, sizeof scale, "scale='min(%d,iw)':-2", MAX_SIDE);
    char *with_seek[] = { "ffmpeg", "-v", "error", "-ss", "5", "-i", (char *)path, "-frames:v", "1", "-vf", scale,
                          "-f", "image2pipe", "-vcodec", "mjpeg", "-", NULL };
    char *plain[] = { "ffmpeg", "-v", "error", "-i", (char *)path, "-frames:v", "1", "-vf", scale,
                      "-f", "image2pipe", "-vcodec", "mjpeg", "-", NULL };
    if (!run_bytes(video ? with_seek : plain, data, len)) return 0;
    free(*data);
    if (!video) return -1;
    return run_bytes(plain, data, len);         /* a short clip: its first frame */
}

/* bytes small enough to send, and their type; -1 when this file cannot be shown */
static int image_data(const char *path, unsigned char **data, size_t *len, const char **mime)
{
    const char *e = ext(path);
    *mime = "image/jpeg";
    if (in(e, VIDEO_EXTS)) return ffmpeg_frame(path, 1, data, len);
    if (!ffmpeg_frame(path, 0, data, len)) return 0;
    static const char *const pass[][2] = { { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".png", "image/png" },
                                           { ".webp", "image/webp" }, { ".gif", "image/gif" } };
    struct stat st;
    for (size_t i = 0; i < sizeof pass / sizeof *pass; i++)
        if (!strcasecmp(e, pass[i][0]) && !stat(path, &st) && st.st_size <= MAX_PASSTHROUGH) {
            FILE *fp = fopen(path, "rb");
            if (!fp) return -1;
            *data = xmalloc((size_t)st.st_size + 1);
            *len = fread(*data, 1, (size_t)st.st_size, fp);
            fclose(fp);
            *mime = pass[i][1];
            return 0;
        }
    return -1;
}

static char *base64(const unsigned char *p, size_t n)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = xmalloc(4 * ((n + 2) / 3) + 1), *o = out;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)p[i] << 16 | (i + 1 < n ? (unsigned)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        *o++ = t[v >> 18 & 63];
        *o++ = t[v >> 12 & 63];
        *o++ = i + 1 < n ? t[v >> 6 & 63] : '=';
        *o++ = i + 2 < n ? t[v & 63] : '=';
    }
    *o = 0;
    return out;
}

/* ------------------------------------------------------------------ the replies */

static char *collapse(const char *s)
{
    sbuf b = { 0 };
    int space = 0;
    for (; *s; s++) {
        if (isspace((unsigned char)*s)) { space = b.len > 0; continue; }
        if (space) sb_puts(&b, " ");
        space = 0;
        sb_add(&b, s, 1);
    }
    return b.s ? b.s : xstrdup("");
}

/* the rest of the line after "<word>s? [:=]", searched case-insensitively; NULL if none */
static char *labelled(const char *content, const char *word, int plural)
{
    size_t n = strlen(word);
    for (const char *p = content; *p; p++) {
        if (strncasecmp(p, word, n)) continue;
        const char *q = p + n;
        if (plural && (*q == 's' || *q == 'S')) q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != ':' && *q != '=') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        const char *e = q + strcspn(q, "\n");
        if (e > q) return xprintf("%.*s", (int)(e - q), q);
    }
    return NULL;
}

static void split_tags(const char *s, strlist *out)
{
    char *copy = xstrdup(s);
    for (char *t = copy, *next; t; t = next) {
        next = strpbrk(t, ",;");
        if (next) *next++ = 0;
        strlist_add(out, t);
    }
    free(copy);
}

/* a 'Caption: / Tags:' reply; JSON and bare text are accepted too */
void vision_parse_reply(const char *content, char **caption_out, strlist *tags_out)
{
    char *caption = NULL;
    strlist raw = { 0 };
    char *c = labelled(content, "caption", 0), *t = labelled(content, "tag", 1);
    if (c || t) {
        caption = c ? c : xstrdup("");
        if (t) split_tags(t, &raw);
        free(t);
    } else {
        const char *open = strchr(content, '{'), *close = strrchr(content, '}');
        jv *d = NULL;
        if (open && close && close > open) {
            char *body = xprintf("%.*s", (int)(close - open + 1), open);
            d = json_parse(body);
            free(body);
        }
        if (d && d->kind == 'o' && d->n) {
            const char *cap = jstr_of(json_get(d, "caption"));
            caption = xstrdup(cap ? cap : "");
            const jv *tv = json_get(d, "tags");
            if (tv && tv->kind == 's') split_tags(tv->str, &raw);
            for (size_t i = 0; tv && tv->kind == 'a' && i < tv->n; i++)
                if (tv->vals[i]->kind == 's') strlist_add(&raw, tv->vals[i]->str);
        } else {
            const char *s = content;
            while (isspace((unsigned char)*s)) s++;
            caption = xstrdup(*s == '{' ? "" : content);      /* the model ignored the format: still a caption */
        }
        jfree(d);
    }
    /* the caption: one line, without quotes, at most 200 bytes */
    char *col = collapse(caption);
    free(caption);
    char *a = col, *e = col + strlen(col);
    while (*a == '"') a++;
    while (e > a && e[-1] == '"') e--;
    while (a < e && *a == ' ') a++;
    while (e > a && e[-1] == ' ') e--;
    size_t len = (size_t)(e - a) > 200 ? 200 : (size_t)(e - a);
    while (len && ((unsigned char)a[len] & 0xC0) == 0x80 && len < (size_t)(e - a)) len--;
    caption = xprintf("%.*s", (int)len, a);
    free(col);
    strlist tags = { 0 };
    for (size_t i = 0; i < raw.n; i++) {
        char *x = collapse(raw.v[i]), *s = x, *xe = x + strlen(x);
        while (*s == '"' || *s == '.') s++;
        while (xe > s && (xe[-1] == '"' || xe[-1] == '.')) xe--;
        *xe = 0;
        for (char *q = s; *q; q++) *q = (char)tolower((unsigned char)*q);
        if (strlen(s) > 30) s[30] = 0;
        if (*s && !strlist_has(&tags, s)) strlist_add(&tags, s);
        free(x);
    }
    strlist_free(&raw);
    /* an answer copied from the instructions says nothing */
    sbuf all = { 0 };
    sb_puts(&all, caption);
    for (size_t i = 0; i < tags.n; i++) sb_printf(&all, " %s", tags.v[i]);
    char *low = lower(all.s);
    int echo = 0;
    for (int i = 0; ECHOES[i]; i++) echo |= strstr(low, ECHOES[i]) != NULL;
    free(low);
    free(all.s);
    memset(tags_out, 0, sizeof *tags_out);
    if (echo) {
        *caption_out = xstrdup("");
        strlist_free(&tags);
        free(caption);
        return;
    }
    *caption_out = caption;
    for (size_t i = 0; i < tags.n && i < 6; i++) strlist_add(tags_out, tags.v[i]);
    strlist_free(&tags);
}

/* ------------------------------------------------------------------ looking */

static int look(llm_client *c, const unsigned char *data, size_t len, const char *mime, char **caption, strlist *tags, char **err)
{
    char *b64 = base64(data, len), *url = xprintf("data:%s;base64,%s", mime, b64);
    free(b64);
    jv *text = jobj(), *image = jobj(), *iu = jobj(), *content = jarr(), *msg = jobj(), *messages = jarr();
    jput(text, "type", jstr("text"));
    jput(text, "text", jstr(PROMPT));
    jput(iu, "url", jstr(url));
    free(url);
    jput(image, "type", jstr("image_url"));
    jput(image, "image_url", iu);
    jpush(content, text);
    jpush(content, image);
    jput(msg, "role", jstr("user"));
    jput(msg, "content", content);
    jpush(messages, msg);
    char *reply = llm_chat(c, messages, "0.1", 0, err);
    if (!reply) {
        char *low = lower(*err);
        if (strstr(low, "image") || strstr(low, "multimodal")) {
            char *e = xprintf("the model %s does not accept images (%s); choose a vision model with --vision-model",
                              c->model ? c->model : "?", *err);
            free(*err);
            *err = e;
        }
        free(low);
        return -1;
    }
    vision_parse_reply(reply, caption, tags);
    free(reply);
    return 0;
}

int vision_analyse(llm_client *c, const char *root, const items *l, int per_folder, int max_total, seen *out, char **err)
{
    memset(out, 0, sizeof *out);
    items picked;
    vision_sample(l, per_folder, max_total, &picked);
    for (size_t i = 0; i < picked.n;) {
        const char *p = picked.v[i].path, *slash = strrchr(p, '/');
        char *folder = slash ? xprintf("%.*s", (int)(slash - p), p) : xstrdup(".");
        strlist captions = { 0 }, tags = { 0 };
        size_t *counts = NULL, j = i;
        for (; j < picked.n; j++) {
            const char *q = picked.v[j].path, *qs = strrchr(q, '/');
            char *qf = qs ? xprintf("%.*s", (int)(qs - q), q) : xstrdup(".");
            int same = !strcmp(qf, folder);
            free(qf);
            if (!same) break;
            if (isatty(2)) fprintf(stderr, "\rLooking at %zu/%zu: %-80.80s", j + 1, picked.n, q);
            char *full = join(root, q), *caption = NULL;
            unsigned char *data;
            size_t len;
            const char *mime;
            strlist t = { 0 };
            if (!image_data(full, &data, &len, &mime)) {
                int rc = look(c, data, len, mime, &caption, &t, err);
                free(data);
                if (rc) {
                    free(full);
                    free(folder);
                    strlist_free(&captions);
                    strlist_free(&tags);
                    free(counts);
                    for (size_t k = 0; k < picked.n; k++) free(picked.v[k].path);
                    free(picked.v);
                    return -1;
                }
                if (*caption) strlist_add(&captions, caption);
                for (size_t k = 0; k < t.n; k++) {          /* a Counter, in order of first appearance */
                    size_t m = 0;
                    while (m < tags.n && strcmp(tags.v[m], t.v[k])) m++;
                    if (m == tags.n) {
                        strlist_add(&tags, t.v[k]);
                        counts = xrealloc(counts, tags.n * sizeof *counts);
                        counts[m] = 0;
                    }
                    counts[m]++;
                }
                free(caption);
                strlist_free(&t);
            }
            free(full);
        }
        if (captions.n || tags.n) {
            out->v = xrealloc(out->v, (out->n + 1) * sizeof *out->v);
            seen_folder *s = &out->v[out->n++];
            memset(s, 0, sizeof *s);
            s->folder = xstrdup(folder);
            sbuf cb = { 0 };
            for (size_t k = 0; k < captions.n && k < 3; k++) sb_printf(&cb, "%s%s", k ? " / " : "", captions.v[k]);
            s->caption = cb.s ? cb.s : xstrdup("");
            for (int k = 0; k < 6; k++) {                   /* most_common(6) */
                size_t best = tags.n;
                for (size_t m = 0; m < tags.n; m++)
                    if (counts[m] && (best == tags.n || counts[m] > counts[best])) best = m;
                if (best == tags.n) break;
                strlist_add(&s->tags, tags.v[best]);
                counts[best] = 0;
            }
            s->images = j - i;
        }
        strlist_free(&captions);
        strlist_free(&tags);
        free(counts);
        free(folder);
        i = j;
    }
    if (isatty(2) && picked.n) fputs("\n", stderr);
    for (size_t k = 0; k < picked.n; k++) free(picked.v[k].path);
    free(picked.v);
    return 0;
}

char *vision_inventory_section(const seen *s)
{
    if (!s || !s->n) return xstrdup("");
    sbuf b = { 0 };
    sb_puts(&b, "\n\nWhat sampled images show (from a local vision model; may be imperfect):");
    for (size_t i = 0; i < s->n; i++) {
        sb_printf(&b, "\n  %s/ (%zu sampled): %s [", s->v[i].folder, s->v[i].images, s->v[i].caption);
        for (size_t k = 0; k < s->v[i].tags.n; k++) sb_printf(&b, "%s%s", k ? ", " : "", s->v[i].tags.v[k]);
        sb_puts(&b, "]");
    }
    return b.s;
}

/* describe.with_vision: the images' tags added to the folder tags (up to 8), and their captions */
void vision_merge(suggestion *sug, const seen *v)
{
    for (size_t i = 0; v && i < v->n; i++) {
        strlist *tags = ftags_get(&sug->folders, v->v[i].folder, 1);
        for (size_t k = 0; k < v->v[i].tags.n; k++)
            if (!strlist_has(tags, v->v[i].tags.v[k]) && tags->n < 8) strlist_add(tags, v->v[i].tags.v[k]);
        if (*v->v[i].caption) ftags_set_caption(&sug->folders, v->v[i].folder, v->v[i].caption);
    }
}

jv *seen_json(const seen *s)
{
    jv *o = jobj();
    for (size_t i = 0; s && i < s->n; i++) {
        jv *f = jobj();
        jput(f, "caption", jstr(s->v[i].caption));
        jput(f, "tags", jstrings(&s->v[i].tags));
        char n[32];
        snprintf(n, sizeof n, "%zu", s->v[i].images);
        jput(f, "images", jnum(n));
        jput(o, s->v[i].folder, f);
    }
    return o;
}

void seen_from_json(const jv *v, seen *out)
{
    memset(out, 0, sizeof *out);
    for (size_t i = 0; v && v->kind == 'o' && i < v->n; i++) {
        out->v = xrealloc(out->v, (out->n + 1) * sizeof *out->v);
        seen_folder *s = &out->v[out->n++];
        memset(s, 0, sizeof *s);
        s->folder = xstrdup(v->keys[i]);
        const char *cap = jstr_of(json_get(v->vals[i], "caption"));
        s->caption = xstrdup(cap ? cap : "");
        const jv *t = json_get(v->vals[i], "tags"), *n = json_get(v->vals[i], "images");
        for (size_t k = 0; t && t->kind == 'a' && k < t->n; k++)
            if (t->vals[k]->kind == 's') strlist_add(&s->tags, t->vals[k]->str);
        s->images = n && n->kind == 't' ? (size_t)strtoul(n->str, NULL, 10) : 0;
    }
}

void seen_free(seen *s)
{
    for (size_t i = 0; i < s->n; i++) {
        free(s->v[i].folder);
        free(s->v[i].caption);
        strlist_free(&s->v[i].tags);
    }
    free(s->v);
    memset(s, 0, sizeof *s);
}
