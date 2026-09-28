#!/bin/bash
#
# End-to-end smoke test of the archival pipeline, with no mounting or root needed:
#
#   1. Bag a small payload with BagIt (sha256 + sha512 manifests)
#   2. Build a hybrid ISO9660/UDF image with scripts/create-archive-iso.sh
#      (which also augments it with dvdisaster RS03 ECC)
#   3. Zero out a run of sectors to simulate disc damage
#   4. Repair the image with dvdisaster and check it matches the original
#   5. Extract the repaired image and validate the bag
#
# Requires: genisoimage, dvdisaster, 7z (p7zip-full), python3 with bagit (pip install bagit)
#
# Usage: tests/smoke-test.sh

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)

for cmd in genisoimage dvdisaster 7z python3; do
    if ! command -v "$cmd" &> /dev/null; then
        echo "Error: $cmd is not installed. Please install it."
        exit 1
    fi
done
python3 -c "import bagit" 2>/dev/null || { echo "Error: python bagit module missing (pip install bagit)"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

echo "== 1. Bagging payload"
SRC="$WORK/2025-01-13_Smoke_Test"
mkdir -p "$SRC/sub dir"
head -c 20M /dev/urandom > "$SRC/random.bin"
echo "hello archive" > "$SRC/sub dir/note.txt"
python3 -m bagit --sha256 --sha512 \
    --external-identifier "SMOKE-0001" \
    --bag-group-identifier "SMOKE-SET" \
    --bag-count "1 of 1" \
    "$SRC"

echo "== 2. Building image + RS03 ECC"
"$REPO_ROOT/scripts/create-archive-iso.sh" "$SRC" "$WORK/smoke.iso"
cp smoke.iso damaged.iso

echo "== 3. Simulating damage (500 zeroed sectors)"
dd if=/dev/zero of=damaged.iso bs=2048 seek=2000 count=500 conv=notrunc status=none

echo "== 4. Repairing with dvdisaster (slow: single-threaded in distro 0.79.10 builds)"
# dvdisaster 0.79.10 exits 1 even after a successful repair, so judge the result by comparison instead
dvdisaster -i damaged.iso -f --no-progress < /dev/null || true
cmp smoke.iso damaged.iso
echo "Repaired image is byte-identical to the original."

echo "== 5. Validating bag from repaired image"
7z x -oextracted damaged.iso > /dev/null
python3 -m bagit --validate extracted

echo "SMOKE TEST PASSED"
