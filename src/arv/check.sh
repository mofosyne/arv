#!/bin/sh
# Checks arv, without Python:
# - the reference scenarios (tests/reference/): every command run in a fixed setting must print
#   and write what expected/ holds (first written by the Python arv, since retired), byte for byte (normalised);
# - what only a real disc shows: a disc made by arv from a folder like a git clone verifies,
#   restores to a clean `git status` (links and execute bits included), and damage is found;
#   RS03 is added, tested (and damage found) and repaired by arv itself; with dvdisaster Light, it
#   accepts and repairs arv's images; with Siegfried, formats are identified; with arv's
#   git checkout as the source, tools/ gets the commit and its history; git repositories in a
#   folder go on as a compacted .git (trimmed with --git-since) and are recognised by their history;
# - SHA-256 and SHA-512 against sha256sum and sha512sum, and the recfile writer.
# - that describe, tag and models go to arv-assist, and gui to arv-gui.
# (tests/fixtures/ is checked by build/fixtures, which make check runs first.)
#
#   sh check.sh build/arv
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
"$tool" make -C home --no-ecc --formats no --set CODE --output disc.iso src >/dev/null 2>&1 || no "arv make"
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
[ -f disc/tools/arv/src/arv/arv.c ] && grep -q "Software: arv@" disc/catalog.rec \
    && ok "tools/ carries arv's last commit, and the disc names it" || no "tools/ from the git checkout"
if [ -f "$here/build/arv.com" ]; then
    [ -f disc/tools/arv.com ] && grep -q "tools/arv.com" disc/README.txt || no "tools/arv.com missing, or README.txt silent on it"
    cp disc/tools/arv.com ape.com && chmod 755 ape.com     # copied off the disc, as README.txt says (7z drops modes)
    (cd disc && ../ape.com verify . >/dev/null) && ok "tools/arv.com (Actually Portable Executable), copied off the disc, verifies it" \
        || no "tools/arv.com from the disc"
fi

"$tool" --home fresh-home location add HOME "Home" >/dev/null && [ -s fresh-home/catalog/archive.rec ] \
    && ok "a catalogue command on a home that does not exist yet creates it" || no "location add on a new home"


# ------------------------------------------------------------------ git repositories: compacted, trimmed, recognised
gitenv() { GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.invalid GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.invalid "$@"; }
mkdir -p g/src/proj
(
    cd g/src/proj && git init -q -b main
    for i in 1 2 3; do
        echo $i > f$i && git add f$i
        GIT_AUTHOR_DATE="200$i-01-01T00:00:00Z" GIT_COMMITTER_DATE="200$i-01-01T00:00:00Z" gitenv git commit -qm c$i
    done
    git remote add origin https://me:secret@example.invalid/x.git
    printf '#!/bin/sh\nexit 0\n' > .git/hooks/post-checkout && chmod +x .git/hooks/post-checkout
    echo wip > f1 && gitenv git stash -q && echo dirty > f2
)
"$tool" make -C g/home -y --no-ecc --formats no --set CODE --output g/full.iso g/src >g/make.txt 2>&1 || { cat g/make.txt; no "arv make (git)"; }
7z x -og/full g/full.iso >/dev/null
r=g/full/data/proj
git -C $r fsck --no-progress >/dev/null 2>&1 && [ "$(git -C $r rev-list --all | wc -l)" -eq 5 ] \
    && [ "$(git -C $r stash list | wc -l)" -eq 1 ] && [ "$(ls $r/.git/objects/pack/*.pack | wc -l)" -eq 1 ] \
    && ok "git: the disc holds a whole repository: one pack, every commit, the stash (fsck)" || no "git: the compacted .git"
[ "$(git -C $r status --porcelain)" = " M f2" ] && ok "git: the uncommitted change is kept, as a plain file" \
    || no "git status on the disc: $(git -C $r status --porcelain)"
[ ! -e $r/.git/hooks/post-checkout ] && ! grep -q secret $r/.git/config && grep -q "url = https://example.invalid/x.git" $r/.git/config \
    && ok "git: hooks and the credentials in remote URLs are left out" || no "git: hooks or credentials on the disc"
grep -q "^proj	root	" g/full/catalog/volumes/*/git.tsv && grep -q "^proj	commit	" g/full/catalog/volumes/*/git.tsv \
    && grep -q "git repository proj: 1 branch, 0 tags, 5 commits, history full" g/home/catalog/archive.rec \
    && ok "git: git.tsv lists its roots, heads and commits; the ingestion event says what was done" || no "git: git.tsv or event"
"$tool" make -C g/home -y --no-ecc --formats no --set CODE --git-since 2002-06-01 --output g/trim.iso g/src >/dev/null 2>&1 \
    || no "arv make --git-since"
