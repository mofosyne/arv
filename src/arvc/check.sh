#!/bin/sh
# Checks arvc, without Python:
# - the reference scenarios (tests/reference/): every command run in a fixed setting must print
#   and write what the Python arv did when expected/ was generated, byte for byte (normalised);
# - what only a real disc shows: a disc made by arvc from a folder like a git clone verifies,
#   restores to a clean `git status` (links and execute bits included), and damage is found;
#   with dvdisaster, RS03 is added and tested; with Siegfried, formats are identified; with arv's
#   git checkout as the source, tools/ gets the commit and its history;
# - SHA-256 and SHA-512 against sha256sum and sha512sum, and the recfile writer.
# With python3 on PATH it also checks that arvc called as arv hands what it lacks to the Python arv.
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
ok "reference scenarios: $(find ref -type f | wc -l) files as the Python arv wrote them (make, --split, rebuild,"
echo "    links, --tools-history, --ro-crate, an interactive make, recording, editing, names, init, tags, queries)"
n=0
for f in "$repo"/tests/fixtures/recfile/*.rec "$repo/tests/reference/home/catalog/archive.rec"; do
    name=$(basename "$f")
    [ "$f" = "$repo/tests/reference/home/catalog/archive.rec" ] && name=home-archive.rec
    "$here/build/fixtures" --roundtrip "$f" roundtrip.rec || no "roundtrip $f"
    cmp -s roundtrip.rec "$expected/recfile/$name" || no "recfile writer differs from python for $f"
    n=$((n + 1))
done
ok "recfile writer: $n files written byte for byte as python writes them"

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
    python3 disc/tools/bagit.py --validate disc >/dev/null 2>&1 && ok "bagit.py (from the disc's tools/) validates it" \
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
if command -v dvdisaster >/dev/null && dvdisaster --help 2>&1 | grep -q no-bdr-defect-management; then
    "$tool" make -C ecc-home --formats no --set CODE --medium-sectors 9600 --output-dir ecc-out src >/dev/null 2>&1 \
        || no "arvc make with RS03"
    grep -q "RS03: " ecc-home/catalog/archive.rec && grep -q "Type: fixity check" ecc-home/catalog/archive.rec \
        && ok "arvc make with RS03 error correction: image tested by dvdisaster" || no "RS03 events"
    "$tool" check -C ecc-home --image ecc-out/*.iso --note "yearly check" >/dev/null 2>&1 \
        && [ "$(grep -c 'Type: fixity check' ecc-home/catalog/archive.rec)" -eq 2 ] \
        && ok "arvc check: the image passes dvdisaster again, logged" || no "arvc check"
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
    bin/arv --help 2>&1 | grep -q "usage: arv" || no "arv --help (python's)"
    ok "arv: ported commands run in C, the others (describe, --help) in the Python arv"
fi
echo "all checks passed"
