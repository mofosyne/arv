# NetBSD makefs (UDF only) for Linux

The only open-source tool that builds a **UDF 2.50** image (with a metadata
partition, as Blu-ray uses) from a folder is NetBSD's `makefs -t udf`. This
builds just that part as a Linux userspace program.

```sh
sudo apt install libbsd-dev          # plus git and a C compiler
./build.sh --check                   # -> build/makefs, then builds and checks a test image
build/makefs -t udf -o T=bdrom,v=2.50,V=2.50,L=MYDISC disc.udf folder/
```

- `T=bdrom`: a finished, read-only image written in one pass (metadata partition,
  no VAT). `T=bdr` makes an incrementally recordable layout (VAT) instead.
- `v=`/`V=`: minimum and maximum UDF revision. `L=`: logical volume name.

## What is here

| File | What |
|---|---|
| `build.sh` | Fetches NetBSD's sources at a pinned commit (only the five directories needed), applies `linux.patch`, compiles 5 NetBSD files plus `compat/` |
| `linux.patch` | Leaves out the other filesystems, and fixes two upstream bugs (below) |
| `compat/` | Stand-ins for NetBSD headers and libutil functions, on top of libbsd |

NetBSD sources are BSD-licensed and are downloaded, not copied into this repository.

## Upstream bugs fixed by the patch

1. `usr.sbin/makefs/udf.c`, `udf_copy_file`: the read buffer is the file's
   length, but the last partial sector is written as a whole sector. Up to 2 KiB
   of unrelated memory per file could end up in the image's padding.
   Fixed: the buffer is rounded up to whole sectors and the padding zeroed.
2. `sbin/newfs_udf/udf_core.c`, `udf_set_regid`: `strcpy` of a 23-character id
   into a 23-byte field writes its NUL into the next field (harmless in practice,
   because that byte is set right after, but undefined behaviour; glibc's
   `_FORTIFY_SOURCE` aborts on it). Fixed with a bounded copy.

Both are worth sending upstream to NetBSD.

## Limits

- **No metadata mirror yet.** The image has the Metadata Mirror File, but it
  points at the same blocks as the Metadata File and the "duplicated" flag is
  off (upstream: `/* XXX no support for metadata mirroring yet */`). So there is
  no second copy of the directory data, which is the one robustness feature of
  2.50 over 2.01. Adding it means allocating a second extent (ideally far from
  the first), writing every metadata sector to both, and setting
  `METADATA_DUPLICATED`.
- No ISO9660 bridge: the image is UDF only.
- `-F` (mtree spec files) and `-N` (user databases) are not supported.
- Tested: images read back identical with 7-Zip; dvdisaster adds RS03 to them.
  Not yet tested: Linux kernel mount, Windows, macOS.
