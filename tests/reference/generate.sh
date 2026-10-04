#!/bin/sh
# Writes tests/reference/expected/ from the Python arv, the reference: the scenarios'
# output and files (scenarios.sh), and the recfile writer's output for the recfiles in
# tests/fixtures/recfile/ and home/. Run it after a change in behaviour, read
# `git diff tests/reference/expected` and commit it with the change; src/arvc/check.sh then
# holds arvc to it.
#
#   sh tests/reference/generate.sh
# Needs python3, udfwrite (make -C src/udfwrite) and src/arvc/build/ptyrun (make -C src/arvc).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
PATH=$repo/src/udfwrite/build:$PATH
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sh "$here/scenarios.sh" "$tmp/expected" python3 "$repo/arv"
mkdir -p "$tmp/expected/recfile"
for f in "$repo"/tests/fixtures/recfile/*.rec "$here/home/catalog/archive.rec"; do
    name=$(basename "$f")
    [ "$f" = "$here/home/catalog/archive.rec" ] && name=home-archive.rec
    python3 -c "import sys; sys.path.insert(0, '$repo/src'); from arv import recfile; sys.stdout.write(recfile.dumps(recfile.read(sys.argv[1])))" \
        "$f" > "$tmp/expected/recfile/$name"
done
rm -rf "${here:?}/expected"
mv "$tmp/expected" "$here/expected"
echo "wrote $here/expected ($(find "$here/expected" -type f | wc -l) files)"
