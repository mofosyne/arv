#!/bin/sh
# Checks arvc against a disc made by arv (python), from a folder like a git clone:
# verify passes, restore gives back the same tree (git status is clean: links and execute bits
# included), and damage is found. find and list are compared with python's on samples/home.
# (tests/fixtures/ is checked by build/fixtures, which make check runs first.)
#
#   sh check.sh build/arvc
# Needs python3 and git, and src/udfwrite built (arv make's default writer).
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

python3 "$repo/arv" --home home make -y --no-ecc --formats no --set CODE --output disc.iso src >/dev/null 2>&1 \
    || no "arv make"
mkdir disc
python3 - <<'PY' || no "extracting the image needs 7z"
import shutil, subprocess, sys
sys.exit(0 if shutil.which("7z") and subprocess.run(["7z", "x", "-odisc", "disc.iso"], stdout=subprocess.DEVNULL).returncode == 0 else 1)
PY
"$tool" verify disc >/dev/null && ok "verify: every file matches the manifests" || no "verify"
"$tool" restore disc out >/dev/null && ok "restore: no errors" || no "restore"
[ -z "$(git -C out status --porcelain)" ] && ok "restored tree is exactly the commit (git status clean)" \
    || no "git status: $(git -C out status --porcelain)"
[ -L out/README.md ] && [ -L out/latest ] && [ -L out/dead ] && [ -x out/bin/tool.sh ] \
    && ok "links and execute bits are back" || no "links or execute bits"

cp -r disc bad
printf X | dd of=bad/data/docs/guide.md bs=1 seek=0 conv=notrunc 2>/dev/null
if "$tool" verify bad >out.txt; then no "verify missed damage"; fi
grep -q "FAILED   data/docs/guide.md" out.txt && ok "verify finds a damaged file" || no "damage report"
if "$tool" restore bad out-bad >out.txt; then no "restore missed damage"; fi
[ -f out-bad/README.md ] && [ ! -L out-bad/README.md ] \
    && ok "restore keeps a link's copy when its target is damaged" || no "copy of a damaged target"
# arvc make writes the same disc as the Python arv: two discs into a fresh home each (the second
# carries the first in its catalogue snapshot), every file compared (UUIDs and versions aside)
mkdir -p second/letters
echo "dear diary" > second/letters/2001-05-01.txt
touch -d '2001-05-01 12:00' second/letters/2001-05-01.txt
printf '%%PDF-1.4\n%%EOF\n' > "second/letters/scan,page1.pdf"     # a comma: quoted in formats.csv
touch -d '2001-05-02 12:00' "second/letters/scan,page1.pdf"
fmt="--formats no"                     # with Siegfried, both identify formats (formats.csv compared too)
command -v sf >/dev/null && sf -version >/dev/null 2>&1 && fmt="--formats yes"
for who in py c; do
    mkdir -p "$who-home" "$who-out"
    for folder in src second; do
        if [ $who = py ]; then
            python3 "$repo/arv" --home "$who-home" make -y --no-ecc $fmt --set CODE --location BOX1 \
                --importance "essential for self" --output-dir "$who-out" $folder >/dev/null 2>&1 || no "python make $folder"
        else
            "$tool" make -C "$who-home" --no-ecc $fmt --set CODE --location BOX1 --importance "essential for self" \
                --output-dir "$who-out" $folder >/dev/null 2>&1 || no "arvc make $folder"
        fi
    done
