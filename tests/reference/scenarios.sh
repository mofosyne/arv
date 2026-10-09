#!/bin/sh
# The reference scenarios: every command arv has, run in a fixed setting, their output and the
# files they write kept, normalised, in OUT. src/arv/check.sh runs them and compares OUT with
# expected/ (first written by the Python arv, which the C arv was ported from); generate.sh
# rewrites expected/ after a change made on purpose.
#
#   sh tests/reference/scenarios.sh OUT ARV-COMMAND...
#   e.g. sh tests/reference/scenarios.sh /tmp/ref src/arv/build/arv
#
# Fixed: the clock (SOURCE_DATE_EPOCH), the time zone, the user, the machine config, the file
# dates of every input, and tools/ (ARV_SOURCE: a small stand-in tree, and ARV_APE: a stand-in
# arv.com, so discs do not change with every commit). Not covered here: RS03 (dvdisaster), Siegfried and git checkouts as
# tools/, whose output depends on the machine; check.sh tests those of arv directly.
# Needs src/arv/build/ptyrun (make -C src/arv).
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
ptyrun=$repo/src/arv/build/ptyrun
[ -x "$ptyrun" ] || { echo "scenarios.sh: build src/arv first (make -C src/arv)" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
root=$work/r
mkdir -p "$root"
cd "$root"
export TZ=UTC LC_ALL=C USER=archivist LOGNAME=archivist HOME="$root/userhome" XDG_CONFIG_HOME="$root/userhome/.config" \
    XDG_DATA_HOME="$root/userhome/.local/share" SOURCE_DATE_EPOCH=1767225600 ARV_SOURCE="$root/arv-source" \
    ARV_APE="$root/arv.com"
unset ARV_HOME
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

# the normalised copy of a text file: this run's folder, UUIDs, the software name (arv@ or arv@),
# temporary names, the time stamps of files written while making (the RO-Crate files), and image
# hashes (an image holds its archive's UUID)
norm() {
    sed -E -e "s#$root#ROOT#g" -e "s#$repo#REPO#g" \
        -e 's/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/UUID/g' \
        -e 's/arv?@reference/SW/g' -e 's/^(ImageSha256: ).*/\1SHA256/' \
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
mkdir -p arv-source/src/arv
echo "arv@reference" > arv-source/VERSION
printf 'arv (reference stand-in for tools/)\n' > arv-source/README.md
printf '/* arv.c stand-in */\n' > arv-source/src/arv/arv.c
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
cp -r h hother                        # a home of another archive: its discs are not merged by accident
for s in out-split/.archive-make-*/stage-*; do
    log rebuild.out --home hother rebuild "$s"
    log rebuild.out --home hother rebuild --any-archive "$s"
    break
done
log rebuild.out --home hb where
keep rebuild.out rebuild/transcript.txt
keep_home hb rebuild/home

# ------------------------------------------------------------------ --draft: what arv describe and arv tag save
cat > draft-model.json <<'JSON'
{"title": "Letters and scans", "description": "Old letters.", "subjects": ["letters", "family"],
 "notes": ["Drafted by a model."], "agent": "llm:qwen2.5 (local)", "authorship": "suggested",
 "folder_tags": {".": ["Holiday", "kids", "place: Kyoto"], "letters": ["letters", "relatives"], "elsewhere": ["x"]},
 "folder_captions": {"letters": "two pages,\tone scan\nand a stamp", "missing": "not on this disc"}}
JSON
cat > draft-human.json <<'JSON'
{"title": "Hand-written été 😀", "subjects": [], "agent": "sample script (hand-written)",
 "folder_tags": {"letters": ["letters"]}}
JSON
printf '{"description": "No agent, no title"}\n' > draft-bare.json
log draft.out --home hd make -y --no-ecc --formats no --set CODE --draft draft-model.json --title "My own title" \
    --note "from the command line" --keep-stage --output-dir out-d1 second
log draft.out --home hd make -y --no-ecc --formats no --set CODE --draft draft-human.json --keep-stage --output-dir out-d2 src
log draft.out --home hd make -y --no-ecc --formats no --set CODE --draft draft-bare.json --title T --keep-stage \
    --output-dir out-d3 hist-src
printf '{"agent": "embeddings:bge-small", "folder_tags": {"letters": ["letters"], ".": ["scan"]}}\n' > draft-split.json
log draft.out --home hd make -y --no-ecc --formats no --set SCAN --split --medium-sectors 4800 --draft draft-split.json \
    --keep-stage --output-dir out-d4 big
printf 'not json' > draft-broken.json
log draft.out --home hd make -y --no-ecc --formats no --set CODE --draft draft-broken.json --output-dir out-d5 src
keep draft.out draft/transcript.txt
for k in 1 2 3 4; do keep_stages out-d$k draft/d$k; done
keep_home hd draft/home

# ------------------------------------------------------------------ collections: a workflow folder and its editions
mkdir -p coll/fam/2019
echo a > coll/fam/2019/a.txt
echo b > coll/fam/b.txt
stamp 1560000000 coll/fam/2019/a.txt
stamp 1560000000 coll/fam/b.txt
log coll.out --home hc collection init coll/fam --code FAM --title "Family photos" --set CODE --access public
log coll.out --home hc collection init coll/fam --code OTHER --title "Again"
log coll.out --home hc collection init coll --code fam --title "Code taken"
log coll.out --home hc collection init coll --code F --title "Code too short"
log coll.out --home hc make -y --no-ecc --formats no --keep-stage --output-dir out-c1 coll/fam
echo B > coll/fam/b.txt
echo c > coll/fam/c.txt
rm coll/fam/2019/a.txt
stamp 1570000000 coll/fam/b.txt
stamp 1570000000 coll/fam/c.txt
log coll.out --home hc make -y --no-ecc --formats no --message "the 2019 sort" --keep-stage --output-dir out-c2 coll/fam
log coll.out --home hc make -y --no-ecc --formats no --keep --set CODE --output-dir out-c3 src
log coll.out --home hc status coll/fam
echo e > coll/fam/e.txt
mv coll/fam/c.txt coll/fam/c2.txt
echo X > coll/fam/b.txt
stamp 1580000000 coll/fam/e.txt
stamp 1580000000 coll/fam/b.txt
log coll.out --home hc status coll/fam
log coll.out --home hc status coll/fam
log coll.out --home hc checkpoint --message "renamed c" coll/fam
log coll.out --home hc checkpoint coll/fam
log coll.out --home hc diff FAM/1 FAM/2
log coll.out --home hc diff FAM/1
log coll.out --home hc diff 0000000
log coll.out --home hc log FAM
mkdir -p coll/loose coll/plain
cp coll/fam/e.txt coll/loose/e.txt
echo n > coll/loose/n.txt
cp -p coll/fam/c2.txt coll/plain/c.txt
log coll.out --home hc status coll/loose
log coll.out --home hc link coll/loose FAM --past
log coll.out --home hc status coll/loose
log coll.out --home hc link coll/plain FAM
log coll.out --home hc status coll/plain
log coll.out --home hc link coll/plain NOPE
log coll.out --home hc link coll/loose FAM-01_2019_0
log coll.out --home hc status coll/loose
log coll.out --home hc collection list
log coll.out --home hc collection show FAM
log coll.out --home hc collection keep FAM 9
log coll.out --home hc todo
log coll.out --home hc retire FAM
cp out-c2/FAM-02_2019_Y.noecc.iso bad.iso
printf X | dd of=bad.iso bs=1 seek=40000 conv=notrunc 2>/dev/null
log coll.out --home hc burned FAM-02_2019_Y --device bad.iso --location BOX1
log coll.out --home hc burned --device out-c2/FAM-02_2019_Y.noecc.iso --location BOX1
log coll.out --home hc burned FAM-01_2019_0 --location BOX1
log coll.out --home hc todo
log coll.out --home hc retire FAM
log coll.out --home hc retire FAM --yes
log coll.out --home hc retire FAM --yes --accept-loss
log coll.out --home hc retire FAM
log coll.out --home hc log FAM
log coll.out --home hc find a.txt
log coll.out --home hc collection keep FAM 2
log coll.out --home hc collection keep FAM 2
log coll.out --home hc location add NAS "The NAS" --temperature hot
log coll.out --home hc location add DRAWER "Desk drawer" --temperature tepid
log coll.out --home hc location move NAS --temperature warm
mkdir -p nas
cp out-c2/FAM-02_2019_Y.noecc.iso nas/
log coll.out --home hc stored FAM-02_2019_Y nas/FAM-02_2019_Y.noecc.iso --location NAS
log coll.out --home hc stored FAM-02_2019_Y bad.iso
7z x -onas/FAM-02_2019_Y out-c2/FAM-02_2019_Y.noecc.iso >/dev/null
log coll.out --home hc stored FAM-01_2019_0 nas/FAM-02_2019_Y
echo tampered >> nas/FAM-02_2019_Y/data/c.txt
log coll.out --home hc stored FAM-02_2019_Y nas/FAM-02_2019_Y --location NAS
7z x -onas/FAM-02-files out-c2/FAM-02_2019_Y.noecc.iso >/dev/null
log coll.out --home hc stored FAM-02_2019_Y nas/FAM-02-files --location NAS --note "unpacked, for quick restores"
log coll.out --home hc location list
log coll.out --home hc todo
log coll.out --home hc todo
log coll.out --home hc appraise FAM-02_2019_Y:c.txt --importance "essential for family" --importance "useful for self"
log coll.out --home hc todo
log coll.out --home hc appraise FAM-02_2019_Y:c.txt --importance "important for family"
log coll.out --home hc todo
log coll.out --home hc collection show NOPE
mkdir -p lay/Photos lay/Records lay/Taxes lay/misc "lay/New folder (2)" lay/Grandma lay/2019 lay/.hidden
log coll.out --home hc status lay
log coll.out --home hc list
log coll.out --home hc objects
log coll.out --home hc objects FAM
keep coll.out coll/transcript.txt
keep coll/fam/.arv coll/marker
for k in 1 2; do keep_stages out-c$k coll/c$k; done
keep_home hc coll/home
keep hd/config/tags.rec draft/tags.rec

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
log tags.out --home tags tags --vocab "$repo/src/arv/data/default_tags.rec"
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
log query.out stauts                         # an unknown command: the nearest, not the whole usage
log query.out --home q todo --bogus          # arguments a command does not take: named, with its usage
log query.out --home q burned --help         # a command's own lines of the usage
log query.out --home q-empty todo            # nothing made yet: not "nothing owed"
(XDG_CONFIG_HOME="$root/no-config" && export XDG_CONFIG_HOME && cd "$work" && log "$root/query.out" todo)   # no archive: arv init, as git
keep query.out query/transcript.txt

# ------------------------------------------------------------------ disc plans: discs composed by hand
mkdir -p plan-pc/Videos plan-nas/photos/2025
echo "a film" > plan-pc/Videos/wedding.mkv
echo "jpeg a" > plan-nas/photos/2025/a.jpg
echo "jpeg b" > plan-nas/photos/2025/b.jpg
echo "notes" > plan-pc/notes.txt
echo "soon gone" > plan-pc/gone.txt
for f in plan-pc/Videos/wedding.mkv plan-nas/photos/2025/a.jpg plan-nas/photos/2025/b.jpg plan-pc/notes.txt plan-pc/gone.txt; do
    stamp 1751328000 "$f"
done
log plan.out --home hp plan list
log plan.out --home hp plan new trip --title "Holiday 2025" --set TRIP
log plan.out --home hp plan new trip
log plan.out --home hp plan new "bad name"
log plan.out --home hp plan add trip plan-pc/Videos/wedding.mkv --as video/wedding.mkv
log plan.out --home hp plan add trip plan-nas/photos/2025 --as photos --disc new
log plan.out --home hp plan add trip plan-pc/notes.txt
log plan.out --home hp plan add trip plan-pc/notes.txt --disc 2
log plan.out --home hp plan add trip plan-pc/Videos --as ../up
log plan.out --home hp plan add trip plan-pc/gone.txt --as video/wedding.mkv --disc 1
log plan.out --home hp plan disc trip add
log plan.out --home hp plan disc trip drop 1
log plan.out --home hp plan disc trip drop 3
log plan.out --home hp plan move trip notes.txt --disc 2
log plan.out --home hp plan show trip
log plan.out --home hp plan show trip --json
log plan.out --home hp plan list
log plan.out --home hp plan make trip -y --no-ecc --formats no --split
log plan.out --home hp plan make trip -y --no-ecc --formats no --keep-stage --output-dir out-plan
log plan.out --home hp plan show trip
log plan.out --home hp plan add trip plan-pc/gone.txt
log plan.out --home hp plan show trip --medium bd100
log plan.out --home hp plan list
log plan.out --home hp plan list --all
log plan.out --home hp plan again trip trip-2026 --title "Holiday 2026"
log plan.out --home hp plan again trip trip-2026
log plan.out --home hp plan show trip-2026
log plan.out --home hp find wedding
log plan.out --home hp status plan-nas/photos/2025
log plan.out --home hp status plan-pc
echo "jpeg c" > plan-nas/photos/2025/c.jpg
stamp 1751328000 plan-nas/photos/2025/c.jpg
log plan.out --home hp status -v plan-nas/photos/2025
log plan.out --home hp plan new again --set TRIP
log plan.out --home hp plan add again plan-nas/photos/2025 --as photos
cp -Rp plan-nas/photos/2025 plan-copy
log plan.out --home hp plan add again plan-copy --disc new
log plan.out --home hp plan make again -y --no-ecc --formats no --output-dir out-plan2
log plan.out --home hp status plan-nas/photos/2025
mv plan-copy plan-moved
log plan.out --home hp status plan-moved
log plan.out --home hp find photos
log plan.out --home hp objects
log plan.out --home hp burned TRIP-03_2025_7 --location BOX1
log plan.out --home hp stored TRIP-04_2025_5 out-plan2/TRIP-04_2025_5.noecc.iso
log plan.out --home hp stored TRIP-01_2025_B out-plan/TRIP-01_2025_B.noecc.iso
mv plan-pc/Videos/wedding.mkv plan-pc/wedding-moved.mkv
log plan.out --home hp objects 2025
log plan.out --home hp objects wedding.mkv
log plan.out --home hp objects nothing-called-this
log plan.out --home hp todo
log plan.out --home hp objects --json
log plan.out --home hp plan new later --medium bd100
log plan.out --home hp plan add later plan-pc/gone.txt
rm plan-pc/gone.txt
log plan.out --home hp plan show later
log plan.out --home hp plan make later -y --no-ecc --formats no
log plan.out --home hp plan drop later gone.txt
log plan.out --home hp plan delete later
log plan.out --home hp plan list
# a card that will not be here at make: copied into the plan; a linked item that changes after planning
mkdir -p plan-sd/DCIM plan-docs
echo "raw image" > plan-sd/DCIM/IMG_0001.JPG
echo "letter" > plan-docs/letter.txt
stamp 1751328000 plan-sd/DCIM/IMG_0001.JPG
stamp 1751328000 plan-sd/DCIM
stamp 1751328000 plan-docs/letter.txt
log plan.out --home hp plan new card --set TRIP
log plan.out --home hp plan add card plan-sd/DCIM --copy --as camera
log plan.out --home hp plan add card plan-docs/letter.txt
rm -r plan-sd
echo "letter, signed" > plan-docs/letter.txt
stamp 1751414400 plan-docs/letter.txt
log plan.out --home hp plan show card
log plan.out --home hp plan add card plan-sd/DCIM
log plan.out --home hp plan refresh card
log plan.out --home hp plan show card
log plan.out --home hp plan delete card
log plan.out --home hp plan make card -y --no-ecc --formats no --output-dir out-card
log plan.out --home hp objects DCIM
log plan.out --home hp plan delete card
log plan.out --home hp plan again card card-2
log plan.out --home hp plan show card-2
log plan.out --home hp plan delete card --yes
keep plan.out plan/transcript.txt
keep hp/drafts/plans/trip.rec plan/trip.rec
keep hp/drafts/plans/card-2.rec plan/card-2.rec
keep_stages out-plan plan/discs
keep_home hp plan/home

echo "scenarios: $(find "$out" -type f | wc -l) files in $out"
