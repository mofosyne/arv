# bagit-python: the Library of Congress's BagIt tool

A copy of [bagit-python](https://github.com/LibraryOfCongress/bagit-python) 1.9.0, public domain
(CC0). Only change: a `VERSION` fallback at the top, so it runs without pip.

arv no longer puts it on discs: every disc's `tools/arv.com` (or the arv built from `tools/`)
validates the bag itself, through [`src/bagit`](../../src/bagit/), and `sha256sum -c` needs no
software of ours at all. It stays here as an outside referee: `src/bagit/check.sh` runs it, when
`python3` is there, on the same good and broken bags as arv's validator, and the two must agree.
