#!/bin/sh
# Checks arvc, without Python:
# - the reference scenarios (tests/reference/): every command run in a fixed setting must print
#   and write what expected/ holds (first written by the Python arv), byte for byte (normalised);
# - what only a real disc shows: a disc made by arvc from a folder like a git clone verifies,
#   restores to a clean `git status` (links and execute bits included), and damage is found;
#   RS03 is added, tested (and damage found) and repaired by arv itself; with dvdisaster Light, it
#   accepts and repairs arv's images; with Siegfried, formats are identified; with arv's
#   git checkout as the source, tools/ gets the commit and its history;
# - SHA-256 and SHA-512 against sha256sum and sha512sum, and the recfile writer.
# With python3 on PATH it also checks that arvc called as arv hands the add-on's commands to it.
# (tests/fixtures/ is checked by build/fixtures, which make check runs first.)
#
#   sh check.sh build/arvc
# Needs 7z (to read disc images) and git.
set -eu
tool=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
dir=$here/build/check
rm -rf "$dir"
mkdir -p "$dir"
cd "$dir"
ok() { echo "ok: $*"; }
no() { echo "FAILED: $*"; exit 1; }
command -v 7z >/dev/null || no "7z is needed to read disc images (p7zip-full)"
unset ARV_APE ARV_SOURCE    # the real-disc checks use what a user has: the checkout, its arv.com

# ------------------------------------------------------------------ the reference scenarios
sh "$repo/tests/reference/scenarios.sh" ref "$tool" >/dev/null 2>scenarios.err || { cat scenarios.err; no "scenarios"; }
expected=$repo/tests/reference/expected
if ! diff -r -x recfile "$expected" ref > ref.diff; then
    head -40 ref.diff
    no "the reference scenarios differ from tests/reference/expected (diff in $dir/ref.diff)"
