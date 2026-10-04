#!/bin/sh
# Checks bagit: good bags are valid and every kind of damage is found, the digests agree with
# md5sum, sha1sum, sha256sum and sha512sum, and (with python3) the Library of Congress's
# bagit.py (upstream/bagit-python) gives the same verdict on every bag.
#
#   sh check.sh build/bagit
set -eu
tool=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
here=$(cd "$(dirname "$0")" && pwd)
referee=$here/../../upstream/bagit-python/bagit.py
dir=$here/build/check
rm -rf "$dir"
mkdir -p "$dir"
cd "$dir"
ok() { echo "ok: $*"; }
no() { echo "FAILED: $*"; exit 1; }

# make NAME: a valid bag with all four algorithms, a tag manifest and Payload-Oxum
make_bag() {
    mkdir -p "$1/data/sub dir"
    printf hello > "$1/data/a.txt"
    head -c 70000 /dev/urandom > "$1/data/sub dir/b.bin"
    : > "$1/data/empty"
    (cd "$1"
     printf 'BagIt-Version: 1.0\nTag-File-Character-Encoding: UTF-8\n' > bagit.txt
     for a in md5 sha1 sha256 sha512; do find data -type f | sort | while read -r f; do ${a}sum "$f"; done > manifest-$a.txt; done
     printf 'Payload-Oxum: %s.3\n' $((5 + 70000)) > bag-info.txt
     sha256sum bagit.txt bag-info.txt manifest-*.txt > tagmanifest-sha256.txt)
}
n=0
verdict() {      # verdict NAME valid|invalid [--fast]: ours, and the referee's when there is one
    name=$1 want=$2
    shift 2
    if "$tool" "$@" "$name" > "$name.out"; then got=valid; else got=invalid; fi
    [ "$got" = "$want" ] || { cat "$name.out"; no "$name: bagit says $got, should be $want"; }
    if command -v python3 >/dev/null; then
        if python3 "$referee" --validate "$@" "$name" >/dev/null 2>&1; then ref=valid; else ref=invalid; fi
        [ "$ref" = "$want" ] || no "$name: bagit.py says $ref, bagit says $want"
    fi
    n=$((n + 1))
}

make_bag good;      verdict good valid
make_bag changed;   printf X >> changed/data/a.txt;                       verdict changed invalid
make_bag samesize;  printf J | dd of=samesize/data/a.txt bs=1 seek=0 conv=notrunc 2>/dev/null
                    verdict samesize invalid;                              verdict samesize valid --fast
make_bag extra;     printf x > extra/data/new.txt;                         verdict extra invalid
make_bag missing;   rm "missing/data/sub dir/b.bin";                      verdict missing invalid
make_bag partial;   sed -i '/a.txt/d' partial/manifest-sha1.txt;          verdict partial invalid
make_bag tags;      printf 'X' >> tags/bag-info.txt;                       verdict tags invalid
make_bag oxum;      sed -i 's/^Payload-Oxum: .*/Payload-Oxum: 1.3/' oxum/bag-info.txt
                    (cd oxum && sha256sum bagit.txt bag-info.txt manifest-*.txt > tagmanifest-sha256.txt)
                    verdict oxum invalid
make_bag nobagit;   rm nobagit/bagit.txt;                                  verdict nobagit invalid
make_bag fetch                                      # a file fetch.txt names (and the manifests list), not fetched
(cd fetch && printf 'https://example.invalid/c.txt 3 data/c.txt\n' > fetch.txt
 for a in md5 sha1 sha256 sha512; do printf '%s  data/c.txt\n' "$(printf abc | ${a}sum | cut -d' ' -f1)" >> manifest-$a.txt; done
 sed -i '/^Payload-Oxum/d' bag-info.txt
 sha256sum bagit.txt bag-info.txt manifest-*.txt > tagmanifest-sha256.txt)
verdict fetch invalid
make_bag escaped                                    # a name with a line break, encoded as %0A
printf 'line\nbreak' > "escaped/data/two
lines.txt"
(cd escaped && for a in md5 sha1 sha256 sha512; do
     printf '%s  data/two%%0Alines.txt\n' "$(${a}sum < "data/two
lines.txt" | cut -d' ' -f1)" >> manifest-$a.txt; done
 sed -i 's/^Payload-Oxum: .*/Payload-Oxum: 70015.4/' bag-info.txt
 sha256sum bagit.txt bag-info.txt manifest-*.txt > tagmanifest-sha256.txt)
verdict escaped valid
ok "$n verdicts on good and broken bags (changed, same-size change, extra, missing, a file one manifest"
echo "    leaves out, a tag file, Payload-Oxum, no bagit.txt, fetch.txt, an escaped name; --fast)$(command -v python3 >/dev/null && echo ", each the same as bagit.py's")"

grep -q "^FAILED   data/a.txt (md5 sha1 sha256 sha512)" changed.out && grep -q "^EXTRA    data/new.txt" extra.out \
    && grep -q "^MISSING  data/sub dir/b.bin" missing.out && grep -q "^EXTRA    data/a.txt (not in manifest-sha1.txt)" partial.out \
    && grep -q "^INVALID  bag-info.txt (Payload-Oxum is 1.3" oxum.out && grep -q "^MISSING  data/c.txt" fetch.out \
    && ok "each problem named with its file and cause" || no "the reports"
