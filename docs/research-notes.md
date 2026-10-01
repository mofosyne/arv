# Research notes

Findings behind the design choices in this repo. Items marked **verified** were
reproduced in a Linux container (Ubuntu 24.04, udftools, genisoimage,
dvdisaster 0.79.10, bagit-python 1.9.0).

## 1. UDF revision: "latest" is not automatically "most robust"

| Revision | What it adds | Linux kernel | Linux image tooling |
|---|---|---|---|
| 1.02 | Baseline, DVD-Video | read/write | `genisoimage -udf` (ISO9660 bridge) |
| 2.01 | Large files, current default | **read/write** | `mkudffs` + loop mount + `cp` |
| 2.50 | **Metadata partition, optional metadata mirror** (duplicate of the directory/inode data); BD-ROM uses it | read-only | **`lib/udfmake`** (NetBSD makefs, userspace; since 2026-09; no metadata mirror yet) |
| 2.60 | Pseudo-OverWrite for BD-R drives | read-only | none (makefs caps at 2.50: `udf.c:151`, "0x260 is not ready"); not needed for single-pass images |

- The only feature past 2.01 that helps *robustness* is the 2.50 **metadata
  mirror**. 2.60 only adds a recording method for drives, which does nothing for
  an image written once.
- With dvdisaster RS03 the ECC works on raw sectors *under* the filesystem, so
  it repairs damaged metadata the same way it repairs file data. That makes the
  metadata mirror a second line of defence rather than the main one.
- **Verified:** `mkudffs` refuses `--udfrev` above 2.01 for every media type
  except `bdr`, and for `bdr` it silently caps at **2.50** and writes an empty
  *write-once/VAT* layout. With no VAT written yet, `udfinfo` reports "Virtual
  Allocation Table not found". That explains the old
  `create-archive-udf.sh` mount failure ("bad superblock").
- The Linux kernel UDF driver mounts 2.50/2.60 read-only, so "format, then
  mount and copy" can never produce a 2.50+ image on Linux.