fi
ok "reference scenarios: $(find ref -type f | wc -l) files as expected/ holds them (make, --split, --draft, rebuild,"
echo "    links, --tools-history, --ro-crate, an interactive make, recording, editing, names, init, tags, queries)"
n=0
for f in "$repo"/tests/fixtures/recfile/*.rec "$repo/tests/reference/home/catalog/archive.rec"; do
    name=$(basename "$f")
    [ "$f" = "$repo/tests/reference/home/catalog/archive.rec" ] && name=home-archive.rec
    "$here/build/fixtures" --roundtrip "$f" roundtrip.rec || no "roundtrip $f"
    cmp -s roundtrip.rec "$expected/recfile/$name" || no "recfile writer differs from expected/ for $f"
    n=$((n + 1))
done
ok "recfile writer: $n files written byte for byte as expected"

# ------------------------------------------------------------------ a real disc: verify, restore, damage
mkdir -p src/docs src/bin
echo guide > src/docs/guide.md
printf '#!/bin/sh\necho hi\n' > src/bin/tool.sh
chmod 755 src/bin/tool.sh
ln -s docs/guide.md src/README.md
ln -s docs src/latest
ln -s missing.txt src/dead
git -C src init -q -b main
git -C src -c user.name=t -c user.email=t@example.invalid add -A
git -C src -c user.name=t -c user.email=t@example.invalid commit -q -m test
"$tool" make -C home --no-ecc --formats no --set CODE --output disc.iso src >/dev/null 2>&1 || no "arvc make"
7z x -odisc disc.iso >/dev/null || no "7z cannot read the image"
"$tool" verify disc >/dev/null && ok "verify: every file matches the manifests" || no "verify"
"$tool" restore disc out >/dev/null && ok "restore: no errors" || no "restore"
[ -z "$(git -C out status --porcelain)" ] && ok "restored tree is exactly the commit (git status clean)" \
    || no "git status: $(git -C out status --porcelain)"
[ -L out/README.md ] && [ -L out/latest ] && [ -L out/dead ] && [ -x out/bin/tool.sh ] \
    && ok "links and execute bits are back" || no "links or execute bits"
if command -v python3 >/dev/null; then
    python3 "$repo/upstream/bagit-python/bagit.py" --validate disc >/dev/null 2>&1 \
        && ok "the Library of Congress's bagit.py (upstream/bagit-python) agrees the disc is a valid bag" \
        || no "bagit.py says the disc is not a valid bag"
fi
cp -r disc bad
printf X | dd of=bad/data/docs/guide.md bs=1 seek=0 conv=notrunc 2>/dev/null
if "$tool" verify bad >out.txt; then no "verify missed damage"; fi
grep -q "FAILED   data/docs/guide.md" out.txt && ok "verify finds a damaged file" || no "damage report"
if "$tool" restore bad out-bad >out.txt; then no "restore missed damage"; fi
[ -f out-bad/README.md ] && [ ! -L out-bad/README.md ] \
    && ok "restore keeps a link's copy when its target is damaged" || no "copy of a damaged target"
[ -f disc/tools/arv/src/arvc/arvc.c ] && grep -q "Software: arvc@" disc/catalog.rec \
    && ok "tools/ carries arv's last commit, and the disc names it" || no "tools/ from the git checkout"
if [ -f "$here/build/arv.com" ]; then
    [ -f disc/tools/arv.com ] && grep -q "tools/arv.com" disc/README.txt || no "tools/arv.com missing, or README.txt silent on it"
    cp disc/tools/arv.com ape.com && chmod 755 ape.com     # copied off the disc, as README.txt says (7z drops modes)
    (cd disc && ../ape.com verify . >/dev/null) && ok "tools/arv.com (Actually Portable Executable), copied off the disc, verifies it" \
        || no "tools/arv.com from the disc"
fi

"$tool" --home fresh-home location add HOME "Home" >/dev/null && [ -s fresh-home/catalog/archive.rec ] \
    && ok "a catalogue command on a home that does not exist yet creates it" || no "location add on a new home"

# ------------------------------------------------------------------ what depends on the machine
"$tool" make -C hist --no-ecc --formats no --set CODE --tools-history --output-dir hist-out src >hist.txt 2>&1 \
    || no "arvc make --tools-history"
7z x -ohist-disc hist-out/*.iso >/dev/null
if [ -f hist-disc/tools/arv.bundle ]; then
    git bundle list-heads hist-disc/tools/arv.bundle | grep -q refs/ \
        && ok "--tools-history: tools/arv.bundle holds arv's branches" || no "the bundle holds no branches"
else
    grep -q "git bundle failed" hist.txt && ok "--tools-history: git could not bundle this checkout, as warned" \
        || no "--tools-history: no bundle and no warning"
fi
# RS03, added and tested by arv itself (src/rs03); no dvdisaster needed
"$tool" make -C ecc-home --formats no --set CODE --medium-sectors 9600 --output-dir ecc-out src >/dev/null 2>&1 \
    || no "arvc make with RS03"
iso=$(ls ecc-out/*.iso)
grep -q "RS03: " ecc-home/catalog/archive.rec && grep -q "Type: fixity check" ecc-home/catalog/archive.rec \
    && grep -q "Outcome: success" ecc-home/catalog/archive.rec \
    && [ $(($(wc -c < "$iso") / 2048)) -eq $((9600 / 255 * 255)) ] \
    && ok "arvc make with RS03 error correction: image filled to the medium and tested" || no "RS03 make"
[ "$(sed -n 's/^ImageSha256: //p' ecc-home/catalog/archive.rec)" = "$(sha256sum < "$iso" | cut -d' ' -f1)" ] \
    && [ "$(sed -n 's/^ImageSectors: //p' ecc-home/catalog/archive.rec)" -eq $(($(wc -c < "$iso") / 2048)) ] \
    && ok "the home's Binding records the finished image's size and SHA-256 (to check a burned disc against)" \
    || no "ImageSectors / ImageSha256"
# check --device reads the image's sectors back and compares (a file stands in for the drive)
id=$(sed -n 's/^Id: //p' ecc-home/catalog/archive.rec | head -1)
cp "$iso" drive.img
dd if=/dev/zero bs=2048 count=40 >> drive.img 2>/dev/null       # a drive gives sectors past the image too
out=$(PATH=/usr/bin:/bin "$tool" check -C ecc-home --device drive.img "$id" 2>&1) \
    && echo "$out" | grep -q "holds exactly the image arv made" && grep -q "Note: read-back of the whole image" ecc-home/catalog/archive.rec \
    || { echo "$out"; no "check --device read-back"; }
printf X | dd of=drive.img bs=1 seek=$((7000 * 2048)) conv=notrunc 2>/dev/null
"$tool" check -C ecc-home --device drive.img "$id" >/dev/null 2>&1 && no "check --device missed a changed sector"
head -c $((4000 * 2048)) "$iso" > drive.img
out=$("$tool" check -C ecc-home --device drive.img "$id" 2>&1) && no "check --device missed a short disc"
echo "$out" | grep -q "the disc ends at sector 4000" \
    && ok "check --device: reads the image back past the cache, no dvdisaster needed; finds a changed sector, a short disc" \
    || { echo "$out"; no "check --device on a short disc"; }
checks=$(grep -c 'Type: fixity check' ecc-home/catalog/archive.rec)
"$tool" check -C ecc-home --image "$iso" --note "yearly check" >/dev/null 2>&1 \
    && [ "$(grep -c 'Type: fixity check' ecc-home/catalog/archive.rec)" -eq $((checks + 1)) ] \
    && ok "arvc check --image: the image is whole, logged" || no "arvc check"
cp "$iso" damaged.iso
printf X | dd of=damaged.iso bs=1 seek=$((40 * 2048)) conv=notrunc 2>/dev/null
out=$("$tool" check -C ecc-home --image damaged.iso "$id" 2>&1) && no "check missed damage"
echo "$out" | grep -q "1 data sectors with a wrong CRC" && ok "arvc check --image finds a damaged sector" || { echo "$out"; no "damage report"; }
cp damaged.iso light.iso
# repaired by arv itself: a damaged sector, a zero-filled run (as ddrescue or dd leave) and a missing end
printf '\0\0\0\0\0\0\0\0' | dd of=damaged.iso bs=1 seek=$((3000 * 2048)) conv=notrunc 2>/dev/null
dd if=/dev/zero of=damaged.iso bs=2048 seek=5000 count=20 conv=notrunc 2>/dev/null
cp damaged.iso nohome.iso
truncate -s $(( ($(wc -c < "$iso") / 2048 - 100) * 2048 )) damaged.iso
out=$("$tool" check -C ecc-home --image damaged.iso --repair "$id" 2>&1) || { echo "$out"; no "arvc check --repair"; }
cmp -s damaged.iso "$iso" && echo "$out" | grep -q "$id: REPAIRED" && grep -q "Outcome: warning" ecc-home/catalog/archive.rec \
    && ok "arvc check --repair: damaged sectors, a zero-filled run and a missing end, back to the very image; logged" \
    || { echo "$out"; no "arvc check --repair"; }
# the layout's own copies lost (the header, the start of the CRC layer): arv hands over to dvdisaster
# Light, with the medium size from the catalogue
D=$(sed -n "s/^Note: image $id.iso, \([0-9]*\) sectors.*/\1/p" ecc-home/catalog/archive.rec | head -1)
spl=$((9600 / 255)); nd=$(( (D + 2 + spl - 1) / spl )); [ $nd -lt 84 ] && nd=84
cp "$iso" lost.iso
dd if=/dev/zero of=lost.iso bs=2048 seek=$D count=2 conv=notrunc 2>/dev/null
dd if=/dev/zero of=lost.iso bs=2048 seek=$((nd * spl)) count=10 conv=notrunc 2>/dev/null
printf 'damage' | dd of=lost.iso bs=1 seek=$((300 * 2048)) conv=notrunc 2>/dev/null
"$tool" check -C ecc-home --image lost.iso --repair "$id" >lost.txt 2>&1 && no "--repair claims to have repaired a lost layout"
grep -q "^    dvdisaster -i 'lost.iso' -f -n 9600$" lost.txt \
    && ok "--repair without a layout: prints the dvdisaster Light commands, with the catalogue's medium size" \
    || { cat lost.txt; no "the dvdisaster hand-over"; }
