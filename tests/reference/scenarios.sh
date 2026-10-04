#!/bin/sh
# The reference scenarios: every command arv has, run in a fixed setting, their output and the
# files they write kept, normalised, in OUT. Run with the Python arv they make expected/ (see
# generate.sh); src/arvc/check.sh runs them with arvc and compares with expected/, so the C port
# is held to the Python arv's behaviour without needing Python.
#
#   sh tests/reference/scenarios.sh OUT ARV-COMMAND...
#   e.g. sh tests/reference/scenarios.sh /tmp/ref python3 ./arv
#        sh tests/reference/scenarios.sh /tmp/ref src/arvc/build/arvc
#
# Fixed: the clock (SOURCE_DATE_EPOCH), the time zone, the user, the machine config, the file
# dates of every input, and tools/ (ARV_SOURCE: a small stand-in tree, and ARV_APE: a stand-in
# arv.com, so discs do not change with every commit). Not covered here: RS03 (dvdisaster), Siegfried and git checkouts as
# tools/, whose output depends on the machine; check.sh tests those of arvc directly.
# Needs udfwrite (the Python arv runs it; arvc has it built in) and src/arvc/build/ptyrun.
set -eu
[ $# -ge 2 ] || { echo "usage: scenarios.sh OUT ARV-COMMAND..." >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=$1
shift
mkdir -p "$out"
out=$(cd "$out" && pwd)
ARV0=""
ARVARGS=""
for word in "$@"; do        # the command's words; paths made absolute (the scenarios change folder)
    case $word in */*) [ -e "$word" ] && word=$(cd "$(dirname "$word")" && pwd)/$(basename "$word") ;; esac
    if [ -z "$ARV0" ]; then ARV0=$word; else ARVARGS="$ARVARGS $word"; fi
done
ptyrun=$repo/src/arvc/build/ptyrun
[ -x "$ptyrun" ] || { echo "scenarios.sh: build src/arvc first (make -C src/arvc)" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
root=$work/r
mkdir -p "$root"
cd "$root"
export TZ=UTC LC_ALL=C USER=archivist HOME="$root/userhome" XDG_CONFIG_HOME="$root/userhome/.config" \
    XDG_DATA_HOME="$root/userhome/.local/share" SOURCE_DATE_EPOCH=1767225600 ARV_SOURCE="$root/arv-source" \
    ARV_APE="$root/arv.com"
unset ARV_HOME BLURAY_ARCHIVE_HOME
mkdir -p "$HOME"

a() {   # the arv command, with its fixed leading words (python3 path/arv), then the arguments
    # shellcheck disable=SC2086
    "$ARV0" $ARVARGS "$@"
}
log() {   # log FILE ARGS...: "$ ARGS", the output (stdout and stderr) and the exit status
    f=$1
    shift
    echo "\$ $*" >> "$f"
    a "$@" >> "$f" 2>&1 && rc=0 || rc=$?
    echo "rc=$rc" >> "$f"
}

# the normalised copy of a text file: this run's folder, UUIDs, the software name (arv@ or arvc@),
# temporary names, and the time stamps of files written while making (the RO-Crate files)
norm() {
    sed -E -e "s#$root#ROOT#g" -e "s#$repo#REPO#g" \
        -e 's/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/UUID/g' \
        -e 's/arvc?@reference/SW/g' \
        -e 's/(stage-[A-Za-z0-9_-]+-)[A-Za-z0-9_]{6,8}/\1XXXXXX/g' \
        -e 's/\.archive-make-[A-Za-z0-9_]{6,8}/.archive-make-XXXXXX/g' \
        -e 's/\t20[0-9-]+T[0-9:]+Z\t(file\t-\tro-crate)/\tTIME\t\1/' "$1"
}
keep() {   # keep FILE DEST: the normalised copy, at DEST under OUT
    mkdir -p "$(dirname "$out/$2")"
    norm "$1" > "$out/$2"
}
keep_tree() {   # keep_tree DIR DEST: every text file under DIR but tools/, tag manifests and extents
    (cd "$1" && find . -type f -not -path './tools/*' -not -name 'tagmanifest-*' -not -name extents.tsv | sort) |
    while IFS= read -r f; do keep "$1/$f" "$2/${f#./}"; done
    if [ -d "$1/tools" ]; then
        mkdir -p "$out/$2"
        (cd "$1/tools" && find . -maxdepth 2 | sort) > "$out/$2/tools.list"
    fi
}
keep_stages() {   # keep_stages OUTDIR DEST: the kept work folder of a make, stage by stage (by disc id)
    for s in "$1"/.archive-make-*/stage-*; do
        [ -d "$s" ] || continue
        id=$(sed -n 's/^Disc: //p' "$s/catalog.rec" | head -1)
        keep_tree "$s" "$2/$id"
    done
    for b in "$1"/.archive-make-*/batch; do
        [ -d "$b" ] || continue
        (cd "$b" && find . -type f -name '*.json' -o -type f -name '*.html' | sort) |
        while IFS= read -r f; do keep "$b/$f" "$2/batch/${f#./}"; done
    done
}
keep_home() {   # keep_home HOME DEST: the catalogue and file lists (extents aside)
    keep_tree "$1/catalog" "$2"
}
stamp() { touch -h -d "@$1" "$2"; }