done
norm='s/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/UUID/g; s/arvc?@[0-9a-f]{12}(\+uncommitted)?/SW/g'
compared=0
for iso in py-out/*.iso; do
    name=$(basename "$iso")
    [ -f "c-out/$name" ] || no "arvc made no $name"
    rm -rf x-py x-c
    7z x -ox-py "$iso" >/dev/null && 7z x -ox-c "c-out/$name" >/dev/null
    [ "$(cd x-py && find . | sort)" = "$(cd x-c && find . | sort)" ] || no "$name: different files"
    # tag manifests hash the files that differ; extents.tsv (where each file starts in an earlier
    # image) differs because "arvc@" is a byte longer than "arv@", which moves later files
    for f in $(cd x-py && find . -type f -not -name 'tagmanifest-*' -not -name extents.tsv | sort); do
        cmp -s "x-py/$f" "x-c/$f" && continue
        [ "$(sed -E "$norm" "x-py/$f")" = "$(sed -E "$norm" "x-c/$f")" ] || no "$name: $f differs from python's"
    done
    python3 x-c/tools/bagit.py --validate x-c >/dev/null 2>&1 || no "$name: bagit.py says the arvc disc is not valid"
    compared=$((compared + 1))
done
[ "$(sed -E "$norm" py-home/catalog/archive.rec)" = "$(sed -E "$norm" c-home/catalog/archive.rec)" ] \
    || no "home catalogues differ"
for f in $(cd py-home/catalog/volumes && find . -type f -not -name extents.tsv | sort); do
    cmp -s "py-home/catalog/volumes/$f" "c-home/catalog/volumes/$f" || no "home file list $f differs"
done
python3 "$repo/arv" --home c-home list >/dev/null || no "python cannot read the home arvc made"
ok "arvc make: $compared discs written as python writes them (bagit-valid; the home catalogue too$([ "$fmt" = "--formats yes" ] && echo "; Siegfried formats.csv"))"
if command -v dvdisaster >/dev/null && dvdisaster --help 2>&1 | grep -q no-bdr-defect-management; then
    "$tool" make -C ecc-home --formats no --set CODE --medium-sectors 4800 --output-dir ecc-out src >/dev/null 2>&1 || no "arvc make with RS03"
    grep -q "RS03: " ecc-home/catalog/archive.rec && grep -q "Type: fixity check" ecc-home/catalog/archive.rec \
        && ok "arvc make with RS03 error correction: image tested by dvdisaster" || no "RS03 events"
fi

# --split: the same discs, the same files on each, and the same rebalancing as the Python arv
mkdir -p big/letters
python3 -c "
import os, random
r = random.Random(7)
for i in range(70):
    p = 'big/letters/letter-%03d.txt' % i
    open(p, 'wb').write(r.randbytes(100000))
    os.utime(p, (1000000000 + i * 86400 * 30,) * 2)"
python3 "$repo/arv" --home split-py make -y --no-ecc --formats no --set SCAN --split --medium-sectors 4800 \
    --output-dir split-py-out big > split-py.txt 2>&1 || no "python make --split"
"$tool" make -C split-c --no-ecc --formats no --set SCAN --split --medium-sectors 4800 --output-dir split-c-out big \
    > split-c.txt 2>&1 || no "arvc make --split"
[ "$(grep -E '^Rebalancing|^SCAN' split-py.txt | sed 's|split-py-out|OUT|')" = \
  "$(grep -E '^Rebalancing|^SCAN' split-c.txt | sed 's|split-c-out|OUT|')" ] || no "--split divides the files differently"
discs=0
for iso in split-py-out/*.iso; do
    name=$(basename "$iso")
    rm -rf x-py x-c
    7z x -ox-py "$iso" >/dev/null && 7z x -ox-c "split-c-out/$name" >/dev/null || no "$name missing"
    [ "$(cd x-py && find . | sort)" = "$(cd x-c && find . | sort)" ] || no "$name: different files"
    for f in $(cd x-py && find . -type f -not -name 'tagmanifest-*' -not -name extents.tsv | sort); do
        cmp -s "x-py/$f" "x-c/$f" && continue
        [ "$(sed -E "$norm" "x-py/$f")" = "$(sed -E "$norm" "x-c/$f")" ] || no "$name: $f differs from python's"
    done
    discs=$((discs + 1))
done
[ "$(sed -E "$norm" split-py/catalog/archive.rec)" = "$(sed -E "$norm" split-c/catalog/archive.rec)" ] || no "--split: home catalogues differ"
[ $discs -gt 1 ] || no "--split made only one disc"
ok "arvc make --split: $discs discs, divided and written as python does"

# rebuild: a home rebuilt from discs (the split ones above), the same as the Python arv's; then a
# hand edit at home that --prefer-disc replaces with the disc's record
rm -rf rb-discs && mkdir rb-discs
for iso in split-py-out/*.iso; do 7z x -o"rb-discs/$(basename "$iso" .iso)" "$iso" >/dev/null; done
first=$(ls rb-discs | head -1)
for who in py c; do
    rm -rf "rb-$who" && : > "rb-$who.out"
    if [ $who = py ]; then run="python3 $repo/arv --home"; else run="$tool -C"; fi
    for d in rb-discs/*; do $run "rb-$who" rebuild "$d" >> "rb-$who.out" 2>&1 || no "$who rebuild $d"; done
    $run "rb-$who" note "$(echo "$first" | cut -d. -f1)" "edited at home" >/dev/null 2>&1
    $run "rb-$who" rebuild --prefer-disc "rb-discs/$first" >> "rb-$who.out" 2>&1 || no "$who rebuild --prefer-disc"
    sed -i "s|rb-$who|HOME|" "rb-$who.out"
done
cmp -s rb-py.out rb-c.out || { diff rb-py.out rb-c.out; no "rebuild prints differently"; }
grep -q "updated 1," rb-c.out || no "--prefer-disc did not replace the edited record"
cmp -s rb-py/catalog/archive.rec rb-c/catalog/archive.rec || no "rebuild: catalogues differ"
diff -r rb-py/catalog/volumes rb-c/catalog/volumes >/dev/null || no "rebuild: file lists differ"
ok "arvc rebuild: the home catalogue and file lists as python rebuilds them (--prefer-disc too)"

# burned, note, locate (and check, with dvdisaster) change the catalogue as the Python arv does
for who in py c; do
    rm -rf "rec-$who" && cp -r "$repo/samples/home" "rec-$who"
    [ -d ecc-home ] && rm -rf "ecc-$who" && cp -r ecc-home "ecc-$who"
    if [ $who = py ]; then run="python3 $repo/arv --home"; else run="$tool -C"; fi
    {
        $run "rec-$who" burned TRIP-01_2019_4 --copies 2 --location box1 --media-id MEI-T02 --note "for the attic"
        $run "rec-$who" note PROJ-01_2020-2023_L "A long note that goes on about the weather station, its board revisions and the spare parts"
        $run "rec-$who" locate SCAN-01_1995-2008_D SAFE --add
        $run "rec-$who" locate FAMILY-01_2020-2021_K "Somewhere new"
        if [ -d ecc-home ]; then
            $run "ecc-$who" check --image ecc-out/*.iso --note "yearly check"     # the id from the image's label
        fi
    } >/dev/null 2>&1 || no "$who: burned, note, locate or check failed"
done
cmp -s rec-py/catalog/archive.rec rec-c/catalog/archive.rec || no "burned, note, locate: catalogues differ"
[ ! -d ecc-home ] || cmp -s ecc-py/catalog/archive.rec ecc-c/catalog/archive.rec || no "check: catalogues differ"
ok "burned, note, locate$([ -d ecc-home ] && echo ", check"): the catalogue byte for byte as python leaves it"

# access, location, collection, appraise, sets, where and names: the same output, exit codes and
# catalogue as the Python arv (dev/edit-cases.txt)
mkdir -p names/Sub names/sub
(cd names && touch plain.txt "star*?.txt" "photo 😀.txt" "$(printf '%0120d' 0)" Sub/File.txt sub/file.txt "semi;colon")
for who in py c; do
    rm -rf "edit-$who" && cp -r "$repo/samples/home" "edit-$who"
    : > "edit-$who.out"
    grep -v '^#' "$here/dev/edit-cases.txt" | while IFS= read -r line; do
        eval "set -- $line"
        echo "\$ $line" >> "edit-$who.out"
        if [ $who = py ]; then python3 "$repo/arv" --home "edit-$who" "$@" >> "edit-$who.out" 2>/dev/null && rc=0 || rc=$?
        else "$tool" -C "edit-$who" "$@" >> "edit-$who.out" 2>/dev/null && rc=0 || rc=$?; fi
        echo "rc=$rc" >> "edit-$who.out"
    done
    for fs in "" "--limit 2"; do
        if [ $who = py ]; then python3 "$repo/arv" names $fs names >> "edit-$who.out" 2>/dev/null && rc=0 || rc=$?
        else "$tool" names $fs names >> "edit-$who.out" 2>/dev/null && rc=0 || rc=$?; fi
        echo "rc=$rc" >> "edit-$who.out"
    done
    sed -i "s|edit-$who|HOME|g" "edit-$who.out"
done
cmp -s edit-py.out edit-c.out || { diff edit-py.out edit-c.out | head -20; no "editing commands print differently"; }
cmp -s edit-py/catalog/archive.rec edit-c/catalog/archive.rec || no "editing commands leave different catalogues"
ok "access, location, collection, appraise, sets, names, where: $(grep -c '^rc=' edit-c.out) runs as python's, catalogue too"

# init (named homes, pointer files) and --archive: the same output, machine config and pointers
for who in py c; do
    d=$dir/init-$who
    mkdir -p "$d/a" "$d/b" "$d/c/sub" "$d/cfg"
    if [ $who = py ]; then run() { python3 "$repo/arv" "$@"; }; else run() { "$tool" "$@"; }; fi
    (
        cd "$d"
        export XDG_CONFIG_HOME="$d/cfg"
        unset ARV_HOME BLURAY_ARCHIVE_HOME
        for args in "init a --name main" "init b --name other --default" "init a --name main" \
                    "init c --pointer a/.arv" "init c/sub --pointer nowhere" \
                    "init b/../c/sub --pointer ./c/../a/.arv" "--archive main where" "--archive nope where"; do
            # shellcheck disable=SC2086
            run $args 2>&1 && rc=0 || rc=$?
            echo "rc=$rc"
        done
        (cd c/sub && run where 2>&1)
        (cd / && run where 2>&1)
        cat cfg/arv/homes.rec c/.arv c/sub/.arv
    ) | sed "s#$d#D#g" > "init-$who.out"
done
cmp -s init-py.out init-c.out || { diff init-py.out init-c.out | head -20; no "init and --archive differ"; }
ok "init --name/--default/--pointer and --archive: output, homes.rec and pointer files as python's"

# tags and keywords: the same output as the Python arv, on the samples with extra tags (an alias,
# a word not in the vocabulary, a non-ASCII value), and the same tags.rec created on first use
for who in py c; do
    rm -rf "tags-$who"
    cp -r "$repo/samples/home" "tags-$who"
    rm -f "tags-$who/config/tags.rec"
    printf '.\tHoliday, Zzz, person:\303\205lice, place:kyoto\nsub dir\tkids,  travel ,\n' \
        >> "tags-$who/catalog/volumes/TRIP-01_2019_4/tags.tsv"
    if [ $who = py ]; then run() { python3 "$repo/arv" --home "tags-$who" "$@"; }; else run() { "$tool" -C "tags-$who" "$@"; }; fi
    {
        run tags; run tags --namespace place; run tags --namespace ""; run tags --namespace nope
        run tags --vocab "$repo/src/arv/default_tags.rec"
        for d in $(run list | cut -f1) NOPE-01_2000_X; do
            for f in tsv exiftool; do run keywords "$d" --format $f 2>&1 && rc=0 || rc=$?; echo "rc=$rc"; done
        done
    } | sed "s#tags-$who#H#g" > "tags-$who.out"
done
cmp -s tags-py.out tags-c.out || { diff tags-py.out tags-c.out | head -20; no "tags and keywords differ"; }
cmp -s tags-py/config/tags.rec tags-c/config/tags.rec || no "tags creates a different tags.rec"
ok "tags and keywords: $(grep -c . tags-c.out) lines as python prints them; tags.rec created alike"

# called as arv, the program runs what it has and hands the rest to the Python arv
mkdir -p bin && ln -sf "$tool" bin/arv
[ "$(bin/arv --home "$repo/samples/home" describe 2>&1)" = "$(python3 "$repo/arv" --home "$repo/samples/home" describe 2>&1)" ] \
    || no "arv describe (handed to python) differs"
[ "$(bin/arv --home "$repo/samples/home" sets)" = "$(python3 "$repo/arv" --home "$repo/samples/home" sets)" ] \
    || no "arv sets differs"
bin/arv --help 2>&1 | grep -q "usage: arv" || no "arv --help (python's)"
ok "arv: ported commands run in C, the others (describe, --help) in the Python arv"

# find and list give the same lines as the Python arv, on the sample catalogue
same=0
for q in kyoto IMG '*.png' 'place:*' BOX 2019 nothing-matches; do
    [ "$(python3 "$repo/arv" --home "$repo/samples/home" find "$q" 2>/dev/null)" = \
      "$("$tool" find -C "$repo/samples/home" "$q" 2>/dev/null)" ] || no "find $q differs from python"
    same=$((same + 1))
done
for o in "" "--in MEMORIES" "--at BOX1" "--access sealed" "--made 2026" "--covers 2019" "--covers 1995-06"; do
    [ "$(python3 "$repo/arv" --home "$repo/samples/home" list $o 2>&1)" = "$("$tool" list -C "$repo/samples/home" $o 2>&1)" ] \
        || no "list $o differs from python"
    same=$((same + 1))
done
for q in TRIP-01_2019_4 trip-01_2019_4 TRIP-01_2019_5 PROJ-01_2020-2O23_L 2020-2025_PROJECTS_01 "not an id"; do
    [ "$(python3 "$repo/arv" --home "$repo/samples/home" id "$q" 2>&1; echo $?)" = \
      "$("$tool" id -C "$repo/samples/home" "$q" 2>&1; echo $?)" ] || no "id $q differs from python"
    same=$((same + 1))
done
ok "find, list and id: $same queries give the same lines as python"

# SHA-256 at every length around the 64-byte block and padding boundaries, against sha256sum
mkdir -p lengths/data
printf '%%rec: Disc\n\nId: LEN-01_2026_X\n' > lengths/catalog.rec
python3 -c "
for n in range(131):
    open('lengths/data/%03d' % n, 'wb').write(bytes((i * 7 + n) % 256 for i in range(n)))"
(cd lengths && sha256sum data/* > manifest-sha256.txt && sha256sum catalog.rec manifest-sha256.txt > tagmanifest-sha256.txt)
"$tool" verify lengths >/dev/null && ok "SHA-256 matches sha256sum for 0 to 130 bytes" || no "SHA-256"
(cd lengths && "$here/build/fixtures" --hash data/* > ../sha512.c.txt && sha512sum data/* > ../sha512.txt)
cmp -s sha512.c.txt sha512.txt && ok "SHA-512 matches sha512sum for 0 to 130 bytes" || no "SHA-512"
n=0
for f in "$repo"/samples/home/catalog/archive.rec "$repo"/tests/fixtures/recfile/*.rec; do
    "$here/build/fixtures" --roundtrip "$f" roundtrip.rec || no "roundtrip $f"
    python3 -c "import sys; sys.path.insert(0, '$repo/src'); from arv import recfile; sys.stdout.write(recfile.dumps(recfile.read('$f')))" > expected.rec
    cmp -s roundtrip.rec expected.rec || no "recfile writer differs from python for $f"
    n=$((n + 1))
done
ok "recfile writer: $n files written byte for byte as python writes them"
echo "all checks passed"