if command -v dvdisaster >/dev/null && dvdisaster --help 2>&1 | grep -q no-bdr-defect-management; then
    grep "^    dvdisaster" lost.txt > paste.sh
    sh paste.sh >/dev/null 2>&1 || true
    cmp -s lost.iso "$iso" && ok "dvdisaster Light, run with the commands as printed, repairs it to the very image" \
        || no "the pasted dvdisaster commands"
fi
mkdir -p nowhere
out=$(cd nowhere && ARV_HOME="$dir/nowhere/none" "$tool" check --image ../nohome.iso --repair 2>&1) || { echo "$out"; no "repair without a home"; }
cmp -s nohome.iso "$iso" && echo "$out" | grep -q "not logged" \
    && ok "arvc check --repair works on an image in no catalogue (as from a disc found decades on)" || { echo "$out"; no "repair without a home"; }
# overdue checks: a disc checked today is not overdue now, but is five years on; one never checked is
[ -z "$("$tool" list -C ecc-home --unchecked-since 5y)" ] \
    && SOURCE_DATE_EPOCH=$(( $(date +%s) + 6 * 366 * 86400 )) "$tool" list -C ecc-home --unchecked-since 5y | grep -q "last checked 20" \
    && "$tool" list -C hist --unchecked-since 1y | grep -q "last checked never" \
    && "$tool" list -C hist --one-place | grep -q "^CODE" \
    && ok "list --unchecked-since and --one-place: the discs due a check, and those kept in one place" \
    || no "list --unchecked-since / --one-place"
