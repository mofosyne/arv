# udfwrite: closed, read-only UDF 2.50 images for archives

**The UDF writer of arv**, and the only one it makes discs with, implementing [docs/archival-udf.md](../../docs/archival-udf.md):
a metadata partition with a **real mirror**, every file in **one contiguous run** in a fixed order,
and nothing taken from the clock or the machine, so the same folder gives the **same bytes**.
NetBSD makefs, where arv started, is kept outside arv in `upstream/netbsd-makefs/` for the fixes offered upstream. Linux
reads all test images; Windows and macOS are checked with the first real burn (issues #18, #5).

A library with no dependencies beyond the C library (`udfwrite.h`, `udfwrite.c`): the caller
adds folders and files, supplies file bytes through a read callback and receives the image
through a write callback. The program (`udfwrite_cli.c`) makes an image of a folder:

```sh
make                       # build/udfwrite
make check                 # check it, and leave test images in build/check/
build/udfwrite -V TRIP-01_2019_4 -L "TRIP-01_2019_4 Kyoto" -S 0123456789abcdef -t 1790000000 \
    -x extents.tsv TRIP-01_2019_4.iso folder/
```

`-V` volume id (the disc id), `-L` label, `-S` volume set (16 hex digits from the disc's UUID),
`-t` recording time (seconds since 1970, UTC), `-x` where each file starts (Binding data, kept
off the disc). Symbolic links are followed; anything that is not a file or folder is refused.

From arv: `arv make FOLDER` uses it by default.

## What `make check` checks

- two runs give the same bytes;
- every file cut out of the raw image at its `extents.tsv` offset matches the source;
- the mirror is a real copy in other sectors, with the duplicate flag set;
- `udfinfo`: UDF 2.50, closed, read-only, right counts, no warnings;
- `udfdump`: "Metadata is duplicated on disc";
- 7-Zip extracts every file and folder identically.

## Testing readers by hand

`make check` leaves three images in `build/check/` with the files they hold in `build/check/src/`:

| Image | What a reader must do |
|---|---|
| `test.iso` | read it normally |
| `test.damaged-metadata.iso` | the metadata file entry and the metadata are zeroed: read through the mirror |
| `test.damaged-anchor256.iso` | the anchor at sector 256 is zeroed: find the volume through the end anchors |

Linux (as root): `mount -o loop,ro build/check/test.iso /mnt && diff -r build/check/src /mnt; umount /mnt`.
Windows: right-click the image, *Mount*, compare the files. macOS: `hdiutil attach -readonly
build/check/test.iso`, compare, `hdiutil detach`. Findings so far:

| Reader | `test.iso` | metadata destroyed | anchor 256 destroyed |
|---|---|---|---|
| Linux kernel (loop mount, 2026-10-03) | identical | **identical: falls back to the mirror** ("metadata inode efe not found", then mounts) | identical: uses the end anchors |
| udftools (`udfinfo`) | no warnings | fails (doesn't use the mirror) | reads it (uses the second anchor) |
| 7-Zip 23.01 | identical | fails (doesn't use the mirror) | fails (reads sector 256 only) |
| Windows, macOS | to test | to test | to test |
