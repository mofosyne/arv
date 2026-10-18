#!/bin/sh
# Checks arv, without Python:
# - the reference scenarios (tests/reference/): every command run in a fixed setting must print
#   and write what expected/ holds (first written by the Python arv, since retired), byte for byte (normalised);
# - what only a real disc shows: a disc made by arv from a folder like a git clone verifies,
#   restores to a clean `git status` (links and execute bits included), and damage is found;
#   RS03 is added, tested (and damage found) and repaired by arv itself; with dvdisaster Light, it
#   accepts and repairs arv's images; with Siegfried, formats are identified; with arv's
#   git checkout as the source, tools/ gets the commit and its history; git repositories in a
#   folder go on as a compacted .git and are recognised by their history;
# - SHA-256 and SHA-512 against sha256sum and sha512sum, and the recfile writer.
# - that describe and tag go to arv-assist, and gui to arv-gui.
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
export ARV_NO_EXTRA_PLACES=1      # the built-in places only, whatever this machine has installed (one test below adds some)
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
echo "    links, an interactive make, recording, editing, names, init, tags, queries)"
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
rc=0
"$tool" make --location BOX1 src >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 2 ] || no "make still accepts --location"
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
"$tool" make -C home --no-ecc --no-formats --set CODE --output disc.iso src >/dev/null 2>&1 || no "arv make"
7z x -odisc disc.iso >/dev/null || no "7z cannot read the image"
"$tool" check -C home disc >/dev/null && ok "check FOLDER: every file matches the manifests" || no "check FOLDER"
"$tool" info --files disc | grep -q "  docs/guide.md$" && ok "info --files: the disc's listing" || no "info --files"
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
# without a UDF reader (README.txt): every payload file cut out of the image with dd alone, from the
# map the home catalogue keeps (extents.tsv), matches the manifest
ext=$(ls home/catalog/volumes/*/extents.tsv | head -1)
man=$(dirname "$ext")/manifest.sha256
carved=0 wrong=0
while IFS="$(printf '\t')" read -r start size name; do
    case "$name" in data/*) ;; *) continue ;; esac
    got=$(dd if=disc.iso bs=2048 skip="$start" count=$(( (size + 2047) / 2048 )) 2>/dev/null | head -c "$size" \
          | sha256sum | cut -d' ' -f1)
    grep -qxF "$got  $name" "$man" || wrong=$((wrong + 1))
    carved=$((carved + 1))
done < "$ext"
[ "$carved" -gt 0 ] && [ "$wrong" -eq 0 ] \
    && ok "without a UDF reader: $carved files cut out of the image with dd (extents.tsv) match the manifest" \
    || no "dd from extents.tsv: $wrong of $carved files differ from the manifest"
cp -r disc bad
printf X | dd of=bad/data/docs/guide.md bs=1 seek=0 conv=notrunc 2>/dev/null
if "$tool" check -C home bad >out.txt; then no "check missed damage"; fi
grep -q "FAILED   data/docs/guide.md" out.txt && ok "check FOLDER finds a damaged file" || no "damage report"
if "$tool" restore bad out-bad >out.txt; then no "restore missed damage"; fi
[ -f out-bad/README.md ] && [ ! -L out-bad/README.md ] \
    && ok "restore keeps a link's copy when its target is damaged" || no "copy of a damaged target"
[ -f disc/tools/arv/src/arv/arv.c ] && grep -q "Software: arv@" disc/catalog.rec \
    && ok "tools/ carries arv's last commit, and the disc names it" || no "tools/ from the git checkout"
if [ -f "$here/build/arv.com" ]; then
    [ -f disc/tools/arv.com ] && grep -q "tools/arv.com" disc/README.txt || no "tools/arv.com missing, or README.txt silent on it"
    cp disc/tools/arv.com ape.com && chmod 755 ape.com     # copied off the disc, as README.txt says (7z drops modes)
    (cd disc && ../ape.com check . >/dev/null) && ok "tools/arv.com (Actually Portable Executable), copied off the disc, checks it" \
        || no "tools/arv.com from the disc"
fi

"$tool" --home fresh-home location add HOME "Home" >/dev/null && [ -s fresh-home/catalog/archive.rec ] \
    && ok "a catalogue command on a home that does not exist yet creates it" || no "location add on a new home"
"$tool" --home fresh-home todo --json | grep -qx '{"format": "arv-json 1", "overdueBefore": "[0-9-]*", "discs": 0, "owed": \[\], "questions": \[\]}' \
    || no "todo --json with nothing owed"
"$tool" help --json | grep -q '{"name": "todo", "usage": "arv todo \[--overdue AGE\] \[--tsv | --json\]", "summary": "what is owed' \
    || no "help --json"
mkdir -p loose && echo x > loose/f.txt
"$tool" --home fresh-home status --json loose 2>/dev/null \
    | grep -q '"is": "folder", "files": 1, "onDiscs": 0, "mostLike": \[\], "onNone": \["f.txt"\], "notes": \[\]}$' || no "status --json"
"$tool" --home fresh-home info PHOTO-01_2001_G --json \
    | grep -qx '{"format": "arv-json 1", "is": "id", "id": "PHOTO-01_2001_G", "scheme": "set-seq-coverage/1", "set": "PHOTO", "sequence": 1, "coverage": "2001", "check": "G", "checkCorrect": false, "disc": null, "didYouMean": \[\]}' \
    && ok "--json on todo, help, status and info: each the same as the text, for a script" || no "info --json"
"$tool" --home fresh-home location list --json | grep -q '^{"format": "arv-json 1", "locations": \[{"code": "HOME", "name": "Home", "parent": ""' \
    || no "location list --json"
"$tool" --home fresh-home location add BOX "A box" --in HOME --json 2> json.err \
    | grep -q '^{"format": "arv-json 1", "command": "location", "events": \[{"EventId": "[0-9a-f]\{32\}", "Object": "location:BOX", ' \
    || no "--json on a command that changes the record: the events it made"
grep -q "^Code: BOX" fresh-home/catalog/archive.rec || no "location add --json records the place"
rc=0; out=$("$tool" --home fresh-home note NOPE-01 x --json 2> json.err) || rc=$?
[ "$rc" = 1 ] && [ "$out" = '{"format": "arv-json 1", "command": "note", "events": []}' ] && grep -q "^Error: no disc NOPE-01" json.err \
    && ok "--json on a command that changes the record: the events it made on standard output, the rest on standard error, none after a failure" \
    || no "--json after a failure"

"$tool" init init-missing/nested/archive >/dev/null && mkdir -p init-missing/nested/archive/source \
    && echo init > init-missing/nested/archive/source/file.txt \
    && [ -s init-missing/nested/archive/.arv/catalog/archive.rec ] \
    && (cd init-missing/nested/archive && "$tool" make -y --no-ecc --no-formats --set MISSING --output missing.iso source >/dev/null 2>&1) \
    && grep -q '^Id: MISSING-' init-missing/nested/archive/.arv/catalog/archive.rec \
    && ok "init creates a missing folder, and make finds that new home from its source" \
    || no "init on a missing folder or make home discovery"


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
"$tool" make -C g/home -y --no-ecc --no-formats --set CODE --output g/full.iso g/src >g/make.txt 2>&1 || { cat g/make.txt; no "arv make (git)"; }
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
    && grep -q "git repository proj: 1 branch, 0 tags, 5 commits, full history" g/home/catalog/archive.rec \
    && ok "git: git.tsv lists its roots, heads and commits; the ingestion event says what was done" || no "git: git.tsv or event"

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
# ------------------------------------------------------------------ data objects describe themselves
mkdir -p ab/pc/tool ab/pc/bare
(
    cd ab/pc/tool && git init -q -b main
    printf '[![ci](https://ci.invalid/badge.svg)](https://ci.invalid)\n\n# Tool\n\n> [!NOTE]\n> Old.\n\nTool turns **sensor logs**\ninto [plots](https://x.invalid). It is small.\n\n## Use\n' > README.md
    printf 'MIT License\n\nPermission is hereby granted, free of charge, to any person\n' > LICENSE
    git add -A && GIT_AUTHOR_DATE=2011-02-03T00:00:00Z GIT_COMMITTER_DATE=2011-02-03T00:00:00Z gitenv git commit -qm one
    echo 2 > two && git add two && GIT_AUTHOR_DATE=2014-05-06T00:00:00Z GIT_COMMITTER_DATE=2014-05-06T00:00:00Z gitenv git commit -qm two
    git remote add origin https://me:secret@example.invalid/tools/tool.git
    echo wip > two && gitenv git stash -q
)
echo x > ab/pc/bare/x.dat
mkdir -p ab/pc/nodoc && (cd ab/pc/nodoc && git init -q -b main && echo 1 > main.c && git add main.c && gitenv git commit -qm one)
{ "$tool" -C ab/home plan new kit --set KIT && "$tool" -C ab/home plan add kit ab/pc/tool && "$tool" -C ab/home plan add kit ab/pc/bare \
    && "$tool" -C ab/home plan add kit ab/pc/nodoc; } >/dev/null 2>&1 || no "a plan of three objects"
"$tool" -C ab/home plan make kit -y --no-ecc --no-formats --output-dir ab/out >ab/make.txt 2>&1 || { cat ab/make.txt; no "plan make (objects)"; }
"$tool" objects -C ab/home tool > ab/objects.txt
grep -qx '    "Tool": Tool turns sensor logs into plots. It is small.' ab/objects.txt \
    && grep -qx '    2011-02-03/2014-05-06; MIT; example.invalid/tools/tool' ab/objects.txt \
    && grep -q 'about it: "Tool": Tool turns' ab/make.txt \
    && ok "objects: a repository describes itself: its README's title and first paragraph, its licence, its commit dates (not the stash's) and origin (no credentials)" \
    || { cat ab/objects.txt; no "objects: described by its own files"; }
7z x -oab/disc ab/out/*.iso catalog.rec >/dev/null
grep -qx 'Title: Tool' ab/disc/catalog.rec && grep -qx 'Origin: example.invalid/tools/tool' ab/disc/catalog.rec \
    && [ "$(grep -c '^Title: ' ab/disc/catalog.rec)" -eq 2 ] \
    && "$tool" find -C ab/home 'sensor logs' | grep -q '^OBJECT  tool/' \
    && "$tool" objects -C ab/home --json | grep -q '"name": "bare", "readAs": "bare", "kind": "folder", "tags": "", "title": "",' \
    && ok "objects: the disc carries what they say (a folder with no README says nothing); arv find searches it" \
    || no "objects: on the disc, find or --json"
"$tool" todo -C ab/home > ab/todo.txt
grep -q "1 question kept for later" ab/make.txt && grep -q "^Questions waiting for an answer" ab/todo.txt \
    && grep -qx "  nodoc/  1 question (data object)" ab/todo.txt && [ "$(tail -1 ab/todo.txt)" = "  nodoc/  1 question (data object)" ] \
    && grep -qx "Text: What is nodoc for? It has no README to say." ab/home/drafts/questions.rec \
    && ! grep -q "Question\|nodoc for" ab/disc/catalog.rec \
    && "$tool" todo -C ab/home --json | grep -q '"questions": \[{"about": "object:[0-9a-f-]*", "name": "nodoc/", "open": 1}\]}$' \
    && ok "questions: a repository that says nothing about itself gets one, from arv; arv todo lists it last (and in --json apart from what is owed); never on a disc" \
    || { cat ab/make.txt ab/todo.txt; no "questions: kept, and listed by todo"; }
# ------------------------------------------------------------------ packed objects: one file on a disc, the same object
mkdir -p pk/pc/docs/sub pk/pc/notes
for i in 1 2 3; do seq 1 3000 > pk/pc/docs/f$i.txt; done
seq 1 40 > pk/pc/docs/sub/a.txt
echo hi > pk/pc/notes/n.txt
(cd pk/pc && tar cf - notes | gzip -n > made.tar.gz && cp made.tar.gz fake.zip && cp -r docs docs2)
{ "$tool" -C pk/home plan new p --set PK && "$tool" -C pk/home plan add p pk/pc/docs pk/pc/made.tar.gz pk/pc/fake.zip \
    && "$tool" -C pk/home plan pack p docs tar+gzip && "$tool" -C pk/home plan disc p add \
    && "$tool" -C pk/home plan add p pk/pc/notes pk/pc/docs2 --disc 2 \
    && "$tool" -C pk/home plan pack p --disc 2 tar+gzip --together; } >pk/plan.txt 2>&1 || { cat pk/plan.txt; no "plan pack"; }
"$tool" -C pk/home plan pack p docs xz 2>&1 | grep -q "a folder needs tar" || no "plan pack: a compressor alone for a folder"
"$tool" -C pk/home plan show p --json | grep -q '"packing": "tar+gzip", "pack": "docs.tar.gz", "ownPacking": "tar+gzip"' \
    || no "plan show --json: packing"
"$tool" -C pk/home plan again p p2 >/dev/null
"$tool" -C pk/home plan make p -y --no-ecc --no-formats --output-dir pk/out >pk/make.txt 2>&1 || { cat pk/make.txt; no "plan make (packed)"; }
7z x -opk/d1 pk/out/PK-01_*.iso data catalog.rec >/dev/null && 7z x -opk/d2 pk/out/PK-02_*.iso data catalog.rec >/dev/null
[ "$(cd pk/d1/data && ls)" = "$(printf 'docs.tar.gz\nfake.zip\nmade.tar.gz')" ] && [ "$(ls pk/d2/data)" = pack.tar.gz ] \
    && [ "$(tar tzf pk/d1/data/docs.tar.gz)" = "$(printf 'docs/f1.txt\ndocs/f2.txt\ndocs/f3.txt\ndocs/sub/a.txt')" ] \
    && [ "$(tar tzf pk/d2/data/pack.tar.gz)" = "$(printf 'docs2/f1.txt\ndocs2/f2.txt\ndocs2/f3.txt\ndocs2/sub/a.txt\nnotes/n.txt')" ] \
    && (mkdir pk/un && cd pk/un && tar xzf ../d1/data/docs.tar.gz && diff -r docs ../pc/docs) \
    && ok "packed objects: a folder as its own data/docs.tar.gz, a disc's objects together in data/pack.tar.gz; each unpacks to its path" \
    || no "packed objects on the disc"
grep -A12 '^Name: docs$' pk/d1/catalog.rec | grep -qx 'Pack: docs.tar.gz' \
    && grep -A12 '^Name: docs$' pk/d1/catalog.rec | grep -qx 'Packing: tar+gzip' \
    && grep -A12 '^Name: docs$' pk/d1/catalog.rec | grep -qx 'Files: 4' \
    && grep -A12 '^Name: made.tar.gz$' pk/d1/catalog.rec | grep -qx 'Packing: tar+gzip' \
    && ! grep -A12 '^Name: made.tar.gz$' pk/d1/catalog.rec | grep -q '^Pack:' \
    && ! grep -A12 '^Name: fake.zip$' pk/d1/catalog.rec | grep -q '^Packing:' \
    && grep -A12 '^Name: notes$' pk/d2/catalog.rec | grep -qx 'Pack: pack.tar.gz' \
    && ok "packed objects: the record says how (Packing) and where (Pack), with the object's own files; a .tar.gz given as it is is found by its name and bytes, a .zip that is not one is not" \
    || no "packed objects in the record"
"$tool" -C pk/home status pk/pc/docs 2>/dev/null | grep -q "4 of its 4 files are on discs" \
    && "$tool" -C pk/home objects docs | grep -q "data/docs.tar.gz (tar+gzip, unpacked: data/docs/)" \
    && ok "packed objects: arv status finds a folder's files inside its pack; arv objects names the pack" \
    || no "packed objects: status or objects"
"$tool" -C pk/home plan pack p2 docs none >/dev/null && "$tool" -C pk/home plan pack p2 --disc 2 tar+gzip --together >/dev/null
"$tool" -C pk/home plan make p2 -y --no-ecc --no-formats --output-dir pk/out2 >pk/make2.txt 2>&1 || { cat pk/make2.txt; no "plan make (again)"; }
7z x -opk/e2 pk/out2/PK-04_*.iso data >/dev/null
grep -q "^Object docs/ (data/docs on PK-03_.*): the same as a version already archived" pk/make2.txt \
    && cmp -s pk/d2/data/pack.tar.gz pk/e2/data/pack.tar.gz \
    && ok "packed objects: as its files on another disc, the same version; packed again, the very same bytes" \
    || { cat pk/make2.txt; no "packed objects: the same version, the same pack"; }
rm -rf pk/d1 && 7z x -opk/d1 pk/out/PK-01_*.iso >/dev/null
"$tool" -C pk/home restore pk/d1 pk/back >pk/restore.txt 2>&1 && grep -q "^UNPACKED data/docs.tar.gz: 4 files" pk/restore.txt \
    && diff -r pk/back/docs pk/pc/docs && cmp -s pk/back/made.tar.gz pk/pc/made.tar.gz && [ ! -e pk/back/docs.tar.gz ] \
    && [ "$(stat -c %Y pk/back/docs/f1.txt)" = "$(stat -c %Y pk/pc/docs/f1.txt)" ] \
    && grep -qx "$(printf 'sha256\tdocs/sub/a.txt\tdocs.tar.gz\ttar+gzip' | sed "s/^sha256/$(sha256sum pk/pc/docs/sub/a.txt | cut -c1-64)/")" \
        pk/d1/catalog/volumes/PK-01_*/packed.tsv \
    && ok "packed objects: arv restore unpacks each pack where it is, its files with their times; the disc lists every file in its packs (packed.tsv)" \
    || { cat pk/restore.txt; no "packed objects: restore"; }
"$tool" -C pk/home check pk/d1 >pk/check.txt 2>&1 && grep -q "inside its 1 pack, all 4 files match their checksums" pk/check.txt \
    && ok "packed objects: arv check of a disc's files reads inside its packs" || { cat pk/check.txt; no "packed objects: check"; }
printf 'x' | dd of=pk/d1/data/docs.tar.gz bs=1 seek=40 conv=notrunc 2>/dev/null
rc=0; "$tool" -C pk/home restore pk/d1 pk/back2 >pk/restore2.txt 2>&1 || rc=$?
[ "$rc" -eq 1 ] && grep -q "^DAMAGED  docs.tar.gz" pk/restore2.txt && [ -e pk/back2/docs.tar.gz ] \
    && ok "packed objects: a damaged pack is restored as it is, and said to be damaged" || { cat pk/restore2.txt; no "packed objects: damaged pack"; }
"$tool" -C pk/home plan again p p4 >/dev/null
"$tool" -C pk/home plan show p4 --packed --json 2>/dev/null | grep -q '"packs": \[{"pack": "pack.tar.gz", "bytes": [1-9]' \
    && "$tool" -C pk/home plan show p4 --packed 2>/dev/null | grep -q '^  data/pack.tar.gz  .* packed$' \
    && ok "packed objects: plan show --packed packs to measure (the disc's fill with them)" || no "plan show --packed"
if command -v zip >/dev/null && command -v unzip >/dev/null; then
    "$tool" -C pk/home plan again p p3 >/dev/null && "$tool" -C pk/home plan pack p3 docs zip >/dev/null \
        && "$tool" -C pk/home plan make p3 -y --no-ecc --no-formats --output-dir pk/out3 >pk/make3.txt 2>&1 \
        && 7z x -opk/z pk/out3/PK-05_*.iso data/docs.zip >/dev/null && unzip -Z1 pk/z/data/docs.zip | grep -qx 'docs/sub/a.txt' \
        && ok "packed objects: as a zip, by the zip program" || { cat pk/make3.txt; no "packed objects: zip"; }
fi

# ------------------------------------------------------------------ known names: entries of the vocabulary, changed by events
"$tool" init kn/home >/dev/null 2>&1
mkdir -p kn/one/a kn/two/a && echo 1 > kn/one/a/x.txt && echo 2 > kn/two/a/y.txt
printf '{"tags": {"a": ["person:anne-smith"]}, "authorship": "human", "agent": "human:t"}\n' > kn/d.json
for f in one two; do
    "$tool" -C kn/home make -y --no-ecc --no-formats --set KN --access public --draft kn/d.json --output-dir kn/out "kn/$f" >/dev/null 2>&1 \
        || no "make (known names)"
done
d1=$("$tool" -C kn/home list | awk 'NR == 1 { print $1 }'); d2=$("$tool" -C kn/home list | awk 'NR == 2 { print $1 }')
"$tool" -C kn/home tags add person:anne-smith --alias Anne > kn/add.out && "$tool" -C kn/home tags label person:anne-smith "Anne Jones" >/dev/null \
    && grep -A6 '^Name: person:anne-smith$' kn/home/config/tags.rec > kn/entry.txt \
    && grep -qx 'Label: Anne Jones' kn/entry.txt && grep -qx 'Alias: Anne Smith' kn/entry.txt && grep -qx 'Private: yes' kn/entry.txt \
    && [ "$(grep -c '^Object: name:person:anne-smith$' kn/home/catalog/archive.rec)" -ge 2 ] \
    && "$tool" -C kn/home tags | grep -q 'anne-smith .* Anne Jones, private' \
    && ok "known names: an entry of the vocabulary (a person's private); a new label keeps the old as an alias; each change an event about name:KEY" \
    || { cat kn/add.out kn/entry.txt; no "known names: add, label"; }
printf 'b. 1931\ncolleague\n2\n\n' | "$tool" -C kn/home tags split person:anne-smith > kn/split.out 2> kn/split.err \
    && grep -q "1 of 2 uses moved to person:anne-smith-2" kn/split.out \
    && "$tool" -C kn/home tags show person:anne-smith-2 | grep -q "^    $d1  data/a$" \
    && "$tool" -C kn/home tags show person:anne-smith | grep -q "^    $d2  data/a$" \
    && grep -A2 '^Name: person:anne-smith-2$' kn/home/config/tags.rec | grep -qx 'Qualifier: colleague' \
    && grep -q "^Tags: a	person:anne-smith-2$" kn/home/catalog/archive.rec \
    && ok "known names: split, two of one name: each use asked about, a qualifier each; a moved use is a tag change (an event)" \
    || { cat kn/split.out kn/split.err; no "known names: split"; }
"$tool" -C kn/home tags merge person:anne-smith-2 person:anne-smith > kn/merge.out \
    && [ "$("$tool" -C kn/home tags show person:anne-smith | grep -c 'data/a$')" -eq 2 ] \
    && grep -A8 '^Name: person:anne-smith-2$' kn/home/config/tags.rec | grep -qx 'Retired: merged into person:anne-smith' \
    && grep -A12 '^Name: person:anne-smith$' kn/home/config/tags.rec | grep -qx 'Alias: person:anne-smith-2' \
    && "$tool" -C kn/home tags retire person:anne-smith-2 --why "a mistake" >/dev/null \
    && ok "known names: merge moves the uses back, the other kept, retired, its key an alias; retire keeps the entry" \
    || { cat kn/merge.out; no "known names: merge"; }
"$tool" -C kn/home tags add person:ann-smith --label "Ann Smith" >/dev/null && "$tool" -C kn/home tags add person:a-smith --label "A. Smith" >/dev/null \
    && "$tool" -C kn/home tags add person:jo --label Jo >/dev/null && "$tool" -C kn/home tags add person:jon --label Jon >/dev/null \
    && "$tool" -C kn/home tags names > kn/names.out \
    && grep -q '^  person:ann-smith and person:a-smith: "ann smith" and "a smith": initials$' kn/names.out \
    && ! grep -q 'person:anne-smith and\|person:jo and' kn/names.out \
    && "$tool" -C kn/home tags names --json | grep -q '"likelySame": \[' \
    && ok "known names: likely the same offered (a letter apart, initials, a form in common); never a short name, nor one told apart by a qualifier" \
    || { cat kn/names.out; no "known names: likely the same"; }
"$tool" -C kn/home tags add software:kicad --related electronics >/dev/null 2>&1 && no "tags add takes an unknown option"
"$tool" -C kn/home tags add software:kicad --label KiCad --same wikidata:Q1141917 > /dev/null \
    && ! grep -A5 '^Name: software:kicad$' kn/home/config/tags.rec | grep -q '^Private' \
    && echo 3 > kn/one/a/z.txt && "$tool" -C kn/home make -y --no-ecc --no-formats --set KN --access public --output-dir kn/out2 kn/one >/dev/null 2>&1 \
    && 7z x -okn/disc kn/out2/*.iso catalog/config/tags.rec >/dev/null \
    && grep -q '^Name: software:kicad$' kn/disc/catalog/config/tags.rec && ! grep -q '^Name: person:anne-smith' kn/disc/catalog/config/tags.rec \
    && grep -q '^Name: family$' kn/disc/catalog/config/tags.rec \
    && ok "known names: a public disc's vocabulary leaves the private entries out (software:kicad goes, person:anne-smith stays home)" \
    || no "known names: private entries on a public disc"
# the programs arv can use: each found or not, from PATH; none needed for a disc
mkdir -p progbin && printf '#!/bin/sh\n' > progbin/zstd && chmod +x progbin/zstd
printf 'NAME="Debian GNU/Linux"\nID=debian\nPRETTY_NAME="Debian GNU/Linux 13 (trixie)"\n' > deb.os
printf 'ID=fedora\nPRETTY_NAME="Fedora Linux 42"\n' > fed.os
printf 'ID=plan9\n' > other.os
ARV_OS_RELEASE=$PWD/deb.os PATH="$PWD/progbin:/usr/bin/nonexistent" "$tool" programs > prog.out \
    && grep -q '^This system: Debian GNU/Linux 13 (trixie)\.$' prog.out \
    && grep -q '^  ok       zstd  ' prog.out && grep -q '^  missing  exiftool ' prog.out \
    && grep -q '^  to install: sudo apt install libimage-exiftool-perl poppler-utils ffmpeg curl$' prog.out \
    && grep -q '^  sf: Siegfried: ' prog.out && grep -q '^  Every missing one: sudo apt install ' prog.out \
    && ARV_OS_RELEASE=$PWD/fed.os PATH="$PWD/progbin" "$tool" programs | grep -q '^  to install: sudo dnf install perl-Image-ExifTool poppler-utils ffmpeg-free curl$' \
    && ARV_OS_RELEASE=$PWD/other.os PATH="$PWD/progbin" "$tool" programs > prog2.out \
    && grep -q '^  to install, on Arch: sudo pacman -S perl-image-exiftool poppler ffmpeg curl$' prog2.out \
    && grep -q '^  to install, on macOS (Homebrew): brew install exiftool poppler ffmpeg$' prog2.out \
    && ARV_OS_RELEASE=$PWD/deb.os PATH="$PWD/progbin" "$tool" programs --json | grep -q '"system": {"id": "debian", "name": "Debian GNU/Linux 13 (trixie)"}' \
    && PATH="$PWD/progbin" "$tool" programs --json | grep -q '"name": "zstd", "group": "making", "found": true' \
    && ok "arv programs: which programs arv can use are here (PATH), what each is for, and each group's install command for this system (every system's when it is not one arv knows)" \
    || { cat prog.out; no "arv programs"; }
# places: the gazetteer (Natural Earth's countries and cities), a place of your own in one, arv map, a disc's index.html
"$tool" -C kn/home tags show place:kyoto | grep -q '^  in: Japan (place:japan)$' \
    && "$tool" -C kn/home tags add place:fushimi-inari --label "Fushimi Inari" --parent kyoto --at 34.9671,135.7727 > kn/place.out \
    && grep -q 'private: stays home' kn/place.out && grep -A7 '^Name: place:fushimi-inari$' kn/home/config/places.rec > kn/place.txt \
    && grep -qx 'At: +34.9671+135.7727/' kn/place.txt && grep -qx 'Parent: place:kyoto' kn/place.txt && grep -qx 'Kind: site' kn/place.txt \
    && ! grep -q '135.77' kn/home/catalog/archive.rec \
    && ok "places: the gazetteer's entries (a city in its country); one of your own in one, private with a position, never in the record" \
    || { cat kn/place.out kn/place.txt; no "places: the gazetteer"; }
# tags in any script, one spelling a key: Latin accents folded (composed or decomposed alike), Latin,
# Greek and Cyrillic in lower case, other scripts kept; find folds the same way
mkdir -p u8/a u8/b u8/c && echo 1 > u8/a/x && echo 2 > u8/b/y && echo 3 > u8/c/z
printf '{"tags": {"a": ["place:Zürich", "Café"], "b": ["place:Zu\314\210rich"], "c": ["place:京都", "person:Ålice", "Αθήνα", "МОСКВА"]}, "authorship": "human", "agent": "t"}\n' > u8.json
"$tool" init u8h >/dev/null 2>&1
"$tool" -C u8h make -y --no-ecc --no-formats --set T --draft u8.json --output-dir u8out u8 >/dev/null 2>&1 || no "make (UTF-8 tags)"
"$tool" -C u8h tags > u8.out \
    && grep -q '^  zurich  *2 folders on 1 disc   Zürich$' u8.out && grep -q '^  京都 ' u8.out && grep -q '^  alice ' u8.out \
    && grep -q '^  cafe ' u8.out && grep -q '^  αθηνα ' u8.out && grep -q '^  москва ' u8.out \
    && "$tool" -C u8h map | grep -q '^  place:zurich  *Zürich, Switzerland ' \
    && [ "$("$tool" -C u8h find 'Zürich' | grep -c '^TAG')" -eq 2 ] && "$tool" -C u8h find 'ÅLICE' | grep -q 'person:alice' \
    && ok "tags in any script: one spelling a key (Zürich, decomposed or not, is place:zurich, the gazetteer's; Ålice is alice; 京都 kept); find folds alike" \
    || { cat u8.out; no "UTF-8 tags"; }
# extra place data: iso-codes' regions and a places/NAME.rec in the data folders; one a tag uses is copied home
mkdir -p xdata/iso-codes/json xhome/arv/places
printf '{"3166-2": [{"code": "JP-26", "name": "Kyoto", "type": "Prefecture"}, {"code": "JP-01", "name": "Hokkaido", "type": "Prefecture"}]}\n' \
    > xdata/iso-codes/json/iso_3166-2.json
printf '%%rec: Place\n\nName: place:uji\nLabel: Uji\nKind: city\nCountry: JP\nRegion: JP-01\nRegion: Kyoto\nAt: +34.89+135.80/\nSame: geonames:1849372\n' > xhome/arv/places/test.rec
xp() { ARV_NO_EXTRA_PLACES= XDG_DATA_DIRS="$dir/xdata" XDG_DATA_HOME="$dir/xhome" "$tool" "$@"; }
xp -C kn/home tags show place:kyoto-prefecture > kn/x1.out && grep -q '^  in: Japan (place:japan)$' kn/x1.out \
    && grep -q '^  same as: iso3166-2:JP-26$' kn/x1.out && xp -C kn/home tags show place:hokkaido | grep -q '(Prefecture)' \
    && xp -C kn/home tags show place:uji | grep -q '^  in: Kyoto, Japan (place:kyoto-prefecture)$' \
    && xp -C kn/home tags show place:kyoto | grep -q '^  in: Kyoto, Japan (place:kyoto-prefecture)$' \
    && ! grep -q '^Name: place:hokkaido$' kn/home/config/places.rec \
    && xp -C kn/home tags add place:a-ryokan --parent hokkaido >/dev/null \
    && grep -A8 '^Name: place:hokkaido$' kn/home/config/places.rec | grep -q '^Basis: from iso-codes (ISO 3166-2), copied when first used' \
    && mkdir -p kn/uji && echo 6 > kn/uji/u.jpg \
    && printf '{"tags": {".": ["place:uji"]}, "authorship": "human", "agent": "human:t"}\n' > kn/uji.json \
    && xp -C kn/home make -y --no-ecc --no-formats --set KN --access public --draft kn/uji.json --output-dir kn/out4 kn/uji >/dev/null 2>&1 \
    && grep -A12 '^Name: place:uji$' kn/home/config/places.rec | grep -q '^Basis: from test.rec, copied when first used' \
    && "$tool" -C kn/home tags show place:uji | grep -q '^  at: +34.89+135.80/$' \
    && 7z x -okn/disc4 kn/out4/*.iso catalog/config/places.rec >/dev/null && grep -q '^Name: place:uji$' kn/disc4/catalog/config/places.rec \
    && xp programs | grep -q '^  ok       iso-codes ' && "$tool" -C kn/home tags names | grep -qv 'place:hokkaido' \
    && xp -C kn/home find place:kyoto-prefecture | grep -q '  data/  (place:uji)$' \
    && ok "extra place data: iso-codes' regions in their countries, a place's Region by name (not a code when a name is given), places/NAME.rec from the data folders; one a tag uses is copied home (and onto the disc)" \
    || { cat kn/x1.out; no "extra place data"; }
"$tool" -C kn/home tags add place:kyoto --at nowhere >/dev/null 2>&1 && no "a position that is not ISO 6709 was taken"
"$tool" -C kn/home tags add place:kyoto --extent "34.9,135.7" >/dev/null 2>&1 && no "an extent of one corner was taken"
"$tool" -C kn/home tags add place:kyoto --reach far >/dev/null 2>&1 && no "a reach that is not a distance was taken"
"$tool" -C kn/home tags add place:a-park --parent kyoto --extent "+35.0105+135.7495/ 35.02,135.76" --reach 500m > kn/park.out \
    && grep -q 'private: stays home' kn/park.out && grep -A8 '^Name: place:a-park$' kn/home/config/places.rec > kn/park.txt \
    && grep -qx 'Extent: +35.0105+135.7495/+35.02+135.76/' kn/park.txt && grep -qx 'Reach: 500m' kn/park.txt \
    && "$tool" -C kn/home tags show place:a-park | grep -q '^  reach: 0.5 km' \
    && ok "places: a place's extent (two corners, ISO 6709, to four decimals) and reach (2km, 500m); with either, private" \
    || { cat kn/park.out kn/park.txt; no "places: extent and reach"; }
"$tool" -C kn/home tags add person:x --parent kyoto >/dev/null 2>&1 && no "a person was given a parent place"
mkdir -p kn/trip/temple kn/trip/castle && echo 4 > kn/trip/temple/t.jpg && echo 5 > kn/trip/castle/c.jpg
printf '{"tags": {".": ["place:kyoto"], "temple": ["place:fushimi-inari"], "castle": ["place:osaka", "place:nowhere-known"]}, "authorship": "human", "agent": "human:t"}\n' > kn/trip.json
"$tool" -C kn/home make -y --no-ecc --no-formats --set KN --access public --draft kn/trip.json --output-dir kn/out3 kn/trip >/dev/null 2>&1 \
    || no "make (places)"
"$tool" -C kn/home map > kn/map.out \
    && grep -q '^  place:kyoto  *Kyoto, Japan  *+35.03+135.75/  *1 object on 1 disc$' kn/map.out \
    && grep -q '^  place:fushimi-inari  *Fushimi Inari, Kyoto, Japan  *+34.9671+135.7727/  *1 path on 1 disc; private$' kn/map.out \
    && grep -q '^  place:nowhere-known  *(not in the gazetteer)' kn/map.out && grep -q '^1 not in the gazetteer' kn/map.out \
    && "$tool" -C kn/home map --geojson > kn/map.geojson && grep -q '"type": "FeatureCollection"' kn/map.geojson \
    && grep -q '"coordinates": \[135.4600, 34.7500\]}, "properties": {"key": "place:osaka"' kn/map.geojson && ! grep -q nowhere-known kn/map.geojson \
    && "$tool" -C kn/home map --json | grep -q '"key": "place:nowhere-known", .*"position": "none", "inGazetteer": false' \
    && "$tool" -C kn/home map --svg | grep -q '<title>Ōsaka, Japan: 1 path or object</title>' \
    && "$tool" -C kn/home find place:japan > kn/find.out && grep -q 'data/castle/  (place:osaka' kn/find.out \
    && grep -q 'data/temple/  (place:fushimi-inari)' kn/find.out && ! "$tool" -C kn/home find place:osaka | grep -q temple \
    && ok "arv map: the places the tags name, where each is (the gazetteer's, or a broader place's), as text, JSON, GeoJSON and SVG; find place:japan finds the places in it" \
    || { cat kn/map.out kn/map.geojson; no "arv map"; }
7z x -okn/disc3 kn/out3/*.iso index.html catalog/config/places.rec >/dev/null \
    && grep -q '<h2>Places</h2>' kn/disc3/index.html && grep -q '<svg xmlns=' kn/disc3/index.html \
    && grep -q '<td>Ōsaka, Japan</td><td><code>place:osaka</code>' kn/disc3/index.html \
    && ! grep -q 'Fushimi Inari' kn/disc3/index.html && grep -q '<code>place:fushimi-inari</code>' kn/disc3/index.html \
    && grep -q '^Name: place:kyoto$' kn/disc3/catalog/config/places.rec && ! grep -q '^Name: place:fushimi' kn/disc3/catalog/config/places.rec \
    && ok "a disc's index.html shows its places on a map; a public disc says nothing of a private place but its tag, and its gazetteer leaves it out" \
    || no "places on a disc"

# ------------------------------------------------------------------ what depends on the machine
# RS03, added and tested by arv itself (src/rs03); no dvdisaster needed. A small stand-in for
# tools/: the real checkout is far larger than this test's medium (a real disc carries it all)
mkdir -p ecc-tools/src/arv
echo "arv@test" > ecc-tools/VERSION
printf '/* arv.c stand-in */\n' > ecc-tools/src/arv/arv.c
"$tool" make -C ecc-home --no-formats --set CODE --medium-sectors 9600 --output-dir ecc-out --tools ecc-tools src >/dev/null 2>&1 \
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
out=$(ARV_TEST_AFTER_HASH="echo second > race-src/a.txt" "$tool" make -y -C race-home --no-formats --no-ecc --set CODE \
      --output-dir race-out --tools ecc-tools race-src 2>&1) && no "a file changed mid-make was not refused"
echo "$out" | grep -q "changed after it was hashed" && echo "$out" | grep -q "^  a.txt$" \
    && [ -z "$(ls race-out/*.iso 2>/dev/null)" ] && ! grep -q "^Id:" race-home/catalog/archive.rec 2>/dev/null \
    && ok "arv make: a file changed between hashing and writing is refused, and nothing is recorded" \
    || no "a file changed mid-make: $out"
# portable: arv and a .arv home side by side on a drive, run from anywhere: the drive's home, and
# nothing written outside the drive (no machine config)
mkdir -p drive/tools portable-user
cp "$tool" drive/tools/arv
(cd drive && HOME="$dir/portable-user" XDG_CONFIG_HOME= XDG_DATA_HOME= ./tools/arv init >/dev/null 2>&1)
where=$(cd / && HOME="$dir/portable-user" XDG_CONFIG_HOME= XDG_DATA_HOME= "$dir/drive/tools/arv" where 2>&1)
echo "$where" | grep -q "drive/.arv$" && echo "$where" | grep -q "beside arv itself" \
    && [ -z "$(ls -A portable-user)" ] \
    && ok "portable: arv finds the .arv home on its own drive, from anywhere, and writes nothing outside it" \
    || no "portable home: $where"
[ "$(sed -n 's/^ImageSha256: //p' ecc-home/catalog/archive.rec)" = "$(sha256sum < "$iso" | cut -d' ' -f1)" ] \
    && [ "$(sed -n 's/^ImageSectors: //p' ecc-home/catalog/archive.rec)" -eq $(($(wc -c < "$iso") / 2048)) ] \
    && ok "the home's Binding records the finished image's size and SHA-256 (to check a burned disc against)" \
    || no "ImageSectors / ImageSha256"
# check --drive reads the image's sectors back and compares (a file stands in for the drive)
id=$(sed -n 's/^Id: //p' ecc-home/catalog/archive.rec | head -1)
cp "$iso" drive.img
dd if=/dev/zero bs=2048 count=40 >> drive.img 2>/dev/null       # a drive gives sectors past the image too
out=$(PATH=/usr/bin:/bin "$tool" check -C ecc-home --drive drive.img "$id" 2>&1) \
    && echo "$out" | grep -q "holds exactly the image arv made" && grep -q "Note: read-back of the whole image" ecc-home/catalog/archive.rec \
    || { echo "$out"; no "check --drive read-back"; }
printf X | dd of=drive.img bs=1 seek=$((7000 * 2048)) conv=notrunc 2>/dev/null
"$tool" check -C ecc-home --drive drive.img "$id" >/dev/null 2>&1 && no "check --drive missed a changed sector"
head -c $((4000 * 2048)) "$iso" > drive.img
out=$("$tool" check -C ecc-home --drive drive.img "$id" 2>&1) && no "check --drive missed a short disc"
echo "$out" | grep -q "the disc ends at sector 4000" \
    && ok "check --drive: reads the image back past the cache, no dvdisaster needed; finds a changed sector, a short disc" \
    || { echo "$out"; no "check --drive on a short disc"; }
checks=$(grep -c 'Type: fixity check' ecc-home/catalog/archive.rec)
"$tool" check -C ecc-home "$iso" --note "yearly check" >/dev/null 2>&1 \
    && [ "$(grep -c 'Type: fixity check' ecc-home/catalog/archive.rec)" -eq $((checks + 1)) ] \
    && ok "arv check IMAGE: identical to the image arv made, logged" || no "arv check"
cp "$iso" damaged.iso
printf X | dd of=damaged.iso bs=1 seek=$((40 * 2048)) conv=notrunc 2>/dev/null
out=$("$tool" check -C ecc-home damaged.iso "$id" 2>&1) && no "check missed damage"
echo "$out" | grep -q "1 data sectors with a wrong CRC" && ok "arv check IMAGE finds a damaged sector (its RS03 data says which)" || { echo "$out"; no "damage report"; }
cp damaged.iso light.iso
# repaired by arv itself: a damaged sector, a zero-filled run (as ddrescue or dd leave) and a missing end
printf '\0\0\0\0\0\0\0\0' | dd of=damaged.iso bs=1 seek=$((3000 * 2048)) conv=notrunc 2>/dev/null
dd if=/dev/zero of=damaged.iso bs=2048 seek=5000 count=20 conv=notrunc 2>/dev/null
cp damaged.iso nohome.iso
truncate -s $(( ($(wc -c < "$iso") / 2048 - 100) * 2048 )) damaged.iso
out=$("$tool" check -C ecc-home damaged.iso --repair "$id" 2>&1) || { echo "$out"; no "arv check --repair"; }
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
"$tool" check -C ecc-home lost.iso --repair "$id" >lost.txt 2>&1 && no "--repair claims to have repaired a lost layout"
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
out=$(cd nowhere && ARV_HOME="$dir/nowhere/none" "$tool" check ../nohome.iso --repair 2>&1) || { echo "$out"; no "repair without a home"; }
cmp -s nohome.iso "$iso" && echo "$out" | grep -q "not logged" \
    && ok "arv check --repair works on an image in no catalogue (as from a disc found decades on)" || { echo "$out"; no "repair without a home"; }
# overdue checks: a disc checked today is not overdue now, but is five years on; one never checked is
[ -z "$("$tool" list -C ecc-home --unchecked-since 5y)" ] \
    && SOURCE_DATE_EPOCH=$(( $(date +%s) + 6 * 366 * 86400 )) "$tool" list -C ecc-home --unchecked-since 5y | grep -q "last checked 20" \
    && "$tool" make -C due-home --no-formats --no-ecc --set CODE --output-dir due-out --tools ecc-tools src >/dev/null 2>&1 \
    && "$tool" list -C due-home --unchecked-since 1y | grep -q "last checked never" \
    && due=$("$tool" list -C due-home | cut -f1) \
    && "$tool" --home due-home burned --copies 1 --location BOX "$due" >/dev/null 2>&1 \
    && "$tool" list -C due-home --one-place | grep -q "^CODE" \
    && "$tool" --home due-home burned --copies 1 --location SHELF "$due" >/dev/null 2>&1 \
    && [ -z "$("$tool" list -C due-home --one-place)" ] \
    && ok "list --unchecked-since and --one-place: the discs due a check, and those kept in one place" \
    || no "list --unchecked-since / --one-place"
# the one cc line README.txt gives, from the disc's own tools/
mkdir -p cc-build
(cd disc && cc -O2 -pthread -o ../cc-build/arv tools/arv/src/arv/*.c tools/arv/src/bagit/bagit.c tools/arv/src/udfwrite/udfwrite.c \
    tools/arv/src/rs03/rs03.c >/dev/null 2>&1) && cc-build/arv check disc >/dev/null \
    && ok "README.txt's one cc line builds arv from the disc's tools/" || no "building arv from tools/"
if command -v dvdisaster >/dev/null && dvdisaster --help 2>&1 | grep -q no-bdr-defect-management; then
    dvdisaster -i "$iso" -t --no-progress 2>&1 | grep -q "Ecc block test *: pass" \
        && ok "dvdisaster Light accepts arv's RS03 image" || no "dvdisaster Light -t"
    dvdisaster -i light.iso -f --no-progress >/dev/null 2>&1 || true        # it exits 1 after repairing
    cmp -s light.iso "$iso" && ok "dvdisaster Light repairs it back to the image arv made" || no "dvdisaster Light -f"
fi
if command -v sf >/dev/null && sf -version >/dev/null 2>&1; then
    "$tool" make -C sf-home --no-ecc --set CODE --output-dir sf-out src >/dev/null 2>&1 \
        || no "arv make with Siegfried"
    grep -q '^docs/guide.md,' sf-home/catalog/volumes/*/formats.csv \
        && grep -q "Type: format identification" sf-home/catalog/archive.rec \
        && ok "Siegfried: formats.csv and the format identification event" || no "Siegfried formats"
fi

# a disc plan's formats: Siegfried run on each item where it is, its rows at the item's place on the
# disc (a stand-in sf answering in Siegfried's CSV, so this runs without it)
mkdir -p fake-sf plan-sf/pc/photos plan-sf/elsewhere
cat > fake-sf/sf <<'SF'
#!/bin/sh
[ "$1" = -version ] && { echo "siegfried 0.0.0 (stand-in for arv's check)"; exit 0; }
for last; do :; done
echo "filename,filesize,modified,errors,namespace,id,format,version,mime,basis,warning"
find "$last" -type f | sort | while read -r f; do echo "$f,1,2020-01-01,,pronom,x-fmt/test,Test format,,text/plain,stand-in,"; done
SF
chmod +x fake-sf/sf
fake_sf=$(pwd)/fake-sf
echo a > plan-sf/pc/photos/a.jpg; echo b > plan-sf/pc/photos/b.jpg; echo n > plan-sf/elsewhere/notes.txt
"$tool" init plan-sf >/dev/null 2>&1
( cd plan-sf && "$tool" plan new sf --set TRIP >/dev/null && "$tool" plan add sf pc/photos >/dev/null \
    && "$tool" plan add sf elsewhere/notes.txt --as docs/notes.txt >/dev/null \
    && PATH="$fake_sf:$PATH" "$tool" plan make sf -y --no-ecc --output-dir out >/dev/null 2>&1 ) \
    || no "arv plan make with Siegfried"
f=$(ls plan-sf/.arv/catalog/volumes/*/formats.csv 2>/dev/null | head -1)
[ -n "$f" ] && grep -q '^photos/a.jpg,x-fmt/test,' "$f" && grep -q '^photos/b.jpg,' "$f" && grep -q '^docs/notes.txt,' "$f" \
    && ok "a disc plan's formats.csv: each item identified where it is, at its place on the disc" || no "plan formats"

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
"$tool" check lengths LEN-01_2026_X >/dev/null && ok "SHA-256 matches sha256sum for 0 to 130 bytes" || no "SHA-256"
(cd lengths && "$here/build/fixtures" --hash data/* > ../sha512.c.txt && sha512sum data/* > ../sha512.txt)
cmp -s sha512.c.txt sha512.txt && ok "SHA-512 matches sha512sum for 0 to 130 bytes" || no "SHA-512"

# ------------------------------------------------------------------ event ids and arv audit
cp -r home aud && "$tool" -C aud where --here >/dev/null   # a copy, made one of its own
did=$(sed -n 's/^Id: //p' disc/catalog.rec | head -1)
"$tool" -C aud note "$did" "the same words" >/dev/null && "$tool" -C aud note "$did" "the same words" >/dev/null
n_ev=$(grep -c '^EventId: ' aud/catalog/archive.rec)
[ "$(grep -c '^EventId: [0-9a-f]\{32\}$' aud/catalog/archive.rec)" -ge 3 ] && grep -q '^Nonce: 2$' aud/catalog/archive.rec \
    && grep -q '^EventId: ' disc/catalog/archive.rec \
    && ok "event ids: every event has one, on discs too; two events alike are kept apart by a Nonce" \
    || no "event ids"
"$tool" -C aud audit disc > audit.out && grep -q " 0 changed since written" audit.out && grep -q "0 missing at home, 0 different" audit.out \
    || { cat audit.out; no "arv audit, untouched"; }
first=$(sed -n 's/^EventId: //p' disc/catalog/archive.rec | head -1)
awk -v id="$first" 'BEGIN { RS = ""; ORS = "\n\n" } $0 ~ "EventId: " id { sub(/Outcome: success/, "Outcome: failure") } { print }' \
    aud/catalog/archive.rec > aud.rec && mv aud.rec aud/catalog/archive.rec
"$tool" -C aud audit disc > audit.out && no "arv audit passed a changed event"
grep -q "^CHANGED .*$first" audit.out && grep -q "^DIFFERS .*$first" audit.out || { cat audit.out; no "arv audit, a changed event"; }
awk -v id="$first" 'BEGIN { RS = ""; ORS = "\n\n" } $0 !~ "EventId: " id { print }' aud/catalog/archive.rec > aud.rec \
    && mv aud.rec aud/catalog/archive.rec
"$tool" -C aud audit disc > audit.out || true; grep -q "^MISSING .*$first" audit.out || { cat audit.out; no "arv audit, a lost event"; }
"$tool" -C aud rebuild disc >/dev/null && "$tool" -C aud rebuild disc >/dev/null || no "rebuild"
"$tool" -C aud audit disc > audit.out && [ "$(grep -c "^EventId: $first" aud/catalog/archive.rec)" = 1 ] \
    && [ "$(grep -c '^EventId: ' aud/catalog/archive.rec)" = "$n_ev" ] \
    && ok "arv audit: an event changed at home, and one lost, are found against the disc; rebuild brings the lost one back once" \
    || { cat audit.out; no "audit after rebuild"; }

# ------------------------------------------------------------------ two catalogues of one archive: the guard, and merging
mkdir -p two/a/T1 two/a/T2 two/a/T3
echo 1 > two/a/T1/f && echo 2 > two/a/T2/f && echo 3 > two/a/T3/f
(cd two/a && "$tool" init . >/dev/null && "$tool" make -y --no-ecc --no-formats --set CODE --output t1.iso T1 >/dev/null 2>&1) || no "make in two/a"
t1=$(sed -n 's/^Id: //p' two/a/.arv/catalog/archive.rec | head -1)
cp -r two/a two/b
"$tool" -C two/b/.arv note "$t1" "from b" 2>guard.err && no "a copied catalogue took a change"
grep -q "a copy of it" guard.err && grep -q "where --here" guard.err || { cat guard.err; no "the copy guard's message"; }
"$tool" -C two/b/.arv where --here >/dev/null && "$tool" -C two/b/.arv note "$t1" "from b" >/dev/null || no "where --here"
"$tool" -C two/b/.arv burned "$t1" --copies 1 --location ATTIC >/dev/null 2>&1 && "$tool" -C two/a/.arv burned "$t1" --copies 1 --location SHELF >/dev/null 2>&1 \
    || no "burned in both"
(cd two/b && "$tool" make -y --no-ecc --no-formats --set CODE --output t2.iso T2 >/dev/null 2>&1) \
    && (cd two/a && "$tool" make -y --no-ecc --no-formats --set CODE --output t3.iso T3 >/dev/null 2>&1) || no "make in both"
"$tool" -C two/a/.arv rebuild two/b > merge.out 2> merge.err && no "a clashing disc id was merged"
grep -q "two copies lettered A" merge.err && grep -q "^Not merged: CODE-02" merge.err && grep -q "^Note: from b" two/a/.arv/catalog/archive.rec \
    && [ "$(grep -c '^Type: replication' two/a/.arv/catalog/archive.rec)" = 2 ] \
    && ok "two catalogues of one archive: a copy takes no change until arv where --here; rebuild merges the other by event id (notes too), and reports a copy letter and a disc id both gave out" \
    || { cat merge.out merge.err; no "merging two catalogues"; }

# one disc recorded in both: counted once, by the identifier both recorded
mkdir -p dup/a/T && echo d > dup/a/T/f
(cd dup/a && "$tool" init . >/dev/null && "$tool" make -y --no-ecc --no-formats --set CODE --output d.iso T >/dev/null 2>&1) || no "make in dup/a"
d1=$(sed -n 's/^Id: //p' dup/a/.arv/catalog/archive.rec | head -1)
cp -r dup/a dup/b && "$tool" -C dup/b/.arv where --here >/dev/null
"$tool" -C dup/a/.arv burned "$d1" --identifier barcode:ARV-7 --location SHELF >/dev/null 2>&1 && "$tool" -C dup/b/.arv burned "$d1" --identifier barcode:ARV-7 --location ATTIC >/dev/null 2>&1 \
    || no "burned with an identifier"
"$tool" -C dup/a/.arv rebuild dup/b > dup.out 2> dup.err || { cat dup.out dup.err; no "rebuild of one disc recorded twice"; }
rc=0; "$tool" -C dup/a/.arv audit > dup.audit || rc=$?
grep -q "^TWICE .* copies A and A are one disc (barcode:ARV-7)" dup.out && ! grep -q "two copies lettered" dup.err \
    && "$tool" -C dup/a/.arv objects "$d1" | grep -q ": 1 copy$" && [ "$rc" = 1 ] && grep -q "^TWICE" dup.audit \
    && ok "copies by identity: one disc recorded in two catalogues (barcode:) is counted once, and rebuild and audit say so" \
    || { cat dup.out dup.err dup.audit; "$tool" -C dup/a/.arv objects "$d1"; no "one disc recorded twice"; }

# ------------------------------------------------------------------ what is in a drive, and what an image is
mkdir -p drv/T1 drv/T2 && echo 1 > drv/T1/a && echo 2 > drv/T2/b
(cd drv && "$tool" init . >/dev/null && "$tool" make -y --no-ecc --no-formats --set CODE --output i1.iso T1 >/dev/null 2>&1 \
    && "$tool" make -y --no-ecc --no-formats --set CODE --output i2.iso T2 >/dev/null 2>&1) || no "make in drv"
v1=$(sed -n 's/^Id: //p' drv/.arv/catalog/archive.rec | head -1)
"$tool" -C drv/.arv burned "$v1" --device drv/i1.iso --location SHELF >/dev/null 2>&1 || no "burned in drv"
mkdir -p drv/nas && cp drv/i1.iso drv/nas/ && "$tool" -C drv/.arv stored "$v1" drv/nas/i1.iso --location NAS >/dev/null 2>&1 || no "stored in drv"
: > drv/empty.iso
"$tool" -C drv/.arv drives drv/i1.iso drv/i2.iso drv/empty.iso drv/none > drives.out || no "arv drives"
grep -q "^drv/i1.iso  $v1 \"T1\" copy A (its only burned copy; read back when recorded)" drives.out \
    && grep -q '^drv/i2.iso  CODE-02.*: which copy is not known' drives.out && grep -q '^drv/empty.iso  .*not an arv disc' drives.out \
    && grep -q '^drv/none  cannot be read' drives.out || { cat drives.out; no "arv drives says what is in each"; }
"$tool" -C drv/.arv drives --check drv/i1.iso > drives.out 2>&1 && grep -q "^$v1 copy A: OK" drives.out || { cat drives.out; no "arv drives --check"; }
(cd drv && find . -name '*.iso' -exec "$tool" -C .arv info {} \;) > info.out || true
grep -q "^./nas/i1.iso  $v1 \"T1\" copy B (stored here" info.out && grep -q "^./i2.iso  CODE-02.*not recorded as a copy" info.out \
    && grep -q "^./empty.iso  not a disc image" info.out || { cat info.out; no "arv info IMAGE, with find"; }
mkdir -p drv/nas2 && mv drv/nas/i1.iso drv/nas2/
"$tool" -C drv/.arv todo | grep -q "^  $v1  copy B  .*/drv/nas/i1.iso" || no "todo: a stored copy not where recorded"
"$tool" -C drv/.arv todo --json | grep -q "{\"kind\": \"missing\", \"disc\": \"$v1\", \"copy\": \"B\", \"path\": \"[^\"]*/drv/nas/i1.iso\", \"text\": " \
    || no "todo --json: the same, as an item of its kind"
"$tool" -C drv/.arv stored "$v1" drv/nas2/i1.iso --copy B >/dev/null 2>&1 || no "stored --copy: a moved copy"
! "$tool" -C drv/.arv todo | grep -q "Stored copies not found" && "$tool" -C drv/.arv objects "$v1" | grep -q ": 2 copies$" \
    && ok "arv drives names the disc and copy in each drive (and --check checks them); arv info knows an image file (find -exec); todo finds a stored copy gone from its path, and stored --copy records the move" \
    || { "$tool" -C drv/.arv todo; no "a moved stored copy"; }

# copies on a carrier that holds many images (a tape, a drive): counted by carrier
v2=$(sed -n 's/^Id: //p' drv/.arv/catalog/archive.rec | sed -n 2p)
mkdir -p drv/tape1 drv/tape2 && cp drv/i1.iso drv/tape1/x.iso && cp drv/i1.iso drv/tape1/y.iso && cp drv/i2.iso drv/tape1/ && cp drv/i1.iso drv/tape2/
for f in x y; do "$tool" -C drv/.arv stored "$v1" drv/tape1/$f.iso --carrier barcode:TAPE01L8 > carrier.out 2>&1 || no "stored --carrier"; done
grep -q "count as one" carrier.out || { cat carrier.out; no "stored notes a shared carrier"; }
"$tool" -C drv/.arv stored "$v2" drv/tape1/i2.iso --carrier barcode:TAPE01L8 >/dev/null 2>&1 || no "a second disc on the tape"
"$tool" -C drv/.arv objects "$v1" | grep -q ": 3 copies$" || { "$tool" -C drv/.arv objects "$v1"; no "copies on one carrier count once"; }
"$tool" -C drv/.arv todo | grep -q "^  $v1  copies C and D on barcode:TAPE01L8" || no "todo: copies on one carrier"
"$tool" -C drv/.arv audit > carrier.out && ! grep -q "TWO DISCS" carrier.out || { cat carrier.out; no "two discs on one tape are not a clash"; }
"$tool" -C drv/.arv stored "$v1" drv/tape2/i1.iso --copy D --carrier barcode:TAPE02L8 >/dev/null 2>&1 || no "a copy moved to another tape"
"$tool" -C drv/.arv objects "$v1" | grep -q ": 4 copies$" \
    && ok "carriers: copies on one tape count once and todo says so; two discs on one tape are no clash; a copy moved to another tape counts again" \
    || { "$tool" -C drv/.arv objects "$v1"; no "carrier after a move"; }

# a tape library or jukebox: the worklist (todo --tsv), the inventory, and the example mtx loop
mkdir -p lib/T1 lib/T2 lib/T3 lib/mnt lib/bin && echo 1 > lib/T1/a && echo 2 > lib/T2/b && echo 3 > lib/T3/c
(cd lib && "$tool" init . >/dev/null && "$tool" location add LIB1 "Tape library" >/dev/null \
    && for t in T1 T2 T3; do "$tool" make -y --no-ecc --no-formats --set CODE --output $t.iso $t >/dev/null 2>&1 || exit 1; done) \
    || no "make in lib"
l1=$(sed -n 's/^Id: \(CODE-01.*\)/\1/p' lib/.arv/catalog/archive.rec | head -1)
l2=$(sed -n 's/^Id: \(CODE-02.*\)/\1/p' lib/.arv/catalog/archive.rec | head -1)
l3=$(sed -n 's/^Id: \(CODE-03.*\)/\1/p' lib/.arv/catalog/archive.rec | head -1)
"$tool" -C lib/.arv burned "$l1" --copies 1 --identifier barcode:DISC01 --location LIB1 >/dev/null 2>&1 || no "burned in lib"
cp lib/T2.iso lib/mnt/ && "$tool" -C lib/.arv stored "$l2" lib/mnt/T2.iso --carrier barcode:TAPE01L8 --location LIB1 >/dev/null 2>&1 \
    || no "stored on a tape in lib"
"$tool" -C lib/.arv burned "$l3" --copies 1 --location LIB1 >/dev/null 2>&1 || no "burned, no barcode"
"$tool" -C lib/.arv todo --tsv > todo.tsv
grep -q "^read-back	$l1	A	disc	barcode:DISC01		LIB1		$" todo.tsv && ! grep -q "	$l2	" todo.tsv || { cat todo.tsv; no "todo --tsv"; }
"$tool" -C lib/.arv todo --tsv --overdue 2999 | grep -q "^overdue	$l2	A	iso		barcode:TAPE01L8	LIB1	.*/lib/mnt/T2.iso	" \
    || no "todo --tsv: an overdue copy on a tape"
printf '%s\n' '  Storage Changer /dev/sg3:1 Drives, 4 Slots ( 0 Import/Export )' 'Data Transfer Element 0:Empty' \
    '      Storage Element 1:Full :VolumeTag=DISC01' '      Storage Element 2:Full :VolumeTag=TAPE01L8' \
    '      Storage Element 3:Full :VolumeTag=CLN001L1' '      Storage Element 4:Empty' > lib/status.txt
rc=0; "$tool" -C lib/.arv inventory LIB1 lib/status.txt > inv.out || rc=$?
grep -q ": 3 barcodes read; 2 known, 1 unknown; 0 copies recorded here missing, 1 without a barcode" inv.out \
    && grep -q "^UNMARKED   $l3 copy A" inv.out && grep -q "^UNKNOWN    slot 3 CLN001L1" inv.out && [ $rc = 0 ] \
    || { cat inv.out; no "arv inventory"; }
grep -v TAPE01L8 lib/status.txt | "$tool" -C lib/.arv inventory LIB1 > inv.out && no "inventory: a missing tape passed"
grep -q "^MISSING    $l2 copy A" inv.out || { cat inv.out; no "inventory: the missing tape"; }
printf '#!/bin/sh\ncase "$3" in status) cat "%s/lib/status.txt";; *) echo "$3 $4 $5" >> "%s/lib/mtx.log";; esac\n' "$PWD" "$PWD" > lib/bin/mtx
chmod +x lib/bin/mtx
(cd lib && ARV="$tool" MTX="$PWD/bin/mtx" OVERDUE=2999 LTFS_MOUNT=true LTFS_UMOUNT=true \
    sh "$repo/docs/examples/library-check.sh" /dev/sg3 0 T1.iso "$PWD/mnt") > loop.out 2>&1 || { cat loop.out; no "library-check.sh"; }
[ "$(cat lib/mtx.log)" = "$(printf 'load 1 0\nunload 1 0\nload 2 0\nunload 2 0')" ] && grep -q "^skip: $l3 copy A has no barcode" loop.out \
    && grep -q "^$l1 copy A: OK" loop.out && grep -q "^$l2 copy A: OK" loop.out \
    && ! "$tool" -C lib/.arv todo --tsv | grep -q "^read-back	$l1" \
    && ok "a library: todo --tsv is the worklist, inventory reconciles mtx status with the record, and the example mtx loop loads, checks and unloads by barcode" \
    || { cat loop.out lib/mtx.log; no "the library loop"; }

# scanning barcodes: find a copy by a scanned case, --barcode, and a shelf read from the keyboard
"$tool" -C lib/.arv find DISC01 > scan.out && grep -q "^COPY  $l1 copy A  barcode:DISC01  \[Tape library; " scan.out \
    || { cat scan.out; no "find by a scanned barcode"; }
"$tool" -C lib/.arv find TAPE01L8 | grep -q "^COPY  $l2 copy A  barcode:TAPE01L8 (the tape or drive it is on)" || no "find by a tape's barcode"
"$tool" -C lib/.arv find --json TAPE01L8 | grep -q "\"copies\": \[{\"disc\": \"$l2\", \"copy\": \"A\", \"identifier\": \"barcode:TAPE01L8\", \"onCarrier\": true" \
    || no "find --json lists the copies found"
"$tool" -C lib/.arv identify "$l3" A --barcode " CASE07 " >/dev/null || no "identify --barcode"
"$tool" -C lib/.arv find case07 | grep -q "^COPY  $l3 copy A  barcode:CASE07" || no "--barcode, trimmed and found again"
printf 'DISC01\nCASE07\nNOPE9\n' | "$tool" -C lib/.arv inventory LIB1 > scan.out || true
grep -q "^MISSING    $l2 copy A" scan.out && grep -q "^UNKNOWN    NOPE9: " scan.out && ! grep -q UNMARKED scan.out \
    && ok "barcodes: arv find names the disc and copy of a scanned case or tape; --barcode takes a scan; inventory takes a shelf read typed by a scanner" \
    || { cat scan.out; no "a shelf read by hand"; }

# a field changed apart in two catalogues, and a copy relabelled
mkdir -p rl/a/T && echo r > rl/a/T/f
(cd rl/a && "$tool" init . >/dev/null && "$tool" make -y --no-ecc --no-formats --set CODE --output r.iso T >/dev/null 2>&1) || no "make in rl"
r1=$(sed -n 's/^Id: //p' rl/a/.arv/catalog/archive.rec | head -1)
"$tool" -C rl/a/.arv access "$r1" public && cp -r rl/a rl/b && "$tool" -C rl/b/.arv where --here >/dev/null || no "rl setup"
"$tool" -C rl/b/.arv access "$r1" sealed && "$tool" -C rl/a/.arv access "$r1" private || no "access in both"
"$tool" -C rl/a/.arv burned "$r1" --copies 1 --location SHELF >/dev/null 2>&1 && "$tool" -C rl/b/.arv burned "$r1" --copies 1 --location ATTIC >/dev/null 2>&1 \
    || no "burned in both"
"$tool" -C rl/a/.arv rebuild rl/b > rl.out 2>&1 || { cat rl.out; no "rebuild in rl"; }
grep -q "^CONFLICT  $r1 Access: changed apart" rl.out && grep -q "arv relabel $r1 A NEW --at ATTIC" rl.out || { cat rl.out; no "rebuild reports a field conflict"; }
rc=0; "$tool" -C rl/a/.arv audit > rl.out || rc=$?
[ $rc = 1 ] && grep -q "^CONFLICT  $r1 Access" rl.out || { cat rl.out; no "audit reports a field conflict"; }
"$tool" -C rl/a/.arv access "$r1" private && "$tool" -C rl/a/.arv audit > rl.out || { cat rl.out; no "setting it again settles it"; }
"$tool" -C rl/a/.arv relabel "$r1" A B 2> rl.out && no "relabel of an ambiguous letter"
grep -q "has 2 copies lettered A" rl.out || { cat rl.out; no "relabel says which"; }
"$tool" -C rl/a/.arv relabel "$r1" A B --at ATTIC >/dev/null || no "relabel --at"
"$tool" -C rl/a/.arv check --drive rl/a/r.iso --copy B "$r1" >/dev/null 2>&1 || no "check of a relabelled copy"
"$tool" -C rl/a/.arv objects "$r1" > rl.out
grep -q "^  A  disc, cold, .*SHELF; not read back" rl.out && grep -q "^  B  disc, cold, .*ATTIC; last checked" rl.out \
    && ok "two catalogues: a field changed in both is a conflict until set again; relabel gives one of two copies lettered alike a new letter, and a check finds it by it" \
    || { cat rl.out; no "after relabel"; }

# ------------------------------------------------------------------ the manual: arv help --all and --man
"$tool" help --all > manual.md || no "arv help --all"
missing=""
for c in $("$tool" __complete ''); do
    case "$c" in archive|record|home) continue;; esac
    grep -q "arv $c\b" manual.md || missing="$missing $c"
done
[ -z "$missing" ] || no "arv help --all leaves out:$missing"
grep -q '^## Exit status$' manual.md && grep -q '^## For scripts and agents$' manual.md && grep -q '^### arv make in full$' manual.md \
    || no "arv help --all: its sections"
"$tool" help --man > manual.1 && head -1 manual.1 | grep -q '^\.TH ARV 1 ' || no "arv help --man"
if command -v groff >/dev/null; then
    groff -man -Tutf8 -ww manual.1 2> groff.err > /dev/null && [ ! -s groff.err ] || { cat groff.err; no "the man page: groff warnings"; }
fi
ok "the manual: arv help --all names every command, with exit status and the rules for scripts and agents; --man is it as a man page"

# ------------------------------------------------------------------ the helpers: arv-assist and arv-gui
mkdir -p helpers
printf '#!/bin/sh\necho "assist $*"\n' > helpers/assist
printf '#!/bin/sh\necho "gui $*"\n' > helpers/gui
chmod +x helpers/assist helpers/gui
cp -r "$repo/tests/reference/home" arv-home
[ "$(ARV_ASSIST=helpers/assist "$tool" --home arv-home describe Photos --save d.json)" = "assist --home arv-home describe Photos --save d.json" ] \
    || no "arv describe was not handed to arv-assist"

[ "$(ARV_GUI=helpers/gui "$tool" -C arv-home gui --port 0)" = "gui --home arv-home --port 0" ] \
    || no "arv gui was not handed to arv-gui with the home"
ARV_ASSIST=/nonexistent/arv-assist PATH=/nonexistent "$tool" tag x 2>&1 | grep -q "needs arv-assist" || no "missing arv-assist message"
"$tool" --help 2>&1 | grep -q "usage: arv" || no "arv --help"
ok "arv: describe and tag go to arv-assist, gui to arv-gui (with the home); the rest runs here"
echo "all checks passed"