# tools/: a small stand-in for arv's source, the same for both
mkdir -p arv-source/src/arv/vendor
echo "arv@reference" > arv-source/VERSION
printf 'arv (reference stand-in for tools/)\n' > arv-source/README.md
printf '# bagit.py stand-in\n' > arv-source/src/arv/vendor/bagit.py
printf 'arv.com stand-in\n' > arv.com         # tools/arv.com: the Actually Portable Executable

# ------------------------------------------------------------------ make: two discs into one home
mkdir -p src/docs src/bin second/letters
echo guide > src/docs/guide.md
printf '#!/bin/sh\necho hi\n' > src/bin/tool.sh
chmod 755 src/bin/tool.sh
ln -s docs/guide.md src/README.md
ln -s docs src/latest
ln -s missing.txt src/dead
echo "dear diary" > second/letters/2001-05-01.txt
printf '%%PDF-1.4\n%%EOF\n' > "second/letters/scan,page1.pdf"
for f in src/docs/guide.md src/bin/tool.sh src/README.md src/latest src/dead; do stamp 1577880000 "$f"; done
stamp 988718400 second/letters/2001-05-01.txt
stamp 988804800 "second/letters/scan,page1.pdf"
for folder in src second; do
    log make.out --home h make -y --no-ecc --formats no --set CODE --location BOX1 --importance "essential for self" \
        --keep-stage --output-dir "out-$folder" "$folder"
    keep_stages "out-$folder" "make/$folder"
done
keep make.out make/transcript.txt
keep_home h make/home

# ------------------------------------------------------------------ --links copy and record, --label, --access, --snapshot
log links.out --home hl make -y --no-ecc --formats no --set CODE --links copy --label "" --access public \
    --snapshot disc --keep-stage --output-dir out-links src
log links.out --home hl make -y --no-ecc --formats no --set CODE --links record --title "Recorded links" \
    --access sealed --keep-stage --output-dir out-links2 src
keep links.out links/transcript.txt
keep_stages out-links links/copy
keep_stages out-links2 links/record
keep_home hl links/home

# ------------------------------------------------------------------ --tools-history, --extra-tools, --ro-crate
mkdir -p hist-src hist-extra/bin "rc-src/sub dir"
echo a > hist-src/a.txt
echo x > hist-extra/bin/tool
stamp 1600000000 hist-src/a.txt
log extras.out --home hx make -y --no-ecc --formats no --set CODE --tools-history --extra-tools hist-extra \
    --keep-stage --output-dir out-hist hist-src
echo a > rc-src/a.txt
printf '%%PDF-1.4\n%%EOF\n' > "rc-src/sub dir/scan #1.pdf"
echo '"q"' > "rc-src/sub dir/na$(printf '\303\257')ve.md"
for f in rc-src/a.txt "rc-src/sub dir/scan #1.pdf" "rc-src/sub dir/na$(printf '\303\257')ve.md"; do stamp 1600000000 "$f"; done
log extras.out --home hr make -y --no-ecc --formats no --set CODE --ro-crate --title 'T"x' \
    --description 'Line, with "quotes"' --subject one --subject two --creator Sam \
    --rights https://creativecommons.org/licenses/by/4.0/ --keep-stage --output-dir out-rc rc-src
keep extras.out extras/transcript.txt
keep_stages out-hist extras/history
keep_stages out-rc extras/ro-crate
keep_home hr extras/ro-crate-home

# ------------------------------------------------------------------ an interactive make (a terminal, no -y)
mkdir -p Projects_x/fw
echo 'int main(void){return 0;}' > Projects_x/fw/main.c
stamp 1700000000 Projects_x/fw/main.c
printf 'project\nelec, code ,\nMy title\n\nSam\n\na, b\nhello\n' > answers1
for i in 1 2 3 4 5 6 7 8; do printf '^D\n'; done > answers2
questions='(Set code|Extra categories|Title|Description|Creator|Physical location|Subjects|Note)[^:]*: '
for k in 1 2; do
    # shellcheck disable=SC2086
    "$ptyrun" answers$k "$ARV0" $ARVARGS --home hi$k make --no-ecc --formats no --output-dir out-i$k Projects_x \
        > interactive$k.raw 2>&1 || true
    grep -oE "$questions" interactive$k.raw > interactive$k.txt || true
    keep interactive$k.txt interactive/questions$k.txt
    keep_home hi$k interactive/home$k
done

