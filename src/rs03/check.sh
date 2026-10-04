#!/bin/sh
# Checks rs03: -t finds exactly the damage done to an augmented image (data, header, CRC and parity
# sectors); -f repairs damage of every kind back to the very image, and leaves what is too much;
# docs/rs03-format.md's test vectors come out of rs03 and out of spec-check.py, written from it alone. With dvdisaster Light (or the speed47 fork) on PATH as `dvdisaster`, also: the same images augmented by
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

# docs/rs03-format.md's test vectors, from rs03 and from spec-check.py (written from the spec alone)
python3 - <<'PY'
for name, n in (("vec1", 100 * 2048), ("vec2", 400 * 2048), ("vec3", 400 * 2048 + 777)):
    open(name + ".iso", "wb").write(bytes((31 * i + i // 2048) % 256 for i in range(n)))
PY
for v in "vec1 510 9d909de643504e2f1e17ba8fa6ed7d703d9b4e081b6e50a7acb416bf4b1188a3" \
         "vec2 1020 549b58b1f3ea5dc3b1fd2ec844c3fa322a5d7d5f00d5f307d5cb30e464ed5d48" \
         "vec3 1020 6deb1dc9e4a9b5889c747893c0e179c17fd3e5c1d634a3d763f61c25c57c0d28"; do
    set -- $v
    python3 "$here/spec-check.py" $1.iso $2 $1.spec.iso
    "$tool" -n $2 $1.iso >/dev/null
    [ "$(sha256sum < $1.iso | cut -d' ' -f1)" = "$3" ] || no "$1: rs03 differs from the spec's test vector"
    cmp -s $1.iso $1.spec.iso || no "$1: spec-check.py differs from rs03"
done
ok "the spec's test vectors: rs03 and spec-check.py (written from docs/rs03-format.md alone) give them"

# -f: v.iso is 9000 sectors on 12000: 47 sectors per layer, 62 roots, the CRC layer at 9024
python3 - <<'PY'
import random, shutil
r = random.Random(11)
spl, crc = 47, 9024
marker = bytearray(2048)
head, end = b"dvdisaster dead sector marker\nThis sector could not be read from the image.\n", b"dvdisaster dead sector end marker\n"
marker[:len(head)] = head
marker[2046 - len(end):2046] = end
def damage(name, writes, cut=None):
    shutil.copy("v.iso", name + ".iso")
    with open(name + ".iso", "r+b") as f:
        for at, data in writes:
            f.seek(at)
            f.write(data)
        if cut is not None:
            f.truncate(cut)
scattered = [(s * 2048 + r.randrange(2048), bytes([r.randrange(256)])) for s in (0, 1, 16, 100, 2000, 5000, 8999, 9000, 9001)]
damage("fix-mixed", scattered + [(3000 * 2048, bytes(30 * 2048)),            # a zero-filled run (ddrescue, dd)
                                 (4000 * 2048, bytes(marker)),               # dvdisaster's dead sector marker
                                 ((crc + 9) * 2048 + 5, b"xx"),              # a CRC sector
                                 (11000 * 2048, bytes(2048)), (11500 * 2048 + 77, b"garbage")])  # parity
damage("fix-nocrc", [((crc + 19) * 2048 + 3, b"x"), (20 * 2048 + 99, b"y"), (20 * 2048 + 700, b"z")])  # its CRCs damaged too
damage("fix-first", [((crc + n) * 2048 + 1500, b"broken") for n in range(10)])    # the first CRC sectors
damage("fix-cut", [], cut=(11985 - 600) * 2048)                                    # the end missing
damage("fix-much", [(1000 * 2048, bytes(70 * spl * 2048))])                       # 70 layers gone: more than 62 roots
PY
for f in fix-mixed fix-nocrc fix-first fix-cut; do
    "$tool" -f $f.iso >f.txt || { cat f.txt; no "-f could not repair $f"; }
    cmp -s $f.iso v.iso || { cat f.txt; no "-f did not bring $f back to the image"; }
done
grep -q "600 sectors were missing" f.txt || { cat f.txt; no "-f did not report the missing end"; }
ok "-f: scattered bytes, zero-filled runs, dead sector markers, CRC and parity damage, damage its CRCs miss,"
echo "    damaged first CRC sectors and a missing end are each repaired back to the very image"
"$tool" -f fix-much.iso >f.txt && no "-f claims to have repaired too much damage"
grep -q "NOT repaired: .* at 47 positions" f.txt && ok "-f: damage beyond the parity is reported, not guessed at" \
    || { cat f.txt; no "-f on too much damage"; }

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
python3 - <<'PY'
import random, shutil
r = random.Random(13)
for name in ("light", "ours"):
    shutil.copy("b.iso", name + ".iso")
with open("light.iso", "r+b") as f:
    hits = [(r.randrange(100000) * 2048 + r.randrange(2048), bytes([r.randrange(256)])) for _ in range(300)]
    for at, b in hits + [(5000 * 2048, bytes(40 * 2048))]:
        f.seek(at)
        f.write(b)
shutil.copy("light.iso", "ours.iso")
PY
dvdisaster -i light.iso -f --no-progress >/dev/null 2>&1 || true
"$tool" -f ours.iso >/dev/null || no "rs03 -f on scattered damage"
cmp -s light.iso b.iso && cmp -s ours.iso b.iso && ok "rs03 -f and dvdisaster Light -f repair the same damage to the same image" \
    || no "rs03 -f and dvdisaster Light -f differ"
