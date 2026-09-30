#!/bin/sh
# Reproduce BUG-REPORT.md against UNMODIFIED NetBSD sources, on Linux.
#
#   third_party/netbsd-makefs-udf/repro/repro.sh [WORKDIR]
#
# Fetches NetBSD src at the pinned commit (only the directories makefs -t udf
# needs), builds it with the Linux glue from lib/udfmake (compat headers and
# stubs only; the NetBSD files themselves are used as fetched), then:
#   1. fortified build (-D_FORTIFY_SOURCE=2)   -> expect abort in udf_set_regid (bug 2)
#   2. AddressSanitizer build                  -> expect heap-buffer-overflow via udf_copy_file (bug 1)
#   3. plain build, image padding scan         -> expect non-zero bytes after file data (bug 1)
#   4. a 255-character file name               -> expect exit 0 and an unreadable image (bug 3)
#   then the same four with proposed.patch applied -> expect all clean
# Needs: git, cc, make, python3.
set -u

NETBSD_REPO=https://github.com/NetBSD/src.git
NETBSD_REV=477d71b4d1b73a66b61a03b5f6d3dc9212d4f888

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
lib=$root/lib/udfmake
work=${1:-$(mktemp -d)}
mkdir -p "$work"
work=$(cd "$work" && pwd)
echo "work directory: $work"

# The NetBSD files lib/udfmake uses, at their upstream paths
FILES="usr.sbin/makefs/makefs.c usr.sbin/makefs/makefs.h usr.sbin/makefs/walk.c
usr.sbin/makefs/udf.c usr.sbin/makefs/udf/cdio_mmc_structs.h usr.sbin/mtree/mtree.h
sbin/newfs_udf/udf_core.c sbin/newfs_udf/udf_core.h sbin/newfs_udf/newfs_udf.h
sbin/newfs_udf/unicode.h sbin/fsck/partutil.c sbin/fsck/partutil.h
sys/fs/udf/ecma167-udf.h sys/fs/udf/udf_osta.c sys/fs/udf/udf_osta.h
sys/fs/udf/udf_mount.h sys/fs/udf/udf_bswap.h"

fetch() {  # fetch GITDIR: NetBSD src at the pinned commit (sparse)
    if [ ! -d "$1/.git" ]; then
        git init -q "$1"
        git -C "$1" remote add origin "$NETBSD_REPO"
        git -C "$1" sparse-checkout set usr.sbin/makefs usr.sbin/mtree sbin/newfs_udf sbin/fsck sys/fs/udf
        git -C "$1" fetch -q --depth 1 --filter=blob:none origin "$NETBSD_REV" || exit 1
        git -C "$1" checkout -q FETCH_HEAD
    fi
    [ "$(git -C "$1" rev-parse HEAD)" = "$NETBSD_REV" ] || { echo "not at $NETBSD_REV"; exit 1; }
}

extract() {  # extract GITDIR DEST: exact copies of FILES at the pinned commit
    rm -rf "$2"
    for f in $FILES; do
        mkdir -p "$2/$(dirname "$f")"
        git -C "$1" show "$NETBSD_REV:$f" > "$2/$f"
    done
}

build() {  # build NB B CFLAGS [LIBS]
    make -s -C "$lib" NB="$1" B="$2" CFLAGS="$3" LIBS="${4:--lm}" all > "$2.log" 2>&1 \
        || { echo "build failed, see $2.log"; exit 1; }
}

run_all() {  # run_all NB LABEL DESCRIPTION
    nb=$1 label=$2
    echo
    echo "=== $label: NetBSD src $NETBSD_REV, $3"

    b=$work/$label-fortify
    build "$nb" "$b" "-O2 -g -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2"
    rm -f "$work/img"
    if "$b/udfmake" -o T=bdrom,v=2.50,V=2.50 "$work/img" "$work/files" > "$b.run" 2>&1; then
        echo "1. fortified build: OK"
    else
        echo "1. fortified build: FAILED: $(grep -m1 '\*\*\*' "$b.run" || tail -1 "$b.run")"
        if command -v gdb > /dev/null; then
            gdb -q -batch -ex run -ex bt --args "$b/udfmake" -o T=bdrom,v=2.50,V=2.50 "$work/img" "$work/files" 2>&1 \
                | grep -E '^#[0-9]+ .*(strcpy|udf_set_regid|udf_add_logvol|udf_create_logical)' | sed 's/^/   /'
        fi
    fi

    b=$work/$label-asan
    build "$nb" "$b" "-O0 -g -fsanitize=address" "-fsanitize=address -lm"
    rm -f "$work/img"
    if ASAN_OPTIONS=detect_leaks=0 "$b/udfmake" -o T=bdrom,v=2.50,V=2.50 "$work/img" "$work/files" > "$b.run" 2>&1; then
        echo "2. AddressSanitizer: no errors"
    else
        echo "2. AddressSanitizer: $(grep -m1 -o 'ERROR: AddressSanitizer: [a-z-]*' "$b.run")"
        grep -m1 -A9 'ERROR: AddressSanitizer' "$b.run" | grep -E '#[0-9]+ ' | sed 's/^ */   /'
    fi

    b=$work/$label-plain
    build "$nb" "$b" "-O2 -g -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"
    rm -f "$work/img"
    "$b/udfmake" -o T=bdrom,v=2.50,V=2.50 "$work/img" "$work/files" > "$b.run" 2>&1 || { echo "3. build of image failed"; return; }
    printf '3. padding after file data: '
    python3 "$here/padding.py" scan "$work/files" "$work/img" > "$b.scan"
    tail -1 "$b.scan"
    head -3 "$b.scan" | grep '^ ' | sed 's/^ */   /'

    rm -rf "$work/longname" "$work/img"            # bug 3: one file with a 255-character name
    mkdir -p "$work/longname"
    python3 -c "import sys; open(sys.argv[1] + '/' + 'd' * 255, 'w').write('x')" "$work/longname"
    printf '4. 255-character name: '
    if "$b/udfmake" -o T=bdrom,v=2.50,V=2.50 "$work/img" "$work/longname" > "$b.long" 2>&1; then
        if ! command -v 7z > /dev/null; then
            echo "makefs exit 0 (install 7z to check the image)"
        elif 7z l "$work/img" > /dev/null 2>&1; then
            echo "makefs exit 0; image readable, name length $(7z l -slt "$work/img" | sed -n 's/^Path = d/d/p' | awk '{print length($0)}')"
        else
            echo "makefs exit 0; image cannot be read"
        fi
    else
        echo "makefs refused it: $(grep -o 'file name too long for UDF ([^)]*)' "$b.long")"
    fi
}

python3 "$here/padding.py" make "$work/files"

fetch "$work/netbsd-git"
extract "$work/netbsd-git" "$work/upstream"
run_all "$work/upstream" upstream "files unmodified"

extract "$work/netbsd-git" "$work/proposed"
(cd "$work/proposed" && patch -s -p1 < "$here/../proposed.patch") || exit 1
run_all "$work/proposed" proposed "with proposed.patch"