7z x -og/trim g/trim.iso >/dev/null
t=g/trim/data/proj
git -C $t fsck --no-progress >/dev/null 2>&1 && [ -f $t/.git/shallow ] && ! git -C $t cat-file -e HEAD~1 2>/dev/null \
    && grep -q "^proj	root	$(git -C g/src/proj rev-list --max-parents=0 HEAD)" g/trim/catalog/volumes/CODE-02*/git.tsv \
    && ok "git: --git-since keeps the history since then (shallow, fsck clean); git.tsv keeps the true root" \
    || no "git: --git-since"
"$tool" status -C g/home g/src 2>/dev/null | grep -q "git proj: archived: its HEAD .* is on CODE-0.*; uncommitted changes" \
    && ok "git: status knows the repository is archived (by its HEAD)" || no "git: status, archived"
(cd g/src/proj && git checkout -q f2 && echo 4 > f4 && git add f4 && gitenv git commit -qm c4)
"$tool" status -C g/home g/src 2>/dev/null | grep -q "git proj: 1 commit newer than refs/heads/main on CODE-0" \
    && ok "git: and when it is a commit ahead of the disc" || no "git: status, ahead"
git clone -q g/src/proj g/other/proj && (cd g/other/proj && git reset -q --hard HEAD~2 && echo x > x && git add x && gitenv git commit -qm x)
mkdir -p g/other/new && (cd g/other/new && git init -q && echo y > y && git add y && gitenv git commit -qm y)
"$tool" status -C g/home g/other 2>/dev/null > g/status.txt
grep -q "git proj: diverged from CODE-0.*: 1 commit on no disc" g/status.txt && grep -q "git new: not archived" g/status.txt \
    && ok "git: a diverged clone and an unrelated repository are told apart, wherever they are" \
    || { cat g/status.txt; no "git: status, diverged or new"; }
"$tool" find -C g/home "$(git -C g/src/proj rev-parse --short=9 HEAD~1)" | grep -q "^GIT   CODE-01.*proj  commit .*(HEAD, refs/heads/main)" \
    && ok "git: arv find COMMIT names the discs holding it" || no "git: find a commit"
# ------------------------------------------------------------------ what depends on the machine
"$tool" make -C hist --no-ecc --formats no --set CODE --tools-history --output-dir hist-out src >hist.txt 2>&1 \
    || no "arv make --tools-history"
7z x -ohist-disc hist-out/*.iso >/dev/null
if [ -f hist-disc/tools/arv.bundle ]; then
    git bundle list-heads hist-disc/tools/arv.bundle | grep -q refs/ \
        && ok "--tools-history: tools/arv.bundle holds arv's branches" || no "the bundle holds no branches"
else
    grep -q "git bundle failed" hist.txt && ok "--tools-history: git could not bundle this checkout, as warned" \
        || no "--tools-history: no bundle and no warning"
fi
# RS03, added and tested by arv itself (src/rs03); no dvdisaster needed. A small stand-in for
# tools/: the real checkout is far larger than this test's medium (a real disc carries it all)
mkdir -p ecc-tools/src/arv
echo "arv@test" > ecc-tools/VERSION
printf '/* arv.c stand-in */\n' > ecc-tools/src/arv/arv.c
"$tool" make -C ecc-home --formats no --set CODE --medium-sectors 9600 --output-dir ecc-out --tools ecc-tools src >/dev/null 2>&1 \
    || no "arv make with RS03"
