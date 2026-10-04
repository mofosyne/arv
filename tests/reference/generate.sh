#!/bin/sh
# Rewrites tests/reference/expected/ from the C arv, after a change in behaviour made on purpose:
# the scenarios' output and files (scenarios.sh), and the recfile writer's output for the
# recfiles in tests/fixtures/recfile/ and home/. Read `git diff tests/reference/expected` before
# committing it with the change: the diff is the record of what changed.
#
# expected/ was first written by the Python arv, the reference while arv was ported to C; every
# file in it was the same from both. Since the Python core was removed, it moves only with a
# reviewed change.
#
#   sh tests/reference/generate.sh
# Needs src/arv built (make -C src/arv).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
arv=$repo/src/arv/build/arv
fixtures=$repo/src/arv/build/fixtures
[ -x "$arv" ] && [ -x "$fixtures" ] || { echo "generate.sh: build src/arv first (make -C src/arv)" >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sh "$here/scenarios.sh" "$tmp/expected" "$arv"
mkdir -p "$tmp/expected/recfile"
for f in "$repo"/tests/fixtures/recfile/*.rec "$here/home/catalog/archive.rec"; do
    name=$(basename "$f")
    [ "$f" = "$here/home/catalog/archive.rec" ] && name=home-archive.rec
    "$fixtures" --roundtrip "$f" "$tmp/expected/recfile/$name"
done
rm -rf "${here:?}/expected"
mv "$tmp/expected" "$here/expected"
echo "wrote $here/expected ($(find "$here/expected" -type f | wc -l) files); now read git diff tests/reference/expected"
