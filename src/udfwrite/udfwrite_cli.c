/*
 * udfwrite: make a closed, read-only UDF 2.50 image of a folder (docs/spec/archival-udf.md).
 *
 *   udfwrite [-V volume-id] [-L label] [-S volume-set] [-t time] [-x extents.tsv] IMAGE FOLDER
 *
 * Symbolic links are followed (arv stages a folder of links to the files it archives); anything
 * that is not a regular file or a folder is refused. -t fixes the volume's recording time
 * (seconds since 1970, UTC) so that the same folder always gives the same image.
 */
#define _POSIX_C_SOURCE 200809L
#include "udfwrite.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char **sources;          /* disk path of each file, by index */
static size_t nsources, capsources;
static FILE *open_file;         /* the library reads files one after another: keep one open */
static size_t open_index;

static void die(const char *msg)
{
    fprintf(stderr, "udfwrite: %s\n", msg);
    exit(1);
}

static long read_file(void *ctx, uint64_t offset, void *buf, size_t len)
{
    size_t index = (size_t)(uintptr_t)ctx;
    if (!open_file || open_index != index) {
        if (open_file) fclose(open_file);
        open_index = index;
        if (!(open_file = fopen(sources[index], "rb"))) return -1;
    }
    if (fseeko(open_file, (off_t)offset, SEEK_SET)) return -1;
    return (long)fread(buf, 1, len, open_file);
}

static int write_out(void *ctx, const void *buf, size_t count)
{
    return fwrite(buf, UDFW_SECTOR, count, ctx) == count ? 0 : -1;
}

static void extent_line(void *ctx, const char *path, uint64_t sector, uint64_t size)
{
    fprintf(ctx, "%llu\t%llu\t%s\n", (unsigned long long)sector, (unsigned long long)size, path);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Adds everything under disk folder `dir` as image path `rel` ("" for the top). */
static void walk(udfw *w, const char *dir, const char *rel)
{
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "udfwrite: %s: %s\n", dir, strerror(errno)); exit(1); }
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            if (!(names = realloc(names, cap * sizeof *names))) die("out of memory");
        }
        if (!(names[n++] = strdup(e->d_name))) die("out of memory");
    }
    closedir(d);
    qsort(names, n, sizeof *names, by_name);
    for (size_t i = 0; i < n; i++) {
        size_t dl = strlen(dir), rl = strlen(rel), nl = strlen(names[i]);
        char *disk = malloc(dl + nl + 2), *path = malloc(rl + nl + 2);
        if (!disk || !path) die("out of memory");
        sprintf(disk, "%s/%s", dir, names[i]);
        if (rl) sprintf(path, "%s/%s", rel, names[i]); else strcpy(path, names[i]);
        struct stat st;
        if (stat(disk, &st)) { fprintf(stderr, "udfwrite: %s: %s\n", disk, strerror(errno)); exit(1); }
        if (S_ISDIR(st.st_mode)) {
            if (udfw_add_dir(w, path, (int64_t)st.st_mtime)) die(udfw_error(w));
            walk(w, disk, path);
            free(disk);
        } else if (S_ISREG(st.st_mode)) {
            if (nsources == capsources) {
                capsources = capsources ? capsources * 2 : 256;
                if (!(sources = realloc(sources, capsources * sizeof *sources))) die("out of memory");
            }
            sources[nsources] = disk;
            if (udfw_add_file(w, path, (uint64_t)st.st_size, (int64_t)st.st_mtime, (unsigned)st.st_mode, read_file,
                              (void *)(uintptr_t)nsources))
                die(udfw_error(w));
            nsources++;
        } else {
            fprintf(stderr, "udfwrite: %s: not a regular file or a folder\n", disk);
            exit(1);
        }
        free(path);
        free(names[i]);
    }
    free(names);
}

static void usage(void)
{
    fputs("usage: udfwrite [-V volume-id] [-L label] [-S volume-set] [-t time] [-x extents.tsv] IMAGE FOLDER\n",
          stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    udfw_options opt = { 0 };
    const char *extents = NULL;
    int c;
    opt.time = (int64_t)time(NULL);
    while ((c = getopt(argc, argv, "V:L:S:t:x:")) != -1) {
        switch (c) {
        case 'V': opt.volume_id = optarg; break;
        case 'L': opt.label = optarg; break;
        case 'S': opt.volume_set = optarg; break;
        case 't': opt.time = strtoll(optarg, NULL, 10); break;
        case 'x': extents = optarg; break;
        default: usage();
        }
    }
    if (argc - optind != 2) usage();
    const char *image = argv[optind], *folder = argv[optind + 1];
    if (!opt.volume_id) opt.volume_id = "ARCHIVE";

    udfw *w = udfw_new(&opt);
    if (!w) die("out of memory");
    walk(w, folder, "");

    FILE *out = fopen(image, "wb");
    if (!out) { perror(image); return 1; }
    FILE *ext = NULL;
    if (extents) {
        if (!(ext = fopen(extents, "w"))) { perror(extents); return 1; }
        fputs("# arv extents 1\tstart sector\tsize (bytes)\tpath\n", ext);
    }
    int rc = udfw_write(w, write_out, out, ext ? extent_line : NULL, ext);
    if (open_file) fclose(open_file);
    if (rc) fprintf(stderr, "udfwrite: %s\n", udfw_error(w));
    if (fclose(out) || (ext && fclose(ext))) { perror("udfwrite"); rc = -1; }
    if (rc) remove(image);
    udfw_free(w);
    return rc ? 1 : 0;
}