# the one cc line README.txt gives, from the disc's own tools/
mkdir -p cc-build
(cd disc && cc -O2 -pthread -o ../cc-build/arvc tools/arv/src/arvc/*.c tools/arv/src/bagit/bagit.c tools/arv/src/udfwrite/udfwrite.c \
    tools/arv/src/rs03/rs03.c >/dev/null 2>&1) && cc-build/arvc verify disc >/dev/null \
    && ok "README.txt's one cc line builds arv from the disc's tools/" || no "building arv from tools/"
if command -v dvdisaster >/dev/null && dvdisaster --help 2>&1 | grep -q no-bdr-defect-management; then
    dvdisaster -i "$iso" -t --no-progress 2>&1 | grep -q "Ecc block test *: pass" \
        && ok "dvdisaster Light accepts arv's RS03 image" || no "dvdisaster Light -t"
    dvdisaster -i light.iso -f --no-progress >/dev/null 2>&1 || true        # it exits 1 after repairing
    cmp -s light.iso "$iso" && ok "dvdisaster Light repairs it back to the image arv made" || no "dvdisaster Light -f"
fi
if command -v sf >/dev/null && sf -version >/dev/null 2>&1; then
    "$tool" make -C sf-home --no-ecc --formats yes --set CODE --output-dir sf-out src >/dev/null 2>&1 \
        || no "arvc make --formats yes"
    grep -q '^docs/guide.md,' sf-home/catalog/volumes/*/formats.csv \
        && grep -q "Type: format identification" sf-home/catalog/archive.rec \
        && ok "Siegfried: formats.csv and the format identification event" || no "Siegfried formats"
fi

# ------------------------------------------------------------------ SHA-256 and SHA-512 around block boundaries
mkdir -p lengths/data
printf '%%rec: Disc\n\nId: LEN-01_2026_X\n' > lengths/catalog.rec
printf 'BagIt-Version: 1.0\nTag-File-Character-Encoding: UTF-8\n' > lengths/bagit.txt
i=0
while [ $i -lt 256 ]; do
    # shellcheck disable=SC2059
    printf "\\$(printf %03o $(( (i * 7 + 3) % 256 )))"
    i=$((i + 1))
done > bytes
n=0
while [ $n -le 130 ]; do head -c $n bytes > "lengths/data/$(printf %03d $n)"; n=$((n + 1)); done
(cd lengths && sha256sum data/* > manifest-sha256.txt && sha256sum catalog.rec manifest-sha256.txt > tagmanifest-sha256.txt)
"$tool" verify lengths >/dev/null && ok "SHA-256 matches sha256sum for 0 to 130 bytes" || no "SHA-256"
(cd lengths && "$here/build/fixtures" --hash data/* > ../sha512.c.txt && sha512sum data/* > ../sha512.txt)
cmp -s sha512.c.txt sha512.txt && ok "SHA-512 matches sha512sum for 0 to 130 bytes" || no "SHA-512"

# ------------------------------------------------------------------ called as arv: the rest goes to the Python arv
if command -v python3 >/dev/null; then
    mkdir -p bin && ln -sf "$tool" bin/arv
    cp -r "$repo/tests/reference/home" arv-home
    [ "$(bin/arv --home arv-home describe 2>&1)" = "$(python3 "$repo/arv" --home arv-home describe 2>&1)" ] \
        || no "arv describe (handed to python) differs"
    [ "$(bin/arv --home arv-home sets)" = "$("$tool" -C arv-home sets)" ] || no "arv sets did not run in C"
    bin/arv --help 2>&1 | grep -q "usage: arv" || no "arv --help"
    ok "arv: describe goes to the Python add-on, sets and --help run in C"
fi
echo "all checks passed"
