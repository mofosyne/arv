# Research notes

Findings behind the design choices in this repo. Items marked **verified** were
reproduced in a Linux container (Ubuntu 24.04, udftools, genisoimage,
dvdisaster 0.79.10, bagit-python 1.9.0).

## 1. UDF revision: "latest" is not automatically "most robust"

| Revision | What it adds | Linux kernel | Linux image tooling |
|---|---|---|---|
| 1.02 | Baseline, DVD-Video | read/write | `genisoimage -udf` (ISO9660 bridge) |
| 2.01 | Large files, current default | **read/write** | `mkudffs` + loop mount + `cp` |
| 2.50 | **Metadata partition, optional metadata mirror** (duplicate of the directory/inode data); BD-ROM uses it | read-only | **`src/udfmake`** (NetBSD makefs, userspace; since 2026-09; no metadata mirror yet) |
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
    builder I found. **Ported (image builder only):** `src/udfmake/` is a C
    library and self-contained program (C library only; `make static` for one
    binary) built from the NetBSD files plus a small compat layer, with two upstream memory bugs fixed. Draft upstream report and
    reproduction: `src/udfmake/upstream/`.
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
manifests always keep the exact names, and `arv names FOLDER` reports the
issues for a folder.

| Tree | Read by | Longest name | Changed |
|---|---|---|---|
| ISO 9660 level 3 | nothing modern prefers it | 31 characters | nearly everything; ignored in practice |
| Rock Ridge | Linux, BSDs | 255 bytes (as the source) | nothing |
| Joliet (`-joliet-long`) | Windows, macOS | 103 characters | `* : ; ? \` become `_`; the name ends at the first character beyond U+FFFF |
| UDF 1.02 (genisoimage) | Windows, macOS | 103 characters | as Joliet |
| UDF 2.50 (udfmake) | Windows Vista+, macOS, Linux | 254 characters, or 127 with any above U+00FF | nothing beyond U+FFFF can be stored (`arv make` refuses) |

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
| ~~`web/files/*.js` (search.html)~~ | ~~51~~ | ~~13~~ | removed 2026-10-01 |
| **total** | **431** | **about 122** | |

| Files in the whole archive | Snapshot on each new disc | Share of a 25 GB disc's data (~18.6 GB at 20% RS03) |
|---|---|---|
| 100,000 | 0.04 GB | 0.2% |
| 1,000,000 | 0.43 GB | 2.3% |
| 5,000,000 | 2.2 GB | 12% |
| 20,000,000 | 8.6 GB | 46% |

A 25 GB personal disc holds roughly 5,000-50,000 files, so a million files means
about 20-200 discs. Up to that point the snapshot is under 3% and stays plain.
What to do beyond that is in plan.md ("Later: catalogue snapshot size").

### Search data in the browser (measured in Chromium, 1,000,000 files)

(Kept for the record: `search.html` was removed on 2026-10-01; see plan.md, Decisions.)

| Form of a file list | Size | Load | First search | Later searches |
|---|---|---|---|---|
| **in use:** one JS string of TSV rows | 47.6 MB | 875 ms | 196 ms (one split, then cached) | about 35 ms |
| JSON arrays `[[size, path], ...]` | 49.6 MB | 1,176 ms | 33 ms | 33 ms |
| JSON objects `[{size, path}, ...]` | 63.6 MB | 2,358 ms | 34 ms | 34 ms |

JSON brings no gain. The archival files stay recfiles and TSV: line-oriented
(grep, diff, sort, `sha256sum -c`; damage loses lines, not the whole file),
commented, readable and editable by hand, and easy to append to.

### Why search.html loaded `.js` files, and what was not adopted

(Historical: `search.html` was removed on 2026-10-01.)

A page opened from `file://` has a `null` origin. Browsers then **block**
`fetch`/`XMLHttpRequest` of `.rec`, `.tsv`, `.json` and `.wasm` files, and any
`<script type="module">`. They **allow** classic `<script src>` (the pre-CORS rule)
and files the user picks or drops (`FileReader`). So `arv make` derives small
`.js` views (`discs.js`, `files/<id>.js`) from the catalogue, like the SQLite
index. The recfiles and TSV themselves are never wrapped.

- **recutils-js** (npm 0.1.0, 2025-08, GPL-3.0; WebAssembly build of GNU
  recutils): not adopted. It ships as ES modules loading separate `.wasm` files,
  which Chromium blocks on `file://` (tested: "blocked by CORS policy"). Making it
  work needs a bundling step, and adds about 270 KB of WebAssembly per tool, plus
  its JavaScript wrapper, to every disc. It would only cover the disc records,
  because the file lists are TSV. It could suit `arv gui`, which is served
  over http.
- **Wrapping recfiles in `.js`** so the page can parse them: rejected. The
  recfiles stay exactly as they are.
- **A JavaScript recfile parser reading picked or dropped files** (`FileReader`,
  no wrapping): possible later, for catalogues of other or older discs. It would
  be tested against `tests/fixtures/recfile/`.

## 7. Related projects (searched 2026-10-01)

Searched GitHub for "bluray archive", "m-disc archive", "blu-ray backup par2", the
`optical-disc` topic and "dvdisaster". Packing, adding error correction and burning is a crowded
field. None of these keeps a catalogue of what is on each disc and where the disc is kept, with
plain files and a published format, which is what this project does.

| Project | What it does | Error correction | Notes for us |
|---|---|---|---|
| [mikmorg/lcsas](https://github.com/mikmorg/lcsas) (Python + C, MIT stated) | Deduplicated, encrypted backup packs (Rustic) on BD-R/M-DISC; a full SQLite catalogue on every disc; a meta-volume of tools and source | dvdisaster RS03 augmented images, like ours | Closest in design. Its RS03 spec and C decoder are tested in section 8; other ideas in section 9 |
| [teaching-droid/dvdisaster-light](https://github.com/teaching-droid/dvdisaster-light) (C, GPLv3) | dvdisaster fork: RS03 only, CLI only; faster encoders (AVX2, optional GPU); damaged-disc reading (`--rescue`, `--mapfile`, `--retry`, `-R`) | RS03, bit-identical to 0.79.10-pl6 | Tested in section 8 |
| [jzbz/brb](https://github.com/jzbz/brb) (Go + bash) | Per disc: SquashFS, then age encryption, then PAR2, then ISO; every disc independent; encrypted path index | PAR2 over the ciphertext | A Go writer plus a small bash reader you can audit, held to identical answers by a test |
| [greenseeing/ovenmitts](https://github.com/greenseeing/ovenmitts) (Rust) | Burns large files (VeraCrypt containers) with xorriso; each disc documents its own recovery | PAR2, optional dvdisaster RS02 | Burn notes: growisofs pre-formats blank BD-R (unusable to other tools), Brasero's 4 GiB limit, verification must bypass the page cache; VeraCrypt hygiene (plan.md) |
| [Xitee1/bd-archiver](https://github.com/Xitee1/bd-archiver) (Python) | Plain-file discs or DAR archives across discs; SHA-512; burn with verify and resume | PAR2 | Exact fit check without writing the image |
| [nathansottung/obelisk](https://github.com/nathansottung/obelisk) (Go) | Backup to tape, drives, discs, cloud, re-verified on a schedule | PAR2; optional dvdisaster RS02/RS03 layer | |
| [kurmann/disc-archiver](https://github.com/kurmann/disc-archiver) (Python) | PAR2 archives; spec plans a SQLite index of volumes | PAR2 | Index not built yet |
| [hammad93/crypto-disco](https://github.com/hammad93/crypto-disco) (Python) | GUI image builder | per-file Reed-Solomon (reedsolo) | |
| [salfter/bdarchiver](https://github.com/salfter/bdarchiver) (shell, 2015) | Archives a media server to BD-R | dvdisaster | |

Smaller ones: daneubauer/immich-go-disc-archive (bash, PAR2 and manifests),
ambauma/BdArchivePlanner, llawsxx/DiscHelper and volumespan-py (splitting files across discs),
rbuchberger/bdar (shelved before it worked).

### restic/rustic and git-annex: two opposite models

LCSAS stores its data with **rustic** ([rustic-rs/rustic](https://github.com/rustic-rs/rustic),
docs at <https://rustic.cli.rs>), a Rust reimplementation of
**restic** ([restic/restic](https://github.com/restic/restic)) using the same repository format.
**git-annex** (<https://git-annex.branchable.com>) is often mentioned alongside, but works the
other way round:

| | restic / rustic | git-annex | This project |
|---|---|---|---|
| What it is | Backup program | File tracking on top of git | Archive discs + catalogue |
| How files are stored | Cut into variable-size chunks, deduplicated, encrypted (AES-256-CTR + Poly1305), packed into opaque pack files | Whole files, as they are (encrypted only on some remotes) | Whole files, as they are (BagIt) |
| Getting a file back | rustic/restic plus the password, and every disc holding its chunks | Copy it | Copy it; any OS |
| One disc on its own | Not useful | Readable | Readable, with the catalogue of every earlier disc |
| History | Snapshots; each backup adds only new chunks | Git history of where each file was | Events per disc (made, burned, checked, moved) |
| Where copies are | One repository (LCSAS spreads its packs over discs) | Tracks which "remotes" (drives, servers, offline discs) hold each file; enforces a minimum number of copies | Locations per disc, copy counts |
| Space | Dedup across everything | No dedup beyond identical files | None; a file is on the discs it was archived to |

restic/rustic optimises for space and confidentiality and pays in dependence on the tool
and the key: LCSAS offsets that by carrying the restic format spec and a pure-Python restore
fallback on every disc. git-annex is the closer relative to this project: its tracking of
where each file's content is and how many copies exist overlaps with our catalogue's
locations and copy counts, though per file rather than per disc, and inside a git repository
rather than on the discs themselves. Not adopted: dedup and opaque packs go against "any disc,
read on its own, with any OS"; git-annex's per-file tracking is more than a disc-level
archive needs, and needs git to read.

### Self-describing media: Piql AFS and LTFS (2026-10-02)

Two established formats share the idea that the medium must explain itself. Neither replaces
this project, but both confirm the approach and are design references.

**Piql AFS** ([github.com/piql/afs](https://github.com/piql/afs), C99, GPLv3; commit
`290567c`, 2026-01). The Archival File System used on piqlFilm: digital data written as 2D
barcode frames on photosensitive film, read by scanning. Its stated goal is "a completely self
contained file system, containing all information needed to decode and understand its content
in the near and distant future"; each reel also carries human-readable decoding instructions
([IS&T overview](https://library.imaging.org/jist/articles/69/2/020402)).

| Piql AFS (film) | This project (Blu-ray) |
|---|---|
| Control frame: a self-contained bootstrap for decoding the rest of the reel | `README.txt`, `catalog.rec` and `tools/` on every disc |
| Visual (human-readable) frames next to digital ones | `index.html`, the plain README and file listings; nothing readable without a computer yet |
| Table of contents with file format ids, checksums, dates, metadata "in any format" (XML: `tocdata.xsd`) | BagIt manifests, PRONOM ids (Siegfried), recfile catalogue |
| The TOC knows about several reels (`ReelsType`); control frame has Title, Creator, Description, Location, ReelId | Catalogue of every earlier disc; Disc fields; Location tree |
| iVM: "a minimal execution environment for file format decoders", so future implementers need little effort | `.wasm` builds of the tools on every disc (WASI) |

Not adoptable as our format: it is built for scanned film frames, while a Blu-ray already has a
filesystem every computer reads; it serves an institutional film service; it has no curation
(appraisal, access levels) or history between volumes. Worth borrowing: a *visual* layer that
needs no computer at all; for us a printable sheet (README and the disc's index as a PDF) kept
with the disc or in the box.

**LTFS** (Linear Tape File System; SNIA standard, also ISO/IEC 20919;
[overview](https://en.wikipedia.org/wiki/Linear_Tape_File_System),
[IBM documentation](https://www.ibm.com/docs/en/storage-archive-le/2.4.6?topic=overview-ltfs-format)).
Each tape is split into an **index partition** and a **data partition**. The index is an XML
file listing every file and where its blocks are, so any LTFS-capable system mounts the tape
like a disk with no external database. Each new index records a generation number and where the
previous index is, so earlier states of the tape can be recovered. A library of many tapes is
tracked by separate software (LTFS LE/EE and others), not by the tapes.

| LTFS (tape) | This project (Blu-ray) |
|---|---|
| Index partition: this volume's index | at the root of each disc: `catalog.rec` + `manifest-sha256.txt` |
| Data partition | `data/` (the BagIt payload) |
| Index generations, each pointing to the previous one | discs as nodes, each recording the previous ones (plan.md, history graph) |
| Library software tracking all tapes | `catalog/` on every disc: every volume's index travels with each volume |

The last row is the difference: LTFS makes each tape self-describing but leaves the *library*
to external software; here every disc carries the whole library's index too.

**Consequences for the design** (plan.md):
- lay out the per-volume index files by volume (`volumes/<disc-id>/`), as LTFS keeps one index
  per tape, identically on discs and in `.arv/catalog/`;
- keep a printable, computer-free summary of each disc (Piql's visual layer);
- read Piql's control-frame fields and iVM design before writing the multi-disc catalogue spec
  and the WebAssembly runtime plan.

#### Piql in more detail: what is open, and the lessons we take

Looked at 2026-10-02: piql/afs (`290567c`), piql/unbox, piql/unboxing and immortalvm/boxing
(boxinglib), immortalvm/ivm-doc and an iVM emulator.

**Open reader, closed writer.**

| Piece | Public? |
|---|---|
| Decoding the 2D barcode frames from scanned film (boxinglib: "a library for **decoding** high-capacity 2D barcode images"; the source has an unboxer, no boxer) | yes, GPLv3 |
| Error-correction codecs (Reed-Solomon, LDPC, interleaving), both directions | yes, as building blocks only |
| Rendering data into film frames for the film writer | not found in any public Piql or immortalvm repository |
| AFS table of contents and control data, read *and* write (`afs_toc_data_save_file` and similar) | yes, GPLv3 |
| iVM: documentation, ISA, emulators, a C compiler (ivm64), Coq specification | yes (licences of the immortalvm repositories not checked) |

Anyone can read a reel forever, but making one goes through Piql's service and equipment. A
fair business model, and the reader is what the future needs, but creation depends on one
vendor. **This project is open on both ends:** anyone can make a disc as well as read one, with
ordinary burners and open tools (udfmake, dvdisaster, BagIt, this tool), and every disc carries
the source of the tools that made it.

**AFS keeps files untouched.** Each file in the table of contents is one contiguous byte run:

| AFS file entry (`tocdatafile.xsd`) | Here |
|---|---|
| `id`, `uniqueId`, `name`, `parentId` (folder tree) | path in the listing; disc UUID + path |
| `date`, `size` | listing |
| `checksum` (SHA-1 by default) | SHA-256 in the BagIt manifest |
| `formatId` | PRONOM id (Siegfried) |
| `metadata` sources, in any format or pointing to another file | tags, collections, catalogue records |
| `start` / `end` as (frame, byte) | **not recorded yet**: where the file sits on the medium |

**iVM** (Immortal Virtual Machine; [Piql's page](https://www.piql.com/about/research-and-development/preservation-virtual-machine/),
[immortalvm](https://github.com/immortalvm)): a 64-bit stack machine of about 41 instructions,
described twice for posterity (a step-by-step building guide assuming little knowledge, and an
ISA), with a Coq formal specification; one emulator is about 2,800 lines of C. Programs can be
kept as human-readable hex to be typed back in. Its devices are image frames in and out, audio,
text and bytes out; input arrives as scanned image frames, not files.

| | iVM | WASM (WASI) |
|---|---|---|
| Re-implementing from paper | designed for it: ~41 instructions | hard: hundreds of instructions, validation, WASI |
| File input | none (image frames, text) | ordinary files |
| Speed | interpreted | near native (our 1 GiB RS03 repair is practical) |
| Toolchain today | own GCC port, small community | clang, Rust, many runtimes, browsers |

Not a replacement for WASM: WASM stays the practical layer for tools that must run fast now
(verify, repair, read UDF). iVM's *approach* fits a last-resort layer: a tiny, fully documented
machine and a minimal decoder for the most essential job (extract files, check hashes), so a
reader can be rebuilt from the disc's own documentation. Related prior art: VXA, "a virtual
architecture for durable compressed archives" ([MIT PDOS, FAST 2005](https://pdos.csail.mit.edu/papers/vxa:fast05/)),
which stores each archive's decoders inside the archive.

**Lessons we take from Piql** (without their film or barcode layer):
1. **A bootstrap on the medium:** everything needed to start decoding is on the volume itself
   (our README, catalogue and tools).
2. **A visual layer readable without a computer:** a printable summary of each disc.
3. **A table of contents of untouched files with positions:** add each file's location on the
   medium (sector extents for a disc), kept on the disc and in every later disc's catalogue, so
   files can be cut out of a raw image even if the filesystem's own records are lost. Positions
   are a property of the medium; the file list and hashes are not.
4. **Two independent descriptions of anything a future reader must re-implement** (iVM has a
   building guide and an ISA): for RS03 and our catalogue format as well as for any runtime.
5. **A decoder that travels with the data,** kept small enough to re-implement (iVM, VXA): our
   WASM tools now, possibly an iVM-style minimal layer later.

## 8. RS03 tools compared: dvdisaster, dvdisaster Light, lcsas-ecc (measured 2026-10-01)

Three implementations of dvdisaster's RS03 format, run on our seven sample discs:
- **dvdisaster 0.79.10-pl6, speed47 fork** (commit 9c5c616), what `arv make` uses now;
- **dvdisaster Light 0.3.0** (teaching-droid, commit 6a481a6), an RS03-only CLI fork;
- **lcsas-ecc** (LCSAS commit 0fb28e7, `recovery/src/lcsas-ecc/`), a 1,500-line C89
  verify/repair/augment tool, natively and as a WASI `.wasm` under Node.

Reproduce: `research/rs03/build-tools.sh DIR` (fetches and builds all of them at those
commits), then `research/rs03/experiments.py DIR [--big]`. Damage is seeded, so runs
repeat. A drive that cannot read a sector reports an error, and dvdisaster writes a marker in
its place ("unreadable" below, an *erasure*); the harness writes the same markers. "Garbled"
(wrong bytes passed off as good) is the rarer case, because drives have their own error
correction. Section 6 uses dvdisaster's simulated drive (`--debug --sim-cd`) serving an image
with permanently unreadable sectors.

### Findings

1. **dvdisaster Light is a drop-in replacement for speed47.** It verifies every sample, and
   re-creating RS03 with `-n 3200` gives byte-identical images in both tools (also on a 1 GiB
   image). Repairs and reads behave the same in every test here. It adds `--rescue` (read, fill
   from RS03, re-read, in one command), a ddrescue-format `--mapfile`, reverse reading and retry
   passes, and faster encoders.
2. **Repair limits match the theory.** Unreadable sectors up to the parity share (nroots/255:
   19.6% and 52.2% here) are repaired, less a margin because random damage is uneven across
   codewords (15% of 19.6% passed, 18% failed). Garbled sectors cost about twice as much, since
   the decoder must first find them.
3. **lcsas-ecc only repairs the data area.** It repairs unreadable data sectors up to the same
   limit as dvdisaster, natively and as `.wasm` (byte-identical results). But it fails as soon as
   any RS03 sector itself is damaged (header, CRC or ECC sectors), and the ECC area is at the
   end of the disc, the outer edge, where discs usually degrade first. It also holds the whole
   image in memory (1.3 GB for a 1 GiB image, 3x slower than dvdisaster there), and its
   `augment` only targets the standard media sizes. Its `.wasm` build fails on large images
   (section 7 below). So it is a readable
   reference implementation and a last resort, not a replacement. A portable decoder for our
   discs must tolerate damage to RS03's own sectors.
4. **Two damaged copies rebuild each other with stock tools.** Each copy 30% unreadable in
   clusters (beyond repair alone), 6.9% bad on both: reading disc A, then disc B into the same
   image (dvdisaster reads only the sectors still missing), then `-f`, gives the original. So
   do the merge by hand and both dvdisaster builds. This confirms full-disc copies plus RS03, no
   PAR2 (plan.md).
5. **Reading: two settings matter.**
   - **`--ignore-iso-size`.** If the RS03 header sector is unreadable when reading from a drive,
     both tools size the image from the filesystem and drop the whole ECC area (2,441 of 3,060
     sectors read); the repair then fails. With `--ignore-iso-size` the whole disc is read and
     the repair succeeds. `README.txt` on each disc should say this.
   - **Scattered damage and the 16-sector skip.** After a read error dvdisaster skips 16
     sectors (fast on scratches). With damage scattered evenly (35%), almost nothing was read;
     `-j 1` (or Light's retry passes) avoids that. Real damage is mostly clustered.
6. **A weak spot right after the filesystem.** Losing the 18 sectors between the RS03 header
   and the ECC area (here the padding sectors 2,442-2,447 and the whole CRC block,
   2,448-2,459), with no other damage, makes all error correction unrecognisable, even with
   `--ignore-rs03-header`; each part alone is repaired. The CRC block is one RS03 layer
   (medium / 255 sectors): 12 sectors on these samples, about 47,900 (~94 MB) on a 25 GB BD-R,
   so one scratch is unlikely to cover it there. Still to test on a full-size layout.
7. **Simulated drive defects are not permanent.** `--sim-defects` failures are read back by
   Light's retry passes, so permanent damage has to be simulated with marked source images.

#### 1. Verifying the sample discs

| Disc | Data sectors | RS03 redundancy | speed47 -t | Light -t | lcsas-ecc verify |
|---|---|---|---|---|---|
| FAMILY-01_2020-2021_K.iso | 2400 | 26.2% | pass | pass | pass |
| PROJ-01_2020-2023_L.iso | 1663 | 82.1% | pass | pass | pass |
| SCAN-01_1995-2008_D.iso | 2441 | 24.4% | pass | pass | pass |
| SCAN-02_1995-2008_B.iso | 2441 | 24.4% | pass | pass | pass |
| SCAN-03_1995-2008_9.iso | 2236 | 35.6% | pass | pass | pass |
| TAXES-01_2019-2020_I.iso | 1424 | 112.5% | pass | pass | pass |
| TRIP-01_2019_4.iso | 1448 | 109.0% | pass | pass | pass |

#### 2. Re-creating RS03 from the bare image (same medium size)

| Disc | speed47 -c -n 3200 | Light -c -n 3200 |
|---|---|---|
| FAMILY-01_2020-2021_K.iso | identical | identical |
| PROJ-01_2020-2023_L.iso | identical | identical |
| SCAN-01_1995-2008_D.iso | identical | identical |
| SCAN-02_1995-2008_B.iso | identical | identical |
| SCAN-03_1995-2008_9.iso | identical | identical |
| TAXES-01_2019-2020_I.iso | identical | identical |
| TRIP-01_2019_4.iso | identical | identical |

lcsas-ecc augment only targets the standard media sizes (CD, DVD, BD), so it cannot
re-create these custom-size samples; see the notes.

#### 3. Repair limits: random sectors across the whole image

*Unreadable* is what a damaged disc gives: the drive reports a read error and dvdisaster
marks the sector. *Garbled* is silent corruption (wrong bytes returned as good), which
optical drives' own error correction makes rare.

SCAN-01_1995-2008_D.iso: 24.4% redundancy, 50 roots per 255-byte codeword (19.6% of the image)

| Damage | dvdisaster (speed47) | dvdisaster Light | lcsas-ecc | lcsas-ecc.wasm |
|---|---|---|---|---|
| unreadable 10% (306 sectors) | repaired | repaired | **failed** | **failed** |
| unreadable 15% (459 sectors) | repaired | repaired | **failed** | **failed** |
| unreadable 18% (550 sectors) | **failed** | **failed** | **failed** | **failed** |
| unreadable 19% (581 sectors) | **failed** | **failed** | **failed** | **failed** |
| unreadable 20% (612 sectors) | **failed** | **failed** | **failed** | **failed** |
| unreadable 22% (673 sectors) | **failed** | **failed** | **failed** | **failed** |
| garbled 10% (306 sectors) | repaired | repaired | **failed** | **failed** |
| garbled 15% (459 sectors) | **failed** | **failed** | **failed** | **failed** |
| garbled 18% (550 sectors) | **failed** | **failed** | **failed** | **failed** |
| garbled 19% (581 sectors) | **failed** | **failed** | **failed** | **failed** |
| garbled 20% (612 sectors) | **failed** | **failed** | **failed** | **failed** |
| garbled 22% (673 sectors) | **failed** | **failed** | **failed** | **failed** |
| unreadable 10%, data area only | repaired | repaired | repaired | repaired |
| unreadable 15%, data area only | repaired | repaired | repaired | repaired |
| unreadable 18%, data area only | **failed** | **failed** | **failed** | **failed** |

TRIP-01_2019_4.iso: 109.0% redundancy, 133 roots per 255-byte codeword (52.2% of the image)

| Damage | dvdisaster (speed47) | dvdisaster Light | lcsas-ecc | lcsas-ecc.wasm |
|---|---|---|---|---|
| unreadable 20% (612 sectors) | repaired | repaired | **failed** | **failed** |
| unreadable 40% (1224 sectors) | repaired | repaired | **failed** | **failed** |
| unreadable 48% (1468 sectors) | **failed** | **failed** | **failed** | **failed** |
| unreadable 50% (1530 sectors) | **failed** | **failed** | **failed** | **failed** |
| unreadable 52% (1591 sectors) | **failed** | **failed** | **failed** | **failed** |
| unreadable 55% (1683 sectors) | **failed** | **failed** | **failed** | **failed** |
| garbled 20% (612 sectors) | repaired | repaired | **failed** | **failed** |
| garbled 40% (1224 sectors) | **failed** | **failed** | **failed** | **failed** |
| garbled 48% (1468 sectors) | **failed** | **failed** | **failed** | **failed** |
| garbled 50% (1530 sectors) | **failed** | **failed** | **failed** | **failed** |
| garbled 52% (1591 sectors) | **failed** | **failed** | **failed** | **failed** |
| garbled 55% (1683 sectors) | **failed** | **failed** | **failed** | **failed** |
| unreadable 20%, data area only | repaired | repaired | repaired | repaired |
| unreadable 40%, data area only | repaired | repaired | repaired | repaired |

#### 4. Damage to the filesystem and to RS03's own bookkeeping

| Damaged | dvdisaster (speed47) | dvdisaster Light | lcsas-ecc | lcsas-ecc.wasm |
|---|---|---|---|---|
| unreadable: filesystem area (sectors 0-299) | repaired | repaired | repaired | repaired |
| unreadable: RS03 header sector (2441) | repaired | repaired | **failed** | **failed** |
| unreadable: header + first CRC layer | **failed** | **failed** | **failed** | **failed** |
| unreadable: all CRC sectors (2448-2459) | repaired | repaired | **failed** | **failed** |
| unreadable: everything after the header up to the ECC area (2442-2459) | **failed** | **failed** | **failed** | **failed** |
| unreadable: last 300 sectors (ECC area) | repaired | repaired | **failed** | **failed** |
| garbled: filesystem area (sectors 0-299) | repaired | repaired | repaired | repaired |
| garbled: RS03 header sector (2441) | repaired | repaired | **failed** | **failed** |
| garbled: header + first CRC layer | **failed** | **failed** | **failed** | **failed** |
| garbled: all CRC sectors (2448-2459) | **failed** | **failed** | **failed** | **failed** |
| garbled: everything after the header up to the ECC area (2442-2459) | **failed** | **failed** | **failed** | **failed** |
| garbled: last 300 sectors (ECC area) | repaired | repaired | **failed** | **failed** |

#### 5. Two damaged copies of the same disc (each beyond repair alone)

Clustered damage (runs of 8-128 sectors), 30% of each copy; 212 sectors (6.9%) are bad on both.

| Method | One copy alone | Both copies |
|---|---|---|
| merge the two images sector by sector, then `-f` | **failed** / **failed** | repaired |
| speed47: read disc A, then disc B into the same image (`-r -j 1`), then `-f` | **failed** | repaired |
| Light: read disc A, then disc B into the same image (`-r -j 1`), then `-f` | **failed** | repaired |

#### 6. Reading a damaged disc (simulated drive, permanent damage)

| Permanently unreadable | Tool | Sectors in the image read | Result |
|---|---|---|---|
| clustered 5% (191 sectors) | Light `-r --rescue` | 2441 | **incomplete** |
| clustered 5% (191 sectors) | Light `-r --rescue --ignore-iso-size` | 3060 | **incomplete** |
| clustered 5% (191 sectors) | speed47 `-r -j 1` then `-f` | 2441 | **incomplete** |
| clustered 5% (191 sectors) | speed47 `-r -j 1 --ignore-iso-size` then `-f` | 3060 | **incomplete** |
| clustered 15% (517 sectors) | Light `-r --rescue` | 3060 | identical |
| clustered 15% (517 sectors) | Light `-r --rescue --ignore-iso-size` | 3060 | identical |
| clustered 15% (517 sectors) | speed47 `-r -j 1` then `-f` | 3060 | identical |
| clustered 15% (517 sectors) | speed47 `-r -j 1 --ignore-iso-size` then `-f` | 3060 | identical |
| clustered 18% (590 sectors) | Light `-r --rescue` | 3060 | **incomplete** |
| clustered 18% (590 sectors) | Light `-r --rescue --ignore-iso-size` | 3060 | **incomplete** |
| clustered 18% (590 sectors) | speed47 `-r -j 1` then `-f` | 3060 | **incomplete** |
| clustered 18% (590 sectors) | speed47 `-r -j 1 --ignore-iso-size` then `-f` | 3060 | **incomplete** |
| only the RS03 header sector (2441) (1 sectors) | Light `-r --rescue` | 2441 | **incomplete** |
| only the RS03 header sector (2441) (1 sectors) | Light `-r --rescue --ignore-iso-size` | 3060 | identical |
| only the RS03 header sector (2441) (1 sectors) | speed47 `-r -j 1` then `-f` | 2441 | **incomplete** |
| only the RS03 header sector (2441) (1 sectors) | speed47 `-r -j 1 --ignore-iso-size` then `-f` | 3060 | identical |
| only ISO/UDF sector 16 (1 sectors) | Light `-r --rescue` | 3060 | identical |
| only ISO/UDF sector 16 (1 sectors) | Light `-r --rescue --ignore-iso-size` | 3060 | identical |
| only ISO/UDF sector 16 (1 sectors) | speed47 `-r -j 1` then `-f` | 3060 | identical |
| only ISO/UDF sector 16 (1 sectors) | speed47 `-r -j 1 --ignore-iso-size` then `-f` | 3060 | identical |

The simulated drive's own `--sim-defects` failures are not permanent: Light's reverse
and retry passes read all of them back, so permanent damage is simulated by marking
sectors unreadable in the image the simulated drive serves. With damage scattered
evenly (not clustered), reading with the default 16-sector skip after an error gives up on
almost the whole disc (3,058 of 3,060 sectors at 35%), so these tests use clustered damage.

#### 7. A 1 GiB image: time and peak memory

A random 1 GiB image (524,288 sectors) with RS03 for a 655,360-sector medium (23.8%
redundancy, 655,350 sectors in all), then 5% of it (32,767 sectors) made unreadable in the data
area, so that lcsas-ecc could take part. Four CPU threads.

| Step | Time | Peak memory | Result |
|---|---|---|---|
| speed47: create RS03 | 5.3 s | 136 MiB | |
| Light: create RS03 | 5.5 s | 136 MiB | byte-identical to speed47 |
| Light `-f` | 188 s | 36 MiB | repaired |
| lcsas-ecc `fix` (native) | 613 s | 1,281 MiB | repaired |
| lcsas-ecc `fix` (`.wasm`, Node) | 717 s | 1,331 MiB | **not repaired**: Node crashed (SIGSEGV) |

Why the `.wasm` run failed: lcsas-ecc reads the whole image with one `fread()` and treats a
short read as an error. WASI runtimes return a read that large in pieces: wasmtime reports
"short read", and Node's WASI crashes. Reading in 16 MiB chunks (a five-line change, tested
with both runtimes) fixes reading. The design still does not scale: it keeps the image and a
work buffer of the same size in memory (about 2.6 GB here), and sizes the image with a `long`,
which is 32 bits in WebAssembly, so a 25 GB disc image cannot work in a 32-bit `.wasm` at all.
A portable decoder has to stream the image (RS03 works one layer at a time), as dvdisaster
does: 36 MiB for the same repair.

With chunked reads, a full `.wasm` repair of the 1 GiB image got further but still failed:
under wasmtime the decode completed (418 s) and then writing the image back failed the same way
("short write", one `fwrite()` of the whole image); under Node the program crashed at once,
most likely on the ~2.6 GB it allocates. Both point the same way: stream, don't load.

## 9. Other ideas from LCSAS, tested or noted (2026-10-01)

### Cross-compiling static binaries with zig (tested)

LCSAS cross-builds its C recovery tools for six platforms with `zig cc`. zig 0.13 (from PyPI,
`pip install ziglang`), one Linux machine, no SDKs:

| Target | lcsas-ecc | udfmake |
|---|---|---|
| x86_64-linux-musl (static) | 356 KB; runs, verifies our discs | (built natively already) |
| aarch64-linux-musl (static) | 1.1 MB | builds |
| arm-linux-musleabihf (static) | 0.9 MB | |
| riscv64-linux-musl (static) | 1.5 MB | |
| x86_64-macos, aarch64-macos | 32 KB, 69 KB | builds (first macOS build of udfmake) |
| x86_64-windows-gnu | 163 KB `.exe` | does not build (POSIX headers); the `.wasm` covers Windows |

Only the x86_64 Linux binary was run. This makes "static binaries for the common platforms,
plus `.wasm` for the rest" cheap for our own C tools (plan.md, "a chain of small programs").

### A kernel-free ISO 9660 reader (tested)

LCSAS's `lcsas-iso9660` (`ls`, `cat`, `extract` without mounting) reads our hybrid discs, but
only the plain ISO 9660 names: `bag-info.txt` shows as `bag_info.txt`, and deep folders appear
under Rock Ridge's `rr_moved/`. It cannot open our UDF 2.50 discs at all. A userspace reader
for our discs has to understand UDF (and Rock Ridge or Joliet for hybrid discs); for now the
operating system's mount or 7-Zip does that.

### Worth adopting (not tested)

- **A plain-language first page for whoever finds the disc** (LCSAS `START_HERE.txt`): what the
  discs are, who made them, who can help, where keys are kept. Our `README.txt` opens with
  technical identifiers; a short plain paragraph at its top would do.
- **Record tool versions** used to make each disc (we record our own `Software`, not
  dvdisaster's or genisoimage's).
- **Pin upstream sources by SHA-256 and carry them on the disc** (LCSAS `UPSTREAM.sha256`):
  the dvdisaster source tarball next to any dvdisaster binary, which GPLv3 asks for anyway.
- **The written RS03 specification** (LCSAS `docs/DVDISASTER_RS03_FORMAT.md`, 445 lines) on
  every disc next to the repair tools, so RS03 can be re-implemented from the disc alone.
