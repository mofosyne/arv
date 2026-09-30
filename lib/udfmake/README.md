# udfmake: UDF image builder (C library)

Builds UDF images, including Blu-ray **UDF 2.50** with a metadata partition, from
a folder on Linux. It is NetBSD's `makefs -t udf`, extracted and made into a
library. This is **our modified copy**; the upstream reference and the bug
report are in [`third_party/netbsd-makefs-udf/`](../../third_party/netbsd-makefs-udf/).

```sh
sudo apt install build-essential    # a C compiler; nothing else is needed
make                 # build/libudfmake.a and build/udfmake (needs only the C library)
make static          # build-static/udfmake: one self-contained binary, no runtime dependencies
make check           # also build a BD-ROM UDF 2.50 image and verify it (7-Zip reads it back)
make asan            # the same under AddressSanitizer
build/udfmake -o T=bdrom,v=2.50,V=2.50,L=MYDISC disc.udf folder/
```

```c
#include "udfmake.h"
int udfmake(const char *image, const char *dir, const char *options);  /* 0 = ok */
```

Options are makefs's `-o` list:
- `T=`: disc type. `bdrom` gives a finished image with a metadata partition; `bdr` gives an incremental layout (VAT).
- `v=` / `V=`: minimum and maximum UDF revision.
- `L=`: volume name.

Upstream reports errors with `err(3)`, which **exits the process**. Callers that
must survive a failure should run the `udfmake` program as a child process.

## How it differs from upstream

| Path | Origin |
|---|---|
| `netbsd/` | NetBSD files at their upstream paths, from src `477d71b4d1b73a66b61a03b5f6d3dc9212d4f888` |
| `compat/`, `udfmake.[ch]`, `udfmake_cli.c`, `Makefile`, `check.sh` | ours: Linux glue, library wrapper, build |

The history keeps these apart:
1. `lib/udfmake: import NetBSD makefs UDF sources, unmodified`: `netbsd/` exactly as upstream.
2. `lib/udfmake: Linux build glue ... (no NetBSD edits)`: glue only.
3. `lib/udfmake: fix two memory bugs in the NetBSD UDF code`: the only changes to `netbsd/`.

To see every change to NetBSD code:

```sh
git log --oneline -- lib/udfmake/netbsd
git diff $(git log --format=%h --diff-filter=A -1 -- lib/udfmake/netbsd/usr.sbin/makefs/udf.c) -- lib/udfmake/netbsd
```

The glue avoids editing NetBSD files:
- `compat/` supplies the few NetBSD libc functions and macros glibc lacks (`setprogname`, `TAILQ_FOREACH_SAFE`, `strsuftoll` and so on), so no extra libraries are needed.
- `compat/cd9660.h` and `compat/other_fs.c` stand in for the filesystems that aren't built.
- `makefs.c` is compiled with `-Dmain=netbsd_makefs_main`.
- `DEFAULT_FSTYPE` is set to `udf` on the compiler command line.

## Local changes to NetBSD code

Both are described in the [bug report](../../third_party/netbsd-makefs-udf/BUG-REPORT.md):

1. `usr.sbin/makefs/udf.c`, `udf_copy_file`: whole-sector read buffer, and the padding is zeroed (it was an out-of-bounds read into the image).
2. `sbin/newfs_udf/udf_core.c`, `udf_set_regid`: bounded copy (it was a 1-byte `strcpy` overrun).

## Limits

- **No metadata mirror.** The Metadata Mirror File points at the same blocks and
  `METADATA_DUPLICATED` is not set (upstream: `XXX no support for metadata
  mirroring yet`), so there is no second copy of the directory data.
- UDF only. There is no ISO 9660 bridge, and `-F` (mtree specs) and `-N` are not supported.
- Tested with 7-Zip read-back and dvdisaster RS03. Not yet tested with a Linux kernel mount, Windows or macOS.
