/*
 * udfwrite: writes closed, read-only UDF 2.50 images for archives.
 *
 * The profile it writes is docs/archival-udf.md: a metadata partition with a real mirror, every
 * file in one contiguous run in a fixed order, nothing taken from the clock or the machine, so
 * the same tree gives the same bytes. No dependencies beyond the C library; no file system
 * access of its own: the caller adds folders and files, supplies file bytes through a read
 * callback, and receives the image sector by sector through a write callback.
 *
 *     udfw *w = udfw_new(&(udfw_options){ .volume_id = "TRIP-01_2019_4", ... });
 *     udfw_add_dir(w, "data", mtime);
 *     udfw_add_file(w, "data/IMG_0001.JPG", size, mtime, read_cb, file);
 *     udfw_write(w, write_cb, out, extent_cb, list);    // lays out and writes the whole image
 *     udfw_free(w);
 */
#ifndef UDFWRITE_H
#define UDFWRITE_H

#include <stddef.h>
#include <stdint.h>

#define UDFW_SECTOR 2048

typedef struct udfw udfw;

/* Receives `count` sectors of the image, in order from sector 0. Returns 0, or -1 to stop. */
typedef int (*udfw_write_fn)(void *ctx, const void *buf, size_t count);
/* Reads up to `len` bytes of a file from `offset`. Returns the bytes read, or -1. */
typedef long (*udfw_read_fn)(void *file_ctx, uint64_t offset, void *buf, size_t len);
/* Reports where a file's data is: its first sector in the image and its size in bytes. */
typedef void (*udfw_extent_fn)(void *ctx, const char *path, uint64_t sector, uint64_t size);

typedef struct {
    const char *volume_id;   /* primary volume identifier: the disc id (at most 30 Latin-1 characters) */
    const char *label;       /* logical volume identifier: the label (at most 126 bytes); NULL: volume_id */
    const char *volume_set;  /* volume set identifier: 16 hex digits from the disc's UUID */
    int64_t time;            /* recording time of the volume, seconds since 1970 (UTC) */
} udfw_options;

udfw *udfw_new(const udfw_options *opt);

/* Paths are relative, separated by '/', in UTF-8. Missing parent folders are added with the
 * volume's time. Names may not be empty, ".", "..", or longer than UDF allows (255 bytes once
 * encoded). Returns 0, or -1 (see udfw_error). */
int udfw_add_dir(udfw *w, const char *path, int64_t mtime);
int udfw_add_file(udfw *w, const char *path, uint64_t size, int64_t mtime,
                  udfw_read_fn read, void *file_ctx);

/* Lays out and writes the whole image. `extents` (may be NULL) is called once per file. */
int udfw_write(udfw *w, udfw_write_fn write, void *ctx, udfw_extent_fn extents, void *extents_ctx);

/* The image size in sectors, known once the tree is complete (lays it out if needed). */
uint64_t udfw_sectors(udfw *w);

const char *udfw_error(const udfw *w);
void udfw_free(udfw *w);

#endif
