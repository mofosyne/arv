#!/bin/sh
# Checks udfwrite against docs/archival-udf.md, and leaves test images for checking readers
# by hand (Linux kernel, Windows, macOS):
#
#   build/check/src/                        the files the images hold
#   build/check/test.iso                    a clean image
#   build/check/test.damaged-metadata.iso   the metadata file entry and metadata zeroed: readers must use the mirror
#   build/check/test.damaged-anchor256.iso  sector 256 zeroed: readers must use the end anchors
#
#   sh check.sh build/udfwrite
# Needs python3; uses udfinfo/udfdump (udftools) and 7z when installed.
set -eu
tool=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
here=$(cd "$(dirname "$0")" && pwd)
dir=$here/build/check
rm -rf "$dir"
mkdir -p "$dir"
cd "$dir"
ok() { echo "ok: $*"; }
no() { echo "FAILED: $*"; exit 1; }

# a tree with what tends to break writers: nesting, empty things, Unicode, many entries, a
# multi-megabyte file
mkdir -p src/a/b/c src/empty-dir "src/sub dir"
python3 - <<'EOF'
import random
r = random.Random(42)
open("src/big.bin", "wb").write(r.randbytes(3_000_000))
open("src/empty.txt", "w").close()
open("src/a/b/c/deep.txt", "w").write("deep\n")
open("src/sub dir/100% ünïcode.txt", "w", encoding="utf-8").write("ü\n")
open("src/日本語の名前.txt", "w", encoding="utf-8").write("jp\n")
open("src/photo 😀.txt", "w", encoding="utf-8").write("emoji\n")
open("src/run-me.sh", "w").write("#!/bin/sh\necho it runs from the disc\n")
import os; os.chmod("src/run-me.sh", 0o755)
for i in range(1, 121):
    open(f"src/a/file-with-a-fairly-long-name-number-{i}.txt", "w").write(f"{i}\n")
EOF
find src -exec touch -h -d '2020-01-02 03:04:05 UTC' {} +

args="-V UDFW-01_2026_X -L UDFW-01_2026_X_udfwrite_test -S 0123456789abcdef -t 1790000000"
"$tool" $args -x extents.tsv test.iso src
ok "wrote test.iso ($(($(wc -c < test.iso) / 2048)) sectors)"

"$tool" $args again.iso src
cmp -s test.iso again.iso && ok "reproducible: a second run gives the same bytes" || no "two runs differ"
rm again.iso

python3 - <<'EOF'
import os, struct, sys
img = open("test.iso", "rb").read()
# every file, cut out of the raw image by its extent, matches the source
n = 0
for line in open("extents.tsv", encoding="utf-8"):
    if line.startswith("#"):
        continue
    sector, size, path = line.rstrip("\n").split("\t", 2)
    start = int(sector) * 2048
    if img[start:start + int(size)] != open(os.path.join("src", path), "rb").read():
        sys.exit("FAILED: extent of %s" % path)
    n += 1
print("ok: %d files cut from the raw image by extents.tsv match the source" % n)
# the metadata mirror is a real copy, in other sectors, with the duplicate flag set
P = 384
pm = img[291 * 2048 + 446:291 * 2048 + 510]
mirror_fe, flags = struct.unpack_from("<I", pm, 44)[0], pm[58]
def extent(block):
    fe = img[(P + block) * 2048:(P + block + 1) * 2048]
    l_ea = struct.unpack_from("<I", fe, 208)[0]
    length, pos = struct.unpack_from("<II", fe, 216 + l_ea)
    return pos, length
(mp, ml), (rp, rl) = extent(0), extent(mirror_fe)
main, mirror = img[(P + mp) * 2048:(P + mp) * 2048 + ml], img[(P + rp) * 2048:(P + rp) * 2048 + rl]
if not (flags & 1 and main == mirror and mp != rp):
    sys.exit("FAILED: metadata mirror")
print("ok: metadata mirror is a real copy (%d blocks at partition block %d and %d)" % (ml // 2048, mp, rp))
# damaged copies for testing readers by hand
damaged = bytearray(img)                     # the metadata file entry and the whole metadata
damaged[P * 2048:(P + mp) * 2048 + ml] = bytes((mp * 2048) + ml)
open("test.damaged-metadata.iso", "wb").write(damaged)
damaged = bytearray(img)
damaged[256 * 2048:257 * 2048] = bytes(2048)
open("test.damaged-anchor256.iso", "wb").write(damaged)
print("ok: wrote test.damaged-metadata.iso and test.damaged-anchor256.iso")
# permissions: an executable source file is executable by everyone; others are only readable
meta = P + mp
fsd = img[meta * 2048:(meta + 1) * 2048]
root = struct.unpack_from("<I", fsd, 404)[0]
def entry(lbn):
    return img[(meta + lbn) * 2048:(meta + lbn + 1) * 2048]
r = entry(root)
l_ea = struct.unpack_from("<I", r, 208)[0]
dlen, dpos = struct.unpack_from("<II", r, 216 + l_ea)
fids = img[(meta + dpos) * 2048:(meta + dpos) * 2048 + dlen]
perms, off = {}, 0
while off < len(fids):
    lfi, lbn, liu = fids[off + 19], struct.unpack_from("<I", fids, off + 24)[0], struct.unpack_from("<H", fids, off + 36)[0]
    name = fids[off + 38 + liu + 1:off + 38 + liu + lfi].decode("latin-1") if lfi and fids[off + 38 + liu] == 8 else ""
    perms[name] = struct.unpack_from("<I", entry(lbn), 44)[0]
    off += (38 + liu + lfi + 3) & ~3
if perms.get("run-me.sh") != 0x14A5 or perms.get("empty.txt") != 0x1084:
    sys.exit("FAILED: permissions %r" % {k: hex(v) for k, v in perms.items() if k in ("run-me.sh", "empty.txt")})
print("ok: run-me.sh is executable by everyone (r-x r-x r-x); other files read-only (r-- r-- r--)")
EOF

if command -v udfinfo >/dev/null; then
    info=$(udfinfo test.iso 2>&1)
    echo "$info" | grep -qi warning && no "udfinfo warns: $info"
    for want in udfrev=2.50 integrity=closed accesstype=readonly numfiles=127 numdirs=6; do
        echo "$info" | grep -q "^$want$" || no "udfinfo: expected $want"
    done
    ok "udfinfo: UDF 2.50, closed, read-only, 127 files, 6 folders, no warnings"
fi
if command -v udfdump >/dev/null; then
    udfdump -b 2048 -S test.iso 2>/dev/null | grep -q "Metadata is duplicated on disc" \
        && ok "udfdump: metadata partition map says duplicated" || no "udfdump: duplicate flag"
fi
if command -v 7z >/dev/null; then
    7z x -y -oextracted test.iso >/dev/null
    diff -r src extracted >/dev/null && ok "7-Zip extracts every file and folder identically" || no "7-Zip extraction differs"
    rm -rf extracted
fi
echo "all checks passed; test images in $dir"