iso=$(ls ecc-out/*.iso)
grep -q "RS03: " ecc-home/catalog/archive.rec && grep -q "Type: fixity check" ecc-home/catalog/archive.rec \
    && grep -q "Outcome: success" ecc-home/catalog/archive.rec \
    && [ $(($(wc -c < "$iso") / 2048)) -eq $((9600 / 255 * 255)) ] \
    && ok "arv make with RS03 error correction: image filled to the medium and tested" || no "RS03 make"
# a file that changes after it was hashed, before its bytes reach the image: refused, nothing recorded
# (ARV_TEST_AFTER_HASH: a command arv make runs between the two, for this test only)
mkdir -p race-src
echo "first" > race-src/a.txt
echo "steady" > race-src/b.txt
out=$(ARV_TEST_AFTER_HASH="echo second > race-src/a.txt" "$tool" make -y -C race-home --formats no --no-ecc --set CODE \
      --output-dir race-out --tools ecc-tools race-src 2>&1) && no "a file changed mid-make was not refused"
echo "$out" | grep -q "changed after it was hashed" && echo "$out" | grep -q "^  a.txt$" \
    && [ -z "$(ls race-out/*.iso 2>/dev/null)" ] && ! grep -q "^Id:" race-home/catalog/archive.rec 2>/dev/null \
    && ok "arv make: a file changed between hashing and writing is refused, and nothing is recorded" \
    || no "a file changed mid-make: $out"
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
    && ok "arv check --image: the image is whole, logged" || no "arv check"
cp "$iso" damaged.iso
printf X | dd of=damaged.iso bs=1 seek=$((40 * 2048)) conv=notrunc 2>/dev/null
out=$("$tool" check -C ecc-home --image damaged.iso "$id" 2>&1) && no "check missed damage"
echo "$out" | grep -q "1 data sectors with a wrong CRC" && ok "arv check --image finds a damaged sector" || { echo "$out"; no "damage report"; }
cp damaged.iso light.iso
# repaired by arv itself: a damaged sector, a zero-filled run (as ddrescue or dd leave) and a missing end
printf '\0\0\0\0\0\0\0\0' | dd of=damaged.iso bs=1 seek=$((3000 * 2048)) conv=notrunc 2>/dev/null
dd if=/dev/zero of=damaged.iso bs=2048 seek=5000 count=20 conv=notrunc 2>/dev/null
cp damaged.iso nohome.iso
truncate -s $(( ($(wc -c < "$iso") / 2048 - 100) * 2048 )) damaged.iso
out=$("$tool" check -C ecc-home --image damaged.iso --repair "$id" 2>&1) || { echo "$out"; no "arv check --repair"; }
cmp -s damaged.iso "$iso" && echo "$out" | grep -q "$id: REPAIRED" && grep -q "Outcome: warning" ecc-home/catalog/archive.rec \
    && ok "arv check --repair: damaged sectors, a zero-filled run and a missing end, back to the very image; logged" \
    || { echo "$out"; no "arv check --repair"; }
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
    && ok "arv check --repair works on an image in no catalogue (as from a disc found decades on)" || { echo "$out"; no "repair without a home"; }
# overdue checks: a disc checked today is not overdue now, but is five years on; one never checked is
[ -z "$("$tool" list -C ecc-home --unchecked-since 5y)" ] \
    && SOURCE_DATE_EPOCH=$(( $(date +%s) + 6 * 366 * 86400 )) "$tool" list -C ecc-home --unchecked-since 5y | grep -q "last checked 20" \
    && "$tool" list -C hist --unchecked-since 1y | grep -q "last checked never" \
    && "$tool" list -C hist --one-place | grep -q "^CODE" \
    && ok "list --unchecked-since and --one-place: the discs due a check, and those kept in one place" \
    || no "list --unchecked-since / --one-place"
# the one cc line README.txt gives, from the disc's own tools/
mkdir -p cc-build
(cd disc && cc -O2 -pthread -o ../cc-build/arv tools/arv/src/arv/*.c tools/arv/src/bagit/bagit.c tools/arv/src/udfwrite/udfwrite.c \
    tools/arv/src/rs03/rs03.c >/dev/null 2>&1) && cc-build/arv verify disc >/dev/null \
    && ok "README.txt's one cc line builds arv from the disc's tools/" || no "building arv from tools/"
if command -v dvdisaster >/dev/null && dvdisaster --help 2>&1 | grep -q no-bdr-defect-management; then
    dvdisaster -i "$iso" -t --no-progress 2>&1 | grep -q "Ecc block test *: pass" \
        && ok "dvdisaster Light accepts arv's RS03 image" || no "dvdisaster Light -t"
    dvdisaster -i light.iso -f --no-progress >/dev/null 2>&1 || true        # it exits 1 after repairing
    cmp -s light.iso "$iso" && ok "dvdisaster Light repairs it back to the image arv made" || no "dvdisaster Light -f"
fi
if command -v sf >/dev/null && sf -version >/dev/null 2>&1; then
    "$tool" make -C sf-home --no-ecc --formats yes --set CODE --output-dir sf-out src >/dev/null 2>&1 \
        || no "arv make --formats yes"
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

# ------------------------------------------------------------------ the helpers: arv-assist and arv-gui
mkdir -p helpers
printf '#!/bin/sh\necho "assist $*"\n' > helpers/assist
printf '#!/bin/sh\necho "gui $*"\n' > helpers/gui
chmod +x helpers/assist helpers/gui
cp -r "$repo/tests/reference/home" arv-home
[ "$(ARV_ASSIST=helpers/assist "$tool" --home arv-home describe Photos --save d.json)" = "assist --home arv-home describe Photos --save d.json" ] \
    || no "arv describe was not handed to arv-assist"
[ "$(ARV_ASSIST=helpers/assist "$tool" models status)" = "assist models status" ] || no "arv models"
[ "$(ARV_GUI=helpers/gui "$tool" -C arv-home gui --port 0)" = "gui --home arv-home --port 0" ] \
    || no "arv gui was not handed to arv-gui with the home"
ARV_ASSIST=/nonexistent/arv-assist PATH=/nonexistent "$tool" tag x 2>&1 | grep -q "needs arv-assist" || no "missing arv-assist message"
"$tool" --help 2>&1 | grep -q "usage: arv" || no "arv --help"
ok "arv: describe, tag and models go to arv-assist, gui to arv-gui (with the home); the rest runs here"
echo "all checks passed"
