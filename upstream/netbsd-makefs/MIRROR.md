# DRAFT change request: a real metadata mirror for read-only images (makefs -t udf)

> [!CAUTION]
> **To NetBSD developers: this is an unsent draft. Please don't act on it yet.**
> It was written and tested with the help of an AI assistant, on Linux only, and has not been
> checked by a person or on NetBSD itself. It is meant to follow the bug report in
> `BUG-REPORT.md` (issue mofosyne/arv#7), once that has been confirmed and sent.

Status: **draft, not yet sent.** The patch is `patches/04-metadata-mirror-readonly.patch`;
`repro/repro.sh` checks it (column 5) along with the three bug fixes.

## What

UDF 2.50 [2.2.10] keeps a *metadata mirror file* so a volume can still be read when the main
metadata file is damaged. `makefs -t udf` and `newfs_udf` create the mirror file entry, but
give it the main file's extent and leave the duplicate flag clear
(`sbin/newfs_udf/udf_core.c`, `udf_create_meta_files()`: `/* XXX no support for metadata
mirroring yet */`; `udf_add_logvol_part_meta()`: `pmap->pmm.flags = 0; /* METADATA_DUPLICATED */`).
That is compliant, but there is no second copy.

On rewritable media a real mirror would have to be kept in sync by the kernel on every change,
which is presumably why it was left. A read-only image (`makefs -t udf -o T=bdrom`, `dvdrom`,
`cdrom`) is never changed after it is made, so there the copy costs only its space.

## The change (read-only formats only)

- **Layout** (`udf_core.c`, the `FORMAT_META` block of the layout): the mirror gets its own
  extent, the size of the metadata partition, aligned, just before the mirror file entry at the
  end of the partition: as far from the main copy as the partition allows, so one scratch is
  unlikely to take both. A new `layout.meta_mirror_part_start_lba` holds it; when it equals
  `meta_part_start_lba` there is no copy (all other formats, and read-only ones with no room).
- **Writing** (`udf_write_virt()`): every write to the metadata partition is written to the
  same place in the mirror's extent as well. Every metadata write goes through this function,
  so the two copies cannot differ.
- **Descriptors**: the mirror file entry maps the new extent, and the partition map gets
  `METADATA_DUPLICATED`.
- **Room** (`usr.sbin/makefs/udf.c`, the size estimate): with a mirror the metadata needs twice
  its share, so the factor becomes `2p / (1 - 2p)` instead of `p / (1 - p)`. With `metaperc` 50
  or more there is no room; the estimate and the layout then fall back to the old shared extent.
- **Safety** (`udf_partition_alloc()`): a data allocation that would run into the mirror's
  extent stops with "won't fit" instead of overwriting it.

## Evidence

Built on Linux with the glue in this folder, against NetBSD src
`477d71b4d1b73a66b61a03b5f6d3dc9212d4f888` (unchanged in trunk as of `5601fdc`, 2026-10-07):

| Image | Result (`repro/mirror.py check`) |
|---|---|
| unmodified, `T=bdrom,v=2.50` | mirror on the same extent, flag clear |
| patched, `T=bdrom,v=2.50` (60 files) | a real copy at partition block 928, byte for byte, flag set |
| patched, `metaperc=40` | a real copy |
| patched, `metaperc=60` | the same extent, flag clear (no room: the fallback) |
| patched, all four patches, AddressSanitizer, 3,003 files incl. three over 4 MiB | a real copy; no errors (leak detection off: the leaks are the same without this patch) |

Read back with 7-Zip, every patched image extracts identically to its source. **The mirror
alone is enough:** `repro/mirror.py only-mirror` zeroes the main metadata and points the main
metadata file entry at the mirror's extent (7-Zip does not fall back to the mirror by itself);
7-Zip then still extracts every file identically. The same treatment of an unmodified image
cannot be read.

**Not tested here:** the Linux kernel mounting an image whose main metadata is destroyed (the
container used has no UDF module or loop devices), and anything on NetBSD itself. arv's own
writer (`src/udfwrite`), which lays out the mirror the same way, was mounted by Linux through
its mirror on 2026-10-03; this patch should be checked the same way before sending.

## How to check

```sh
upstream/netbsd-makefs/repro/repro.sh /tmp/udf-repro      # column 5: ok only with patch 04
python3 upstream/netbsd-makefs/repro/mirror.py check IMAGE
python3 upstream/netbsd-makefs/repro/mirror.py only-mirror IMAGE OUT && 7z l OUT
# on Linux, as root: the main metadata file entry and copy destroyed, mounted through the mirror
python3 upstream/netbsd-makefs/repro/mirror.py damaged IMAGE damaged.udf
mount -o loop,ro -t udf damaged.udf /mnt && ls -R /mnt
```
