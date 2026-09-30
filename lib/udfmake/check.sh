#!/bin/sh
# Build a small BD-ROM UDF 2.50 image with the given udfmake and verify it.
set -eu
udfmake=$1
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
mkdir -p "$t/src/sub dir"
echo hello > "$t/src/a.txt"
printf 'unicode\n' > "$t/src/sub dir/100% ünï.txt"
head -c 3000001 /dev/urandom > "$t/src/big.bin"      # not a multiple of 2048
head -c 5000 /dev/urandom > "$t/src/small.bin"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0} \
    "$udfmake" -o T=bdrom,v=2.50,V=2.50,L=CHECK "$t/check.udf" "$t/src" > "$t/log" 2>&1 || { cat "$t/log"; exit 1; }
python3 - "$t/check.udf" <<'PY'
import re, sys
d = open(sys.argv[1], "rb").read()
revs = {int.from_bytes(d[m.start() + 23:m.start() + 25], "little") for m in re.finditer(rb"\*OSTA UDF Compliant", d)}
assert revs == {0x250}, revs
assert b"*UDF Metadata Partition" in d, "no metadata partition"
print("check: UDF 2.50 with a metadata partition")
PY
if command -v 7z > /dev/null; then
    7z x -o"$t/out" "$t/check.udf" > /dev/null
    diff -r "$t/src" "$t/out" && echo "check: 7-Zip reads back identical files"
fi
