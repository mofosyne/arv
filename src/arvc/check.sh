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
echo "all checks passed"
