# bagit: BagIt bags checked, in C

Checks a [BagIt](https://www.rfc-editor.org/rfc/rfc8493) bag (RFC 8493), the container every arv
disc is, and holds the digests arv uses. C99 and POSIX, no libraries: one file, `bagit.c`.

```sh
make                       # build/bagit
build/bagit /media/disc    # valid, or every problem with its file and cause
build/bagit --fast BAG     # completeness and Payload-Oxum only, no checksums (like bagit.py --fast)
make check
```

What it checks: `bagit.txt`; that every payload manifest (`manifest-md5`, `-sha1`, `-sha256`,
`-sha512`) names every file in `data/` and no other, and each checksum (every file is read once
for all its algorithms); the tag manifests' files; `Payload-Oxum` in `bag-info.txt`; that every
file `fetch.txt` names is in the manifests and was fetched. Paths in manifests may carry `%0A`,
`%0D` and `%25`.

`arv verify` is this check plus arv's wording; `arv make` uses its SHA-256 and SHA-512.

`check.sh` runs it on twelve good and broken bags and, with python3, has the Library of
Congress's bagit-python (`upstream/bagit-python/`) judge the same bags: the verdicts must agree.
(One difference from bagit-python, by RFC 8493 2.2.3: a file in `fetch.txt` that no payload
manifest names makes the bag invalid; bagit-python lets it pass.)
