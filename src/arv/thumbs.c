/*
 * Thumbnails on discs (research/plan.md, "thumbnails on discs"): a small picture of each photo, a
 * frame of each video and the first page of each PDF, so a found disc, the home, its index.html and
 * a model can see what a disc holds without reading it. JPEG, 384 px on the longer side, metadata
 * stripped, named by the SHA-256 of the file it shows (one for every copy of the same bytes):
 *
 *   catalog/volumes/<id>/thumbs/<sha256>.jpg
 *   catalog/volumes/<id>/thumbs.tsv     # arv thumbs 1  sha256  width  height  from  made by
 *
 * Made by tools built for it when they are here (principle 7): vipsthumbnail, else ImageMagick,
 * else ffmpeg, for pictures; ffmpeg for a video's frame; pdftoppm for a PDF's page. With none,
 * none are made. They are made once for a make, before its discs are laid out, and each disc
 * takes its own files' (at most a fiftieth of its medium). A disc carries its own, never other
 * discs': those stay at home, in the home's volumes/<id>/.
 */
#define _XOPEN_SOURCE 700
#include "arv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define SIDE "384"

static const char *const PICTURES[] = { "jpg", "jpeg", "png", "gif", "tif", "tiff", "webp", "bmp", "heic", "heif", "avif",
                                        "dng", "cr2", "cr3", "nef", "arw", "orf", "rw2", "raf", NULL };
static const char *const VIDEOS[] = { "mp4", "m4v", "mov", "mkv", "webm", "avi", "mts", "m2ts", "mpg", "mpeg", "wmv", "3gp", NULL };

static int ext_in(const char *path, const char *const *list)
{
    const char *slash = strrchr(path, '/'), *dot = strrchr(slash ? slash + 1 : path, '.');
    if (!dot) return 0;
    for (int i = 0; list[i]; i++)
        if (!strcasecmp(dot + 1, list[i])) return 1;
    return 0;
}

/* a JPEG's size, from its frame header; 0 when it is not one */
static int jpeg_size(const char *path, int *w, int *h)
{
    FILE *fp = fopen(path, "rb");
    unsigned char b[10];
    *w = *h = 0;
    if (!fp) return 0;
    if (fread(b, 1, 2, fp) != 2 || b[0] != 0xFF || b[1] != 0xD8) { fclose(fp); return 0; }
    for (;;) {
        int c = fgetc(fp);
        while (c == 0xFF) c = fgetc(fp);
        if (c == EOF) break;
        if (fread(b, 1, 2, fp) != 2) break;
        long len = (b[0] << 8) | b[1];
        if (c >= 0xC0 && c <= 0xCF && c != 0xC4 && c != 0xC8 && c != 0xCC) {
            if (fread(b, 1, 5, fp) != 5) break;
            *h = (b[1] << 8) | b[2];
            *w = (b[3] << 8) | b[4];
            break;
        }
        if (fseek(fp, len - 2, SEEK_CUR)) break;
    }
    fclose(fp);
    return *w > 0 && *h > 0;
}

static int ran(char **argv)                  /* 0 when it ran, and said all was well */
{
    char *out = NULL;
    int rc = run(argv, &out);
    free(out);
    return rc;
}

/* one thumbnail of SRC at OUT (a .jpg), by the first tool here that can; the tool, or NULL */
static const char *make_one(const char *src, const char *out, const char **from)
{
    struct stat st;
    char *qout = xprintf("%s[Q=75,strip]", out), *first = xprintf("%s[0]", src), *scale = xstrdup(
        "scale='min(" SIDE ",iw)':'min(" SIDE ",ih)':force_original_aspect_ratio=decrease");
    const char *tool = NULL;
    unlink(out);
    if (ext_in(src, PICTURES)) {
        *from = "picture";
        char *vips[] = { "vipsthumbnail", (char *)src, "-s", SIDE, "-o", qout, NULL };
        char *magick[] = { "magick", first, "-auto-orient", "-thumbnail", SIDE "x" SIDE ">", "-strip", "-quality", "75", (char *)out, NULL };
        char *convert[] = { "convert", first, "-auto-orient", "-thumbnail", SIDE "x" SIDE ">", "-strip", "-quality", "75", (char *)out, NULL };
        char *ff[] = { "ffmpeg", "-v", "error", "-y", "-i", (char *)src, "-frames:v", "1", "-vf", scale, "-map_metadata", "-1",
                       "-q:v", "5", (char *)out, NULL };
        char **tries[] = { vips, magick, convert, ff };
        for (int i = 0; i < 4 && !tool; i++)
            if (on_path(tries[i][0]) && !ran(tries[i]) && !stat(out, &st) && st.st_size > 0) tool = tries[i][0];
    } else if (ext_in(src, VIDEOS)) {
        *from = "frame";
        char *seek[] = { "ffmpeg", "-v", "error", "-y", "-ss", "3", "-i", (char *)src, "-frames:v", "1", "-vf", scale,
                         "-map_metadata", "-1", "-q:v", "5", (char *)out, NULL };
        char *start[] = { "ffmpeg", "-v", "error", "-y", "-i", (char *)src, "-frames:v", "1", "-vf", scale,
                          "-map_metadata", "-1", "-q:v", "5", (char *)out, NULL };
        if (on_path("ffmpeg") && ((!ran(seek) && !stat(out, &st) && st.st_size > 0)       /* a short clip: its first frame */
                                  || (!ran(start) && !stat(out, &st) && st.st_size > 0)))
            tool = "ffmpeg";
    } else if (ext_in(src, (const char *const[]){ "pdf", NULL })) {
        *from = "page 1";
        char *base = xstrdup(out);
        base[strlen(base) - 4] = 0;                 /* pdftoppm adds .jpg */
        char *pp[] = { "pdftoppm", "-jpeg", "-jpegopt", "quality=75", "-scale-to", SIDE, "-f", "1", "-l", "1", "-singlefile",
                       (char *)src, base, NULL };
        if (on_path("pdftoppm") && !ran(pp) && !stat(out, &st) && st.st_size > 0) tool = "pdftoppm";
        free(base);
    }
    free(qout);
    free(first);
    free(scale);
    if (!tool) unlink(out);
    return tool;
}