- Ways to get real UDF 2.50/2.60 images:
  - NetBSD `makefs -t udf` (options `disctype=bdrom|bdr|...`, `minver`/`maxver`,
    `metaperc`). This is the only open-source "directory → UDF 2.50/2.60 image"
    builder I found. **Ported (image builder only):** `lib/udfmake/` is a C
    library and self-contained program (C library only; `make static` for one
    binary) built from the NetBSD files plus a small compat layer, with two upstream memory bugs fixed. Draft upstream report and
    reproduction: `third_party/netbsd-makefs-udf/`.
    `T=bdrom` gives UDF 2.50 with a metadata partition (7-Zip reads it back
    identical). Missing upstream: the metadata *mirror* is not duplicated.
  - Windows: ImgBurn / IMAPI2 build UDF 2.50/2.60 images.
  - Not UDFclient (Debian `udfclient` 0.8.11, the userspace predecessor of
    NetBSD's UDF code, by the same author). Tested 2026-09: `newfs_udf` has no
    revision option and writes **UDF 2.01** without a metadata partition, and
    `udfclient -W` could not add a file or folder to a fresh 2048-byte-sector
    image ("couldn't add new file entry ... Invalid argument"). Useful for
    inspecting images (`udfdump`), not for building 2.50 ones.
- **Recommendation:** default to an ISO9660 (RR + Joliet) + UDF bridge for
  compatibility (the current ISO script), or plain UDF 2.01. Treat 2.50 with a
  metadata mirror as an optional stretch goal through NetBSD `makefs`.

### File name limits (measured 2026-09-30)

What each directory tree on our discs keeps of a file name. The BagIt
manifests always keep the exact names, and `archive names FOLDER` reports the
issues for a folder.

| Tree | Read by | Longest name | Changed |
|---|---|---|---|
| ISO 9660 level 3 | nothing modern prefers it | 31 characters | nearly everything; ignored in practice |
| Rock Ridge | Linux, BSDs | 255 bytes (as the source) | nothing |
| Joliet (`-joliet-long`) | Windows, macOS | 103 characters | `* : ; ? \` become `_`; the name ends at the first character beyond U+FFFF |
| UDF 1.02 (genisoimage) | Windows, macOS | 103 characters | as Joliet |
| UDF 2.50 (udfmake) | Windows Vista+, macOS, Linux | 254 characters, or 127 with any above U+00FF | nothing beyond U+FFFF can be stored (`archive make` refuses) |

genisoimage needs `-input-charset utf-8`. Without it, in a C/POSIX locale,
every non-ASCII name was garbled in the Joliet and UDF trees; this was fixed
2026-09-30. Windows also shows names with `< > : " \ | ? *` changed, and
of names that differ only in case, it shows only one.

## 2. dvdisaster

- **Not a library.** It is a single C program (GPLv3) with a GTK GUI and a
  CLI. The maintained fork is
  [speed47/dvdisaster](https://github.com/speed47/dvdisaster). It adds a
  GTK-free CLI build (`./configure --with-gui=no`), multithreaded RS03,
  `--no-bdr-defect-management` (more ECC space on BD-R), and recovery of RS03
  images when the `-n` value used at creation is unknown.
- Integration options, from least to most work:
  1. Call the CLI as a subprocess (what the scripts do).
  2. Build the speed47 source into a static library behind a thin C API. This
     is feasible because the encoder/decoder code is fairly separate from the
     GUI, but it is a fork to maintain.
  3. Reimplement RS03 from its documented format. This is risky: any mismatch
     breaks compatibility with real dvdisaster, which is the property you want
     to keep.
- Self-describing: an RS03 **augmented image** keeps the ECC after the
  filesystem and replicates its headers, so a stock dvdisaster finds and uses
  it with no side files. The disc still mounts normally in any OS.
- **Verified:**
  - An RS03 augment of a 30 MB ISO repaired 500 zeroed sectors back to a
    byte-identical image.
  - RS03 also accepts a pure `mkudffs` UDF 2.01 image, so no ISO9660 bridge is
    needed.
  - With no size or `-n` option, RS03 pads the image to the **smallest standard
    medium that fits** (here CD, 700 MiB). Real BD-sized payloads round up to
    BD 25/50/100 GB.
  - **dvdisaster 0.79.10 exits with status 1 even after a successful `-f`
    repair**, so scripts must check the result another way (`-t`, `cmp`, or
    bag validation).
  - The distro build is single-threaded and slow: about 3.5 min to verify or
    repair a 700 MiB image. Use speed47 for BD-sized work.
- For a self-contained disc, add a `tools/` or `RECOVERY/` directory with the
  dvdisaster source tarball, static binaries and plain-text instructions. This
  helps as long as the disc can be read at all. Keep a copy off-disc as well.

## 3. BagIt (RFC 8493) for a *personal* archive

For photos, video and source code on your own discs, most of BagIt is optional.
The part that matters costs almost nothing:

- `manifest-sha256.txt` uses the same `<hash>  <path>` format as coreutils, so
  in 30 years `cd disc && sha256sum -c manifest-sha256.txt` still verifies it
  without any BagIt tooling.
- `bagit.txt` at the disc root is a well-known signature. Archivematica, DSpace
  and similar tools recognise a bag if you ever hand the discs to an
  institution.
- `bag-info.txt` gives `Bag-Group-Identifier` + `Bag-Count` ("3 of 7") to tie a
  disc set together, and `Payload-Oxum` for a fast completeness check.

Costs:
- The payload moves under `data/`, which adds one extra folder when browsing.
- Adding a file means re-bagging, but a write-once disc is never updated anyway.

**Verdict:** keep BagIt as the thin fixity and self-description layer, and do
not rely on it for cataloguing. It is a packaging format, not an index.

## 4. recfiles (GNU recutils) as the catalogue

recfiles fit the "human-readable in 50 years" goal better than a database or
JSON-LD, and they are the natural home for descriptive metadata and the
cross-disc index:

- **Per disc:** `catalog.rec` as a BagIt *tag file* at the bag root, so the
  tagmanifest checksums it. It holds one `Disc` record plus optional `File`
  records with fields such as size, sha256, mtime, MIME type and
  EXIF/video metadata.
- **At home:** a master `archive.rec` that concatenates every disc's
  `catalog.rec`. `recsel -t File -e "Path ~ 'IMG_2019'" -p Disc,Path archive.rec`
  answers "which disc is that photo on?" without loading any discs.
- `%rec:` / `%type:` / `%key:` descriptors plus `recfix` give schema validation.

Draft schema:

```rec
%rec: Disc
%key: Id
%type: Created date
%type: Count int
%mandatory: Id Label Created

Id: 2025-01-13_Projects_2020_-_2025
Label: Projects 2020 - 2025
Created: 2025-01-13
Set: PROJECTS-2025
Index: 1
Count: 3
Filesystem: ISO9660+RR+Joliet / UDF 1.02
Ecc: dvdisaster RS03 augmented
Media: BD-R 25GB

%rec: File
%key: Path
%type: Size int
%mandatory: Disc Path Sha256

Disc: 2025-01-13_Projects_2020_-_2025
Path: data/photos/2020/IMG_0001.JPG
Size: 4839201
Sha256: 9f86d0...
```

## Sources

- [NetBSD makefs(8)](https://man.netbsd.org/NetBSD-9.x-BRANCH/i386/makefs.8)
- [NetBSD makefs udf.c](https://github.com/NetBSD/src/blob/trunk/usr.sbin/makefs/udf.c)
- [NetBSD newfs_udf(8)](http://ftp.netbsd.org/pub/NetBSD/NetBSD-current/src/sbin/newfs_udf/newfs_udf.8)
- [mkudffs(8), Debian](https://manpages.debian.org/testing/udftools/mkudffs.8.en.html)
- [Universal Disk Format, Wikipedia](https://en.wikipedia.org/wiki/Universal_Disk_Format)
- [speed47/dvdisaster](https://github.com/speed47/dvdisaster), [CHANGELOG](https://github.com/speed47/dvdisaster/blob/master/CHANGELOG)
- RFC 8493, The BagIt File Packaging Format (V1.0)

## 5. Small models for tagging: embeddings vs. SemIf-style decisions (2026-09-29)

Question: can a small (ideally sub-100 MB) local model tag folders reliably from
a fixed tag list? Tested on 8 folders (names, file names, README text and, for
three opaquely named folders, image captions) against 11 tags, on 4 CPU cores
with llama.cpp `llama-server`.

**SemIf** ([openjev.com](https://openjev.com/), formerly OpenJev,
[TheoLeeCJ/SemIf-OpenJev](https://github.com/TheoLeeCJ/SemIf-OpenJev), MIT) is an
open take on TypeSafe's hosted Jev: ask a question with lettered options, run
one forward pass, and read the option letters' probabilities (softmax over
their logits) instead of generating JSON. It cannot produce malformed output
and is several times faster than generation. The reference results use
Qwen3.5-4B (balanced accuracy 0.813). The browser demo runs Qwen3-0.6B /
MiniCPM5-2B / Qwen3.5-4B through wllama (llama.cpp in WebAssembly), which needs
an HTTP origin and so cannot run from a disc's `file://` pages.
llama.cpp's OpenAI endpoint exposes the needed `logprobs` / `top_logprobs`, so
the technique works with this project's existing standard-library client.

| Model | Method | File size | Top tag correct | Time per folder |
|---|---|---|---|---|
| SmolLM2-135M-Instruct Q4_K_M | JSON generation | 105 MB | failed (invalid output) | ~2 min |
| SmolLM2-135M-Instruct Q4_K_M | SemIf readout | 105 MB | ~chance (letter bias) | 0.3 s |
| **bge-small-en-v1.5 Q8** | **embedding similarity** | **37 MB** | **7/8** (Christmas -> travel) | ~0.02 s |
| Qwen3-0.6B Q4_K_M (thinking off) | SemIf readout | 397 MB | 5/8 | 0.8 s |
| **Qwen2.5-1.5B-Instruct Q4_K_M** | **SemIf readout** | **1.1 GB** | **8/8** | 1.5 s |

Findings:
- Sub-100 MB *generative* models are not usable here, even with forced choice.
- A 37 MB *embedding* model is the best small option: good enough to suggest
  tags from a fixed vocabulary for review. Its scores are close together
  (the top 3 are often within 0.1), so rank rather than threshold.
- SemIf-style readout with a ~1.5B model was the most accurate, but its raw
  probabilities are over-confident (mostly 1.00); SemIf itself applies
  per-workload temperature scaling for calibration.
- For multi-label tagging, SemIf's single choice gives a ranking; per-tag
  yes/no decisions would give independent labels at N passes per folder.

Implications: a fixed, editable tag vocabulary (consistent across discs) scored
by embeddings by default, with optional SemIf-style readout when a >= 1.5B model
is available; both over OpenAI-compatible HTTP (`/v1/embeddings`,
`/v1/chat/completions` with `logprobs`). Model weights stay out of the repo (a
pinned download or `--extra-tools` per disc), because every disc carries a
snapshot of the repository (and its full history with `--tools-history`).


## 6. Catalogue size and the browser search data (measured 2026-10-01)

Every disc carries the catalogue of all earlier discs (`--snapshot full`). Per
file in the archive, each later disc carries this (synthetic 50,000-file disc
with realistic photo, code and document paths):

| Per file, per snapshot copy | Plain | gzip -9 | xz |
|---|---|---|---|
| `formats/*.csv` (only with Siegfried) | 249 bytes | 58 (23%) | 48 |
| `manifests/*.sha256` (BagIt) | 112 | 50 (44%; hex checksums barely compress) | 42 |
| `listings/*.tsv` | 70 | 14 (20%) | 9 |
| `web/files/*.js` (search.html) | 51 | 13 | 9 |
| **total** | **483** | **about 135** | |

| Files in the whole archive | Snapshot on each new disc | Share of a 25 GB disc's data (~18.6 GB at 20% RS03) |
|---|---|---|
| 100,000 | 0.05 GB | 0.3% |
| 1,000,000 | 0.48 GB | 2.6% |
| 5,000,000 | 2.4 GB | 13% |
| 20,000,000 | 9.7 GB | 52% |

A 25 GB personal disc holds roughly 5,000-50,000 files, so a million files means
about 20-200 discs. Up to that point the snapshot is under 3% and stays plain.
What to do beyond that is in plan.md ("Later: catalogue snapshot size").

### Search data in the browser (measured in Chromium, 1,000,000 files)

| Form of a file list | Size | Load | First search | Later searches |
|---|---|---|---|---|
| **in use:** one JS string of TSV rows | 47.6 MB | 875 ms | 196 ms (one split, then cached) | about 35 ms |
| JSON arrays `[[size, path], ...]` | 49.6 MB | 1,176 ms | 33 ms | 33 ms |
| JSON objects `[{size, path}, ...]` | 63.6 MB | 2,358 ms | 34 ms | 34 ms |

JSON brings no gain. The archival files stay recfiles and TSV: line-oriented
(grep, diff, sort, `sha256sum -c`; damage loses lines, not the whole file),
commented, readable and editable by hand, and easy to append to.

### Why search.html loads `.js` files, and what was not adopted

A page opened from `file://` has a `null` origin. Browsers then **block**
`fetch`/`XMLHttpRequest` of `.rec`, `.tsv`, `.json` and `.wasm` files, and any
`<script type="module">`. They **allow** classic `<script src>` (the pre-CORS rule)
and files the user picks or drops (`FileReader`). So `archive make` derives small
`.js` views (`discs.js`, `files/<id>.js`) from the catalogue, like the SQLite
index. The recfiles and TSV themselves are never wrapped.

- **recutils-js** (npm 0.1.0, 2025-08, GPL-3.0; WebAssembly build of GNU
  recutils): not adopted. It ships as ES modules loading separate `.wasm` files,
  which Chromium blocks on `file://` (tested: "blocked by CORS policy"). Making it
  work needs a bundling step, and adds about 270 KB of WebAssembly per tool, plus
  its JavaScript wrapper, to every disc. It would only cover the disc records,
  because the file lists are TSV. It could suit `archive gui`, which is served
  over http.
- **Wrapping recfiles in `.js`** so the page can parse them: rejected. The
  recfiles stay exactly as they are.
- **A JavaScript recfile parser reading picked or dropped files** (`FileReader`,
  no wrapping): possible later, for catalogues of other or older discs. It would
  be tested against `tests/fixtures/recfile/`.
