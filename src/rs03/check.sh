#!/bin/sh
# Checks rs03: -t finds exactly the damage done to an augmented image (data, header, CRC and parity
# sectors). With dvdisaster Light (or the speed47 fork) on PATH as `dvdisaster`, also: the same images augmented by
# both are byte for byte the same (odd sizes, a chosen medium, the automatic one, the redundancy clip,
# a large image), and each tool's test accepts the other's image.
#
#   sh check.sh build/rs03          (python3 makes the test data)
set -eu
tool=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
here=$(cd "$(dirname "$0")" && pwd)
dir=$here/build/check
rm -rf "$dir"
mkdir -p "$dir"
cd "$dir"
ok() { echo "ok: $*"; }
no() { echo "FAILED: $*"; exit 1; }
python3 - <<'PY'
import random
r = random.Random(7)
def img(name, sectors, extra=0, zero=False):
    n = sectors * 2048 + extra
    open(name, "wb").write(bytes(n) if zero else r.randbytes(n))
img("odd.iso", 40, 777)
img("small.iso", 1648)
img("mid.iso", 9000)
img("zero.iso", 20000, zero=True)
img("big.iso", 60000)
PY
# -t: a clean image, then one kind of damage at a time
cp mid.iso v.iso
"$tool" -n 12000 v.iso >/dev/null
"$tool" -t v.iso >/dev/null || no "-t rejects a whole image"
python3 - <<'PY'
import shutil
for name, sectors, fill in [("data", [100, 101, 5000], 0), ("first", [0], 0), ("header", [9000], 0),
                            ("crc", [9030], 1), ("ecc", [11000], 0)]:
    shutil.copy("v.iso", name + ".iso")
    with open(name + ".iso", "r+b") as f:
        for s in sectors:
            f.seek(s * 2048)
            f.write(bytes([fill]) * (10 if fill else 2048))
PY
expect() {
    "$tool" -t "$1.iso" > t.txt && no "-t missed damage in $1"
    sed -n 2p t.txt | grep -q "$2" || { cat t.txt; no "-t reported $1 wrongly (want: $2)"; }
}
expect data "header good; 3 data sectors with a wrong CRC, 0 CRC sectors and 0 parity"
expect first "header good; 1 data sectors with a wrong CRC, 0 CRC sectors and 0 parity"
expect header "header DAMAGED; 1 data sectors with a wrong CRC, 0 CRC sectors and 0 parity"
expect crc "header good; 0 data sectors with a wrong CRC, 1 CRC sectors and 0 parity"
expect ecc "header good; 0 data sectors with a wrong CRC, 0 CRC sectors and 1 parity"
ok "-t: a whole image passes; damaged data, header, CRC and parity sectors are each found and counted"

if ! dvdisaster --help 2>&1 | grep -q no-bdr-defect-management; then
    echo "skipped: no dvdisaster Light (or speed47 fork) on PATH to compare with"
    exit 0
fi
n=0
for case in "odd.iso 4800" "odd.iso -" "small.iso -" "small.iso 400000" "mid.iso 12000" "mid.iso -" \
            "zero.iso 100000" "big.iso 120000"; do
    set -- $case
    cp "$1" a.iso
    cp "$1" b.iso
    if [ "$2" = - ]; then
        dvdisaster -i a.iso -mRS03 -o image -c --no-progress >/dev/null 2>&1
        "$tool" b.iso >/dev/null
    else
        dvdisaster -i a.iso -mRS03 -o image -c -n "$2" --no-progress >/dev/null 2>&1
        "$tool" -n "$2" b.iso >/dev/null
    fi
    cmp -s a.iso b.iso || no "$case differs from dvdisaster"
    n=$((n + 1))
done
ok "$n images augmented byte for byte as dvdisaster Light augments them"
dvdisaster -i b.iso -t --no-progress >t.txt 2>&1 && grep -q "Ecc block test *: pass" t.txt \
    && ok "dvdisaster Light -t accepts the image rs03 made" || { cat t.txt; no "dvdisaster -t"; }
"$tool" -t a.iso >/dev/null && ok "rs03 -t accepts the image dvdisaster Light made" || no "rs03 -t on dvdisaster's image"
for f in data crc ecc; do
    dvdisaster -i $f.iso -f --no-progress >/dev/null 2>&1 || true      # exits 1 after repairing
    "$tool" -t $f.iso >/dev/null || no "$f.iso still damaged after dvdisaster -f"
done
ok "dvdisaster Light repairs the damaged rs03 images, and rs03 -t then finds them whole"
