#!/bin/sh
# Build NetBSD's makefs, UDF backend only, as a Linux userspace program.
#
#   ./build.sh [BUILD_DIR]        -> BUILD_DIR/makefs (default: ./build)
#   ./build.sh --check [BUILD_DIR]   also build a small BD-ROM UDF 2.50 image and check it
#
# Needs: git, a C compiler, libbsd (Debian/Ubuntu: libbsd-dev).
# Sources: NetBSD at a pinned commit (BSD licence), plus compat/ and linux.patch from here.
set -eu

NETBSD_REPO=https://github.com/NetBSD/src.git
NETBSD_REV=477d71b4d1b73a66b61a03b5f6d3dc9212d4f888   # trunk, 2026-09-30

check=no
if [ "${1:-}" = "--check" ]; then check=yes; shift; fi
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "${1:-$here/build}" && cd "${1:-$here/build}" && pwd)
src=$out/netbsd-src

if [ ! -d "$src/.git" ]; then
    git init -q "$src"
    git -C "$src" remote add origin "$NETBSD_REPO"
    git -C "$src" sparse-checkout set usr.sbin/makefs usr.sbin/mtree sbin/newfs_udf sbin/fsck sys/fs/udf
    git -C "$src" fetch -q --depth 1 --filter=blob:none origin "$NETBSD_REV"
    git -C "$src" checkout -q FETCH_HEAD
fi
git -C "$src" checkout -q -- .
(cd "$src" && patch -s -p1 < "$here/linux.patch")

CC=${CC:-cc}
CFLAGS="${CFLAGS:--O2} -Wno-address-of-packed-member -DDEFAULT_FSTYPE=\"udf\""
INC="-I$here/compat -I$src/usr.sbin/makefs -I$src/usr.sbin/mtree -I$src/sbin/newfs_udf -I$src/sys/fs/udf -include $here/compat/nbtool_config.h"
objs=""
for f in usr.sbin/makefs/makefs.c usr.sbin/makefs/walk.c usr.sbin/makefs/udf.c \
         sbin/newfs_udf/udf_core.c sys/fs/udf/udf_osta.c; do
    o=$out/$(basename "$f" .c).o
    $CC $CFLAGS $INC -c "$src/$f" -o "$o"
    objs="$objs $o"
done
$CC $CFLAGS $INC -c "$here/compat/compat.c" -o "$out/compat.o"
$CC -o "$out/makefs" $objs "$out/compat.o" -lbsd -lm
echo "built $out/makefs"

if [ "$check" = yes ]; then
    t=$out/check
    rm -rf "$t" && mkdir -p "$t/src/sub dir"
    echo hello > "$t/src/a.txt"
    printf 'unicode\n' > "$t/src/sub dir/100% ünï.txt"
    head -c 3000001 /dev/urandom > "$t/src/big.bin"
    "$out/makefs" -t udf -o T=bdrom,v=2.50,V=2.50,L=CHECK "$t/check.udf" "$t/src" > "$t/log" 2>&1
    python3 - "$t/check.udf" <<'PY'
import re, sys
d = open(sys.argv[1], "rb").read()
revs = {int.from_bytes(d[m.start() + 23:m.start() + 25], "little") for m in re.finditer(rb"\*OSTA UDF Compliant", d)}
assert revs == {0x250}, revs
assert b"*UDF Metadata Partition" in d, "no metadata partition"
print("check: UDF 2.50 with a metadata partition")
PY
    if command -v 7z > /dev/null; then
        7z x -o"$t/out" "$t/check.udf" > /dev/null
        diff -r "$t/src" "$t/out" && echo "check: 7-Zip reads back identical files"
    fi
fi