# ------------------------------------------------------------------ --split, then rebuild from the discs
mkdir -p big/letters
i=0
while [ $i -lt 70 ]; do
    f=big/letters/letter-$(printf %03d $i).txt
    yes "letter $i" | head -c 100000 > "$f"
    stamp $((1000000000 + i * 2592000)) "$f"
    i=$((i + 1))
done
log split.out --home hs make -y --no-ecc --formats no --set SCAN --split --medium-sectors 4800 --keep-stage \
    --output-dir out-split big
keep split.out split/transcript.txt
keep_stages out-split split/discs
keep_home hs split/home
first=""
for s in out-split/.archive-make-*/stage-*; do
    [ -d "$s" ] || continue
    id=$(sed -n 's/^Disc: //p' "$s/catalog.rec" | head -1)
    [ -n "$first" ] || first=$id
    log rebuild.out --home hb rebuild "$s"
done
log rebuild.out --home hb note "$first" "edited at home"
for s in out-split/.archive-make-*/stage-*; do
    [ -d "$s" ] || continue
    id=$(sed -n 's/^Disc: //p' "$s/catalog.rec" | head -1)
    [ "$id" = "$first" ] && log rebuild.out --home hb rebuild --prefer-disc "$s"
done
keep rebuild.out rebuild/transcript.txt
keep_home hb rebuild/home

# ------------------------------------------------------------------ recording, on a copy of the reference home
cp -r "$here/home" rec
log record.out --home rec burned TRIP-01_2019_4 --copies 2 --location box1 --media-id MEI-T02 --note "for the attic"
log record.out --home rec note PROJ-01_2020-2023_L "A long note that goes on about the weather station, its board revisions and the spare parts"
log record.out --home rec locate SCAN-01_1995-2008_D SAFE --add
log record.out --home rec locate FAMILY-01_2020-2021_K "Somewhere new"
log record.out --home rec burned NOPE-01_2000_X
keep record.out record/transcript.txt
keep rec/catalog/archive.rec record/archive.rec

# ------------------------------------------------------------------ editing (edit-cases.txt) and names
cp -r "$here/home" edit
grep -v '^#' "$here/edit-cases.txt" | while IFS= read -r line; do
    eval "set -- $line"
    log edit.out --home edit "$@"
done
mkdir -p names/Sub names/sub
(cd names && touch plain.txt "star*?.txt" "photo $(printf '\360\237\230\200').txt" "$(printf '%0120d' 0)" Sub/File.txt \
    sub/file.txt "semi;colon")
log edit.out names names
log edit.out names --limit 2 names
keep edit.out edit/transcript.txt
keep edit/catalog/archive.rec edit/archive.rec

# ------------------------------------------------------------------ init, named homes, pointers, --archive
mkdir -p init/a init/b init/c/sub
(
    cd init
    for args in "init a --name main" "init b --name other --default" "init a --name main" \
                "init c --pointer a/.arv" "init c/sub --pointer nowhere" \
                "init b/../c/sub --pointer ./c/../a/.arv" "--archive main where" "--archive nope where"; do
        # shellcheck disable=SC2086
        log ../init.out $args
    done
    (cd c/sub && log ../../../init.out where)
)
keep init.out init/transcript.txt
keep "$XDG_CONFIG_HOME/arv/homes.rec" init/homes.rec
keep init/c/.arv init/pointer
keep init/c/sub/.arv init/pointer-sub

# ------------------------------------------------------------------ tags and keywords
cp -r "$here/home" tags
rm -f tags/config/tags.rec
printf '.\tHoliday, Zzz, person:\303\205lice, place:kyoto\nsub dir\tkids,  travel ,\n' >> tags/catalog/volumes/TRIP-01_2019_4/tags.tsv
log tags.out --home tags tags
log tags.out --home tags tags --namespace place
log tags.out --home tags tags --namespace ""
log tags.out --home tags tags --namespace nope
log tags.out --home tags tags --vocab "$repo/src/arv/default_tags.rec"
for d in $(a --home tags list | cut -f1) NOPE-01_2000_X; do
    for f in tsv exiftool; do log tags.out --home tags keywords "$d" --format $f; done
done
keep tags.out tags/transcript.txt
keep tags/config/tags.rec tags/tags.rec

# ------------------------------------------------------------------ find, list, id, sets, where
cp -r "$here/home" q
for q in kyoto IMG '*.png' 'place:*' BOX 2019 nothing-matches; do log query.out --home q find "$q"; done
for o in "" "--in MEMORIES" "--at BOX1" "--access sealed" "--made 2026" "--covers 2019" "--covers 1995-06"; do
    # shellcheck disable=SC2086
    log query.out --home q list $o
done
for q in TRIP-01_2019_4 trip-01_2019_4 TRIP-01_2019_5 PROJ-01_2020-2O23_L 2020-2025_PROJECTS_01 "not an id"; do
    log query.out --home q id "$q"
done
log query.out --home q sets
log query.out --home q sets -v
log query.out --home q where
keep query.out query/transcript.txt

echo "scenarios: $(find "$out" -type f | wc -l) files in $out"
