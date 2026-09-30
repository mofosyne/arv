# udfmake: UDF image builder (C library)

Builds UDF images, including Blu-ray **UDF 2.50** with a metadata partition, from
a folder on Linux. It is NetBSD's `makefs -t udf`, extracted and made into a
library. This is **our modified copy**; the upstream reference and the bug
report are in [`third_party/netbsd-makefs-udf/`](../../third_party/netbsd-makefs-udf/).

```sh
sudo apt install build-essential    # a C compiler and C library; nothing else
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

## Platforms

The same tree builds on NetBSD natively and on other systems the way NetBSD
builds its own tools on foreign hosts. The NetBSD sources already contain
`#if HAVE_NBTOOL_CONFIG_H` blocks for this; `compat/nbtool_config.h` sets it and
says which NetBSD functions the host lacks, and `compat/` supplies only those.
The Makefile picks the mode from `uname -s` (override with `HOST_OS=`).

| Platform | Mode | `compat/` supplies | Status |
|---|---|---|---|
| Linux, glibc | host | `setprogname`, `strsuftoll`, `snprintb` (+ `strlcpy` before glibc 2.38) | built and tested (gcc and clang; `check`, `asan`, `static`) |
| Linux, musl | host | the same, plus NetBSD's `queue.h`, `ALLPERMS` | built and tested (static) |
| FreeBSD 14.5 | host | only `strsuftoll`, `snprintb` | compiles and links (cross-built against its headers and libc); not run |
| NetBSD-current | native | nothing: its own headers, `libutil`, `libprop` | compiles and links (cross-built against a 2026-09 daily snapshot); not run |
| NetBSD 10.1 | native | nothing | does not compile: `partutil.c` from trunk uses `struct disk_geom` fields newer than 10.1 |
| OpenBSD, DragonFly, macOS | host | guarded for, untested | untested |

Cross-building for a platform with clang, given its headers and libraries in a sysroot directory:

```sh
make B=build-freebsd HOST_OS=FreeBSD AR=llvm-ar LDFLAGS="-static -fuse-ld=lld" \
     CC="clang --target=x86_64-unknown-freebsd14.5 --sysroot=/path/to/freebsd-sysroot"
make B=build-musl CC=musl-gcc LDFLAGS=-static check
```

## How it differs from upstream

| Path | Origin |
|---|---|
| `netbsd/` | NetBSD files at their upstream paths, from src `477d71b4d1b73a66b61a03b5f6d3dc9212d4f888` |
| `stubs/` | ours, all platforms: stand-ins for the filesystems and mtree code that aren't built |
| `compat/` | ours, non-NetBSD hosts only: the NetBSD libc pieces the host lacks |
| `udfmake.[ch]`, `udfmake_cli.c`, `Makefile`, `check.sh` | ours: library wrapper, program, build |

The history keeps these apart:
1. `lib/udfmake: import NetBSD makefs UDF sources, unmodified`: `netbsd/` exactly as upstream
   (plus `sys/sys/queue.h`, imported unmodified later for hosts without one).
2. `lib/udfmake: Linux build glue ... (no NetBSD edits)` and later build commits: no edits to `netbsd/`.
3. `lib/udfmake: fix two memory bugs in the NetBSD UDF code`: the only changes to `netbsd/`.

To see every change to NetBSD code:

```sh
git log --oneline -- lib/udfmake/netbsd
git diff $(git log --format=%h --diff-filter=A -1 -- lib/udfmake/netbsd/usr.sbin/makefs/udf.c) -- lib/udfmake/netbsd
```

No NetBSD file is edited to build on other systems:
- `makefs.c` is compiled with `-Dmain=netbsd_makefs_main`, and `DEFAULT_FSTYPE` is set to `udf` on the compiler command line.
- `stubs/` stands in for what isn't built.

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
