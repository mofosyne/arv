# udfmake: UDF image builder (C library)

Builds UDF images, including Blu-ray **UDF 2.50** with a metadata partition, from
a folder on Linux. It is NetBSD's `makefs -t udf`, extracted and made into a
library. This is **our modified copy**; the upstream reference and the bug
report are in [`src/udfmake/upstream/`](upstream/).

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
| macOS (x86_64, arm64) | host | only `strsuftoll`, `snprintb` | compiles and links (cross-built with `zig cc -target x86_64-macos` / `aarch64-macos`, zig 0.13); not run |
| Linux, ARM64 (musl) | host | as Linux, musl | compiles and links (`zig cc -target aarch64-linux-musl`); not run |
| Windows (native) | — | — | does not build (POSIX headers such as `err.h`); use the WebAssembly build |
| OpenBSD, DragonFly | host | guarded for, untested | untested |
| WebAssembly (WASI preview 1) | host | as musl, plus the `err(3)`/`warn(3)` family and a `readdir` that always gives `.` (Node's runtime leaves it out) | built and tested (`make wasi`: run with Node's WASI; same speed as native on 800 MB; `arv make --filesystem udf250 --udfmake src/udfmake/wasi/udfmake` end to end) |

Cross-building for a platform with clang, given its headers and libraries in a sysroot directory:

```sh
make B=build-freebsd HOST_OS=FreeBSD AR=llvm-ar LDFLAGS="-static -fuse-ld=lld" \
     CC="clang --target=x86_64-unknown-freebsd14.5 --sysroot=/path/to/freebsd-sysroot"
make B=build-musl CC=musl-gcc LDFLAGS=-static check
make B=build-mac HOST_OS=Darwin CC="zig cc -target aarch64-macos" AR="zig ar"   # any target zig knows
make wasi WASI_AR=llvm-ar-18      # WebAssembly; Debian/Ubuntu: apt install wasi-libc libclang-rt-18-dev-wasm32
wasi/udfmake -o T=bdrom,v=2.50,V=2.50 image.udf dir   # run it with Node (UDFMAKE_WASM picks the .wasm)
```

## How it differs from upstream

| Path | Origin |
|---|---|
| `netbsd/` | NetBSD files at their upstream paths, from src `477d71b4d1b73a66b61a03b5f6d3dc9212d4f888` |
| `stubs/` | ours, all platforms: stand-ins for the filesystems and mtree code that aren't built |
| `compat/` | ours, non-NetBSD hosts only: the NetBSD libc pieces the host lacks |
| `udfmake.[ch]`, `udfmake_cli.c`, `Makefile`, `check.sh` | ours: library wrapper, program, build |
| `wasi/` | ours: runs the WebAssembly build with Node (`run.mjs`, and a `udfmake` wrapper script) |

Every change to NetBSD's code is a bug fix, and each one is also a standalone patch
against unmodified upstream in
[`src/udfmake/upstream/patches/`](upstream/patches/).
Together the patches are exactly the difference between `netbsd/` and upstream.
`netbsd/sys/sys/queue.h` is an unmodified addition, used only on hosts without one (musl).

No NetBSD file is edited to build on other systems:
- `makefs.c` is compiled with `-Dmain=netbsd_makefs_main`, and `DEFAULT_FSTYPE` is set to `udf` on the compiler command line.
- `stubs/` stands in for what isn't built.

## Local changes to NetBSD code

Both are described in the [draft bug report, not yet confirmed or sent](upstream/BUG-REPORT.md):

1. `usr.sbin/makefs/udf.c`, `udf_copy_file`: whole-sector read buffer, and the padding is zeroed (it was an out-of-bounds read into the image).
2. `sbin/newfs_udf/udf_core.c`, `udf_set_regid`: bounded copy (it was a 1-byte `strcpy` overrun).
3. `sbin/newfs_udf/udf_core.c`, `unix_to_udf_name`: refuse names longer than UDF can hold (254
   characters, or 127 with any character above U+00FF). It used to wrap the one-byte length
   silently, which gave wrong names or an unreadable image.

## Limits

- **No metadata mirror.** The Metadata Mirror File points at the same blocks and
  `METADATA_DUPLICATED` is not set (upstream: `XXX no support for metadata
  mirroring yet`), so there is no second copy of the directory data.
- UDF only. There is no ISO 9660 bridge, and `-F` (mtree specs) and `-N` are not supported.
- Characters beyond U+FFFF (emoji) are stored as their UTF-8 bytes read as Latin-1, i.e. a
  different name. `arv make` refuses such names for UDF 2.50 before calling udfmake.
- Give it **one** source directory. With several, the UDF backend opens every file relative
  to the first one: it ignores `fsnode->root`, which `walk.c` sets for this. The result is
  "Can't open file" errors and an assertion in `udf_populate_walk`. `arv make` passes a
  single folder of symlinks with `-L` instead. This is upstream behaviour, not yet reported.
- Tested with 7-Zip read-back and dvdisaster RS03. Not yet tested with a Linux kernel mount, Windows or macOS.
