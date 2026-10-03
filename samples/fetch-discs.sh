#!/bin/sh
# Download the sample disc images into samples/discs/ from the "samples" release, and check
# them against its SHA256SUMS. (The images are not kept in git: 52 MB that change on every rebuild.)
#
#   samples/fetch-discs.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
url=https://github.com/mofosyne/arv/releases/download/samples
mkdir -p "$here/discs"
cd "$here/discs"
curl -fsSL -o SHA256SUMS "$url/SHA256SUMS"
cut -c67- SHA256SUMS | while read -r name; do
    echo "Downloading $name"
    curl -fsSL -o "$name" "$url/$name"
done
sha256sum -c SHA256SUMS
