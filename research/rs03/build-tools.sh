#!/bin/sh
# Fetch and build the RS03 tools compared in research/research-notes.md ("RS03 tools compared"),
# at pinned commits, into a work directory (default: /tmp/rs03-tools).
#
#   research/rs03/build-tools.sh [WORKDIR]
#
# Builds:
#   speed47/      dvdisaster 0.79.10-pl6 (speed47 fork; what arv make uses today)
#   light/        dvdisaster Light (teaching-droid; RS03-only CLI fork)
#   lcsas-ecc     LCSAS's C89 RS03 verify/repair decoder (mikmorg/lcsas, recovery/src/lcsas-ecc)
#   lcsas-ecc.wasm  the same as a WASI program, if clang has the wasm32 target and a WASI libc
#
# Needs: git, a C compiler, make, pkg-config and glib2 headers (for dvdisaster).
set -eu
W=${1:-/tmp/rs03-tools}
mkdir -p "$W"
W=$(cd "$W" && pwd)

SPEED47=https://github.com/speed47/dvdisaster
SPEED47_REV=9c5c616bb2663b6e31cef597cacbae3cd20fb3cf
LIGHT=https://github.com/teaching-droid/dvdisaster-light
LIGHT_REV=6a481a6a6d5395f53fcb91287c4f48acc66e78ad
LCSAS=https://github.com/mikmorg/lcsas
LCSAS_REV=0fb28e754ee466a45dbae44806483f7050f600fa

fetch() {  # url rev dir
    if [ ! -d "$3/.git" ]; then
        git init -q "$3"
        git -C "$3" remote add origin "$1"
    fi
    git -C "$3" fetch -q --depth 1 origin "$2"
    git -C "$3" checkout -q FETCH_HEAD
}

build_dvdisaster() {  # dir
    (cd "$1" && { [ -f Makefile.config ] || ./configure --with-gui=no > configure.out 2>&1 || ./configure > configure.out 2>&1; } \
        && make -j"$(nproc 2>/dev/null || echo 2)" > make.out 2>&1)
    "$1/dvdisaster" --version 2>&1 | grep -m1 . || true
}

fetch "$SPEED47" "$SPEED47_REV" "$W/speed47"
build_dvdisaster "$W/speed47"
fetch "$LIGHT" "$LIGHT_REV" "$W/light"
build_dvdisaster "$W/light"

fetch "$LCSAS" "$LCSAS_REV" "$W/lcsas"
cc -O2 -std=c89 -o "$W/lcsas-ecc" "$W"/lcsas/recovery/src/lcsas-ecc/*.c
echo "lcsas-ecc built"
WASI_SYSROOT=${WASI_SYSROOT:-/usr}
if clang --target=wasm32-wasi --sysroot="$WASI_SYSROOT" -O2 -o "$W/lcsas-ecc.wasm" \
        "$W"/lcsas/recovery/src/lcsas-ecc/*.c 2> "$W/wasm.out"; then
    echo "lcsas-ecc.wasm built"
else
    echo "lcsas-ecc.wasm skipped (no wasm32 clang or WASI libc; see $W/wasm.out)"
fi
echo "tools in $W"