int thumbs_kind(const char *path)
{
    return ext_in(path, PICTURES) || ext_in(path, VIDEOS) || ext_in(path, (const char *const[]){ "pdf", NULL });
}

void thumbs_prepare(const entries *files, const entries *members, const char *workdir, thumbset *out)
{
    memset(out, 0, sizeof *out);
    out->dir = join(workdir, "thumbs");
    if (mkdirs(out->dir)) die("cannot create %s", out->dir);
    const entries *lists[] = { files, members };
    size_t wanted = 0, done = 0;
    for (int l = 0; l < 2; l++)
        for (size_t i = 0; lists[l] && i < lists[l]->n; i++) wanted += !*lists[l]->v[i].link && thumbs_kind(lists[l]->v[i].path);
    if (!wanted) return;
    fprintf(stderr, "Making thumbnails of %zu picture%s, video%s and PDF%s ...\n", wanted, wanted == 1 ? "" : "s",
            wanted == 1 ? "" : "s", wanted == 1 ? "" : "s");
    for (int l = 0; l < 2; l++)
        for (size_t i = 0; lists[l] && i < lists[l]->n; i++) {
            const entry *e = &lists[l]->v[i];
            if (*e->link || e->is_pack || !thumbs_kind(e->path) || !e->sha256[0] || thumbs_find(out, e->sha256)) continue;
            char *path = xprintf("%s/%s.jpg", out->dir, e->sha256);
            const char *from = NULL, *tool = make_one(e->source ? e->source : e->path, path, &from);
            int w, h;
            done++;
            if (isatty(2)) fprintf(stderr, "\r  %zu of %zu", done, wanted);
            if (tool && jpeg_size(path, &w, &h)) {
                out->v = xrealloc(out->v, (out->n + 1) * sizeof *out->v);
                thumb *t = &out->v[out->n++];
                memcpy(t->sha, e->sha256, sizeof t->sha);
                t->w = w;
                t->h = h;
                t->from = from;
                t->tool = tool;
                struct stat st;
                t->bytes = !stat(path, &st) ? (uint64_t)st.st_size : 0;
            } else {
                out->failed++;
                unlink(path);
            }
            free(path);
        }
    if (isatty(2)) fputc('\n', stderr);
    if (!out->n) fprintf(stderr, "No thumbnails: no tool here could make them (vipsthumbnail, ImageMagick, ffmpeg, pdftoppm; "
                                 "arv programs says how to install them)\n");
}

const thumb *thumbs_find(const thumbset *t, const char *sha)
{
    for (size_t i = 0; t && i < t->n; i++)
        if (!strcmp(t->v[i].sha, sha)) return &t->v[i];
    return NULL;
}

static int by_sha(const void *a, const void *b)
{
    return strcmp(((const thumb *)a)->sha, ((const thumb *)b)->sha);
}

void thumbs_for_disc(const thumbset *all, const entries *described, uint64_t budget, const char *vol_dir, thumbset *disc)
{
    memset(disc, 0, sizeof *disc);
    uint64_t used = 0;
    for (size_t i = 0; all && i < described->n; i++) {
        const thumb *t = thumbs_find(all, described->v[i].sha256);
        if (!t || thumbs_find(disc, t->sha)) continue;
        if (budget && used + t->bytes > budget) { disc->left_out++; continue; }
        used += t->bytes;
        disc->v = xrealloc(disc->v, (disc->n + 1) * sizeof *disc->v);
        disc->v[disc->n++] = *t;
    }
    if (!disc->n) return;
    qsort(disc->v, disc->n, sizeof *disc->v, by_sha);
    disc->dir = join(vol_dir, "thumbs");
    if (mkdirs(disc->dir)) die("cannot create %s", disc->dir);
    sbuf tsv = { 0 };
    sb_puts(&tsv, "# arv thumbs 1\tsha256\twidth\theight\tfrom\tmade by\n");
    for (size_t i = 0; i < disc->n; i++) {
        const thumb *t = &disc->v[i];
        char *from = xprintf("%s/%s.jpg", all->dir, t->sha), *to = xprintf("%s/%s.jpg", disc->dir, t->sha);
        copy_file(from, to);
        sb_printf(&tsv, "%s\t%d\t%d\t%s\t%s\n", t->sha, t->w, t->h, t->from, t->tool);
        free(from);
        free(to);
    }
    char *path = join(vol_dir, "thumbs.tsv");
    write_text(path, tsv.s);
    free(path);
    free(tsv.s);
    disc->tool = disc->v[0].tool;
}

void thumbs_free(thumbset *t)
{
    free(t->v);
    free(t->dir);
    memset(t, 0, sizeof *t);
}
