# Plan

Working plan for turning the scripts into an `archive` tool. Background is in
`research-notes.md` and `metadata-standards.md`.

## Principles

1. **Plain text is the source of truth.** Everything else (SQLite, HTML,
   RO-Crate) is generated and can be rebuilt.
2. **Every disc stands alone.** It can be verified, browsed and understood with
   what is on it plus a web browser, and repaired with dvdisaster.
3. **Every disc indexes the archive.** It carries a snapshot of the whole
   catalogue, so the newest disc is enough to rebuild the index.
4. **No re-scanning.** Catalogue data is produced while the image is built.
5. **Degrade gracefully.** Useful with a browser without JavaScript, or even
   with `cat`, `grep` and `sha256sum` only.

## Disc layout

```
index.html   README.txt                        ← open these first
bagit.txt  bag-info.txt  manifest-*.txt  tagmanifest-*.txt
catalog.rec                                    this disc: Disc/Copy/Event records
catalog/                                       snapshot of the whole archive at burn time
  archive.rec
  manifests/<disc-id>.sha256
  archive.sqlite                               optional convenience copy
tools/
  bluray-archival-workflow/                    uncompressed snapshot of this repo (HEAD)
  bluray-archival-workflow.bundle              git bundle, only with --tools-history
  dvdisaster/                                  source tarball + static Linux/Windows binaries
  bagit.py
data/                                          payload (embedded EXIF/XMP untouched)
[dvdisaster RS03 ECC after the filesystem]
```

All non-`data/` files are BagIt tag files, covered by the tagmanifests.

## On-disc copy of this repo

- Uncompressed snapshot of the last commit (browsable with no tools). The full history
  (`git bundle`) is opt-in with `--tools-history`: by default discs don't carry it, so
  large files removed from the repo (e.g. old model weights) never ride along.
  No tar.gz: a stream is fragile after the first bad byte.
- The commit hash is recorded in `catalog.rec` as the agent of the creation event.
- Keep the CLI **Python standard library only** (vendor `bagit.py`) so the
  on-disc copy runs without `pip`.

## On-disc HTML viewer

- `index.html`: static, no JavaScript. Disc description, notes, location, a
  folder tree with relative links into `data/`, and sizes and checksums.
- ~~`search.html`~~: removed 2026-10-01 (see Decisions). Searching across
  discs is catalogue software's job, or `archive --home catalog find` run from
  the disc. The notes below are kept for the record.
- One search box with scope **This disc / All discs**, plus disc-level search
  over titles, descriptions, notes and subjects. Results show disc ID, title,
  physical location and path:
  - hits on **this disc** link straight to the file;
  - hits on **past discs** say which disc to fetch and where it is (e.g.
    "PRJ25-01, Projects 2020-2025, Shelf A / Box 3"), and copy the path.
- Only discs burned **before or with** this one are searchable, since the
  snapshot is frozen at burn time.
- Doubles as the RO-Crate preview if the RO-Crate export is enabled.
- Limits to test: browser memory with ~1M entries across discs, and very
  large single-disc listings in the no-JS page (it may need splitting per
  top-level folder).

## Phases

### Phase 1: single-disc MVP
- [x] `archive make <folder>`: bag → prompt for Title/Description/Notes/Subject →
      `catalog.rec` → `tools/` → `index.html` → image → RS03 → `dvdisaster -t`
- [x] Home catalogue `archive.rec` (Disc/Copy/Event), short disc IDs, `Location`
- [x] `archive find <pattern>` (plain scan of manifests to start)
- [x] `archive note <disc-id> <text>` (+ `archive locate`)
- [x] Tests (`tests/test_archivetool.py`)

### Phase 2: whole-archive retrieval
- [x] `catalog/` snapshot with `--snapshot full|set|disc` (+ `listings/` with sizes and dates)
- [ ] ~~Snapshot hash chain~~ dropped: the tagmanifests already cover the snapshot
- [ ] Per-copy tracking via the BD-R BCA serial (deferred, low priority)
- [x] ~~`search.html` across the snapshot~~ (built, then removed 2026-10-01)
- [x] Generated `archive.sqlite` (`archive index`); `find` uses it when fresh
- [x] `archive check --device|--image`: dvdisaster scan/test → `fixity check` Event
- [x] `archive rebuild <disc>`: merge a disc's catalogue into home (idempotent)
- [x] `archive burned <id> --copies N`: record manual burns (`replication` Event)
- [ ] Import from VVV exports

### Phase 3: scale and standards
- [x] Split a source folder across N discs (`Bag-Count: n of N`, `Bag-Group-Identifier`); exact sizing via `genisoimage -print-size`
- [x] Medium sizing (BD 25/50/100/128), `--no-defect-management`, `--min-redundancy` (default 20%)
- [x] Siegfried/PRONOM `catalog/formats/<id>.csv` (+ `format identification` event), RO-Crate 1.2 export (`--ro-crate`, crate inside `data/` per the RO-Crate BagIt notes)

### Phase 4: extras
- [x] GUI over the CLI (`archive gui`)
- [x] NetBSD `makefs -t udf` builds on Linux as a C library: `lib/udfmake/` (UDF 2.50,
      metadata partition, two upstream bugs fixed).
- [ ] Confirm `third_party/netbsd-makefs-udf/BUG-REPORT.md` by hand, then send it to NetBSD.
- [ ] Metadata mirror duplication in udfmake (upstream lacks it).
- [x] `archive make --filesystem udf250` using it (default stays the ISO9660 + UDF 1.02 hybrid).
- [ ] Test the images with a Linux kernel mount, Windows and macOS.
- [ ] Standalone RS03 library (see below). Needs the licence decision first.

## Decisions (2026-09-29)

- CLI: **Python, standard library only**.
- Disc ids: **year range of the files + set + number**, e.g. `2020-2025_PROJECTS_01`.
- Discs for other people or external parties: **minimal catalogue**
  (`--snapshot set`, only this set). Own off-site copies can take `full`.
- Encryption: optional later, not implemented now (direction: see Decisions 2026-10-01).
- Media: **M-DISC BD-R** as standard.
- Copies: not managed. The tool makes the ISO; you burn it and record the count with `archive burned`.
- Physical disc identity: our disc id (volume label + bag-info + written on the disc). Drive-reported
  Media IDs identify the product line, not the disc; stored only as optional info.
- Redundancy: **RS03 inside every image + whole extra copies** rather than
  off-disc parity files. Burn copies from the **same ISO** so they are
  sector-identical: dvdisaster can then fill an image's unreadable sectors by
  reading another copy into the same image file before applying RS03. Off-disc
  RS01 `.ecc` files are only worth it for old discs burned without ECC.
- Default target: **BD-R 25GB with >= 20% RS03 redundancy** (~20 GB data per disc). Requires the
  speed47 dvdisaster fork to fill the disc; the stock build is detected and warned about.
- RO-Crate is opt-in (`--ro-crate`) because it adds two files to `data/`, so the payload is no
  longer byte-identical to the source folder.
- GUI: **local web UI** (`archive gui`), not Tk. tkinter is a separate distro package
  (`python3-tk`) and was missing even here; a browser is always present and matches the
  on-disc HTML. Bound to 127.0.0.1, token-protected, and it only runs `archive` commands.
- Manifest paths are written unencoded (bagit-python and `sha256sum -c`
  compatible). Names with CR/LF or a literal `%0A`/`%0D`/`%25` are rejected.

## Decisions (2026-09-30)

- Language: **Python for the workflow while it is still changing; C for durable, low-level
  format code** (`lib/udfmake`, later an RS03 library). A full C rewrite is worth it only
  once the workflow is settled. To keep that port cheap:
  - the formats are the contract: `docs/smart-archive-format.md` is the spec, not the Python code;
  - tests should move toward language-neutral fixtures (sample catalogues, ids, listings and
    expected outputs as files) that a C version can be checked against;
  - heavy parts live in separate C libraries and tools that a port reuses unchanged;
  - Python modules stay small with one job each (recfile, discid, catalog, bag).
- Disc ids: superseded the 2026-09-29 entry. Scheme `set-seq-coverage/1`
  (`PHOTO-07_2015-2024_Q`), derived from the record's Set, Sequence and Coverage.
- UDF 2.50: available through `lib/udfmake` (NetBSD makefs as a C library and program). The
  hybrid ISO9660 + UDF 1.02 image stays the default.

## Optional: local LLM metadata help (done)

- `archive describe` (folder or disc), `archive make --llm` / `--draft`, GUI "Suggest" panels.
- OpenAI-compatible HTTP API, standard library only; loopback only unless `--llm-allow-remote`.
- The model gets an inventory (names, counts, sizes, dates, types, short README text), returns
  title / description / subjects / folder tags / questions as JSON. The owner reviews every
  field; Q&A answers become notes; accepted changes are PREMIS `metadata modification` events
  with agent `llm:<model> + owner review`.
- Folder tags: `catalog/tags/<disc-id>.tags`, searched by `find` and the GUI.
- Images (done, `--vision`): sampled images and video frames go to a local vision model only
  (loopback enforced, no override, since file contents leave the inventory-only design).
  Captions feed the text model and are stored in the tags file (third column); searchable.
  Tiny models (SmolVLM-500M) caption well but ignore output formats, so the prompt asks for
  plain "Caption:/Tags:" lines, never JSON mode, and echoed instructions are discarded.
- Possible later: per-file captions for all images (not just samples) as an overnight job.

## Next: hybrid AI tiers (agreed direction, 2026-09-29)

See research-notes.md section 5 for the measurements behind this.

1. **Built-in (no setup):** a ~37 MB embedding model (bge-small-en-v1.5) for topic tags from a
   fixed, editable tag vocabulary, "more like this", and nearest-neighbour learning from the
   owner's past tag reviews. Runtime managed by the tool (a local process on a random loopback
   port). Model and runtime are a pinned, SHA-256-checked download to
   `~/.local/share/bluray-archive/models/`, never committed (every disc carries the repo history).
   Done: `archive tag` runs llama.cpp's `llama-embedding` CLI as a subprocess (no server/API),
   with `--embed-url` (OpenAI-compatible /v1/embeddings) as a fallback engine.
2. **Bring your own AI (optional, OpenAI-compatible):** >= 1.5B model for SemIf-style decisions
   (logprob readout over lettered options: private/shareable, content type, rule-based yes/no);
   >= 3B for descriptions and questions (existing); local vision model for captions (existing).
   Capabilities detected; fall back to tier 1 when no server is present.

All tiers remain optional, suggestion-only, and recorded as PREMIS events with the model as agent.

## Direction: a chain of small programs, each carried on every disc (2026-10-01)

The end state is a series of programs run in order, every one of them on every disc, so a
disc can be read, checked, repaired and searched with what is on it. Each program is small,
does one step, reads and writes plain files, and has a written format behind it. On the disc
each one travels as **source**, plus **static binaries** for the common platforms (zig cc builds
Linux x86_64/arm64/armv7/riscv64, macOS and Windows from one machine; tested 2026-10-01), plus
a **`.wasm`** build for anything else (any WASI runtime). Python glue stays while the workflow
settles; settled steps move to C.

Making a disc (`archive make` runs these in order):

| # | Step | Now | Later |
|---|---|---|---|
| 1 | Scan, hash, check names | Python (`archivetool`) | C, once settled |
| 2 | Describe: vocabulary, tags, catalogue snapshot | Python | Python (optional local LLM) |
| 3 | Bag (BagIt) | Python | C |
| 4 | Image: hybrid ISO9660/UDF 1.02, or UDF 2.50 | genisoimage, or our udfmake (C, also `.wasm`) | udfmake |
| 5 | Add RS03 | dvdisaster Light (recommended) or speed47: byte-identical output (tested) | librs03, if dvdisaster Light splits into libraries ([issue](https://github.com/teaching-droid/dvdisaster-light/issues/1)) |
| 6 | Verify the image | dvdisaster `-t` | same |
| 7 | Burn and record | any burner; `archive burned` | a safe-burning note (xorriso) |

Reading, checking and repairing (what a disc must carry for itself):

| # | Step | Now | Gap |
|---|---|---|---|
| 1 | Read a damaged disc to an image | dvdisaster `-r` (`--ignore-iso-size` if the RS03 header is unreadable); Light `-r --rescue --mapfile` | needs a real drive: native binaries only (SCSI), no `.wasm` |
| 2 | Combine two damaged copies | read copy B into copy A's image (stock dvdisaster reads only what is missing; tested) | a documented procedure in README.txt |
| 3 | Repair the image | dvdisaster `-f` | a small portable RS03 decoder (C → `.wasm`) that, unlike lcsas-ecc, survives damage to the CRC/ECC sectors and the header |
| 4 | Check the files | `sha256sum -c`, `tools/bagit.py` | a tiny C `sha256` checker for the `.wasm` set |
| 5 | Get files out without mounting | OS mount, or 7-Zip | a userspace reader for UDF 2.50 (and Rock Ridge/Joliet for hybrid discs) |
| 6 | Search the archive | `archive --home catalog find` (Python) | — |

Details and measurements: research-notes.md, sections 7-9.

## Later: an archive organiser (2026-10-02)

Not everything fits on Blu-ray, and not everything needs to (philosophy.md). The NAS and
loose drives hold everything; discs hold a curated selection. An organiser would propose
what is worth a disc:
- [ ] rank folders on everyday storage by importance and audience (rules, tags, collections,
      optionally a local model), and show why;
- [ ] skip what is already archived (by SHA-256 against the catalogue manifests);
- [ ] fill discs with the most important material first, by set;
- [ ] report copy health: discs with fewer than two copies, or all copies at one site
      (git-annex's `numcopies`, per disc rather than per file);
- [ ] report duplicates: at `archive make`, list files whose SHA-256 is already on another disc,
      and on request, which files are on several discs. Report only, never deduplicate: every disc
      stands alone, so a file archived twice stays on both discs unless the person drops it.

The tool proposes; the person decides.

## Separate track: standalone RS03 library

Neither dvdisaster nor the speed47 fork has a library or API; it is one GPLv3 C
program. The RS03 on-disc format is effectively frozen, so a library is
practical: extract the RS03 encoder/decoder from speed47's `src/`, keep
bit-for-bit compatibility (tested against the dvdisaster CLI), and expose
create/verify/repair. The result stays GPLv3, which constrains the licence
of anything linking to it (this repo's licence is still undecided).

Found 2026-10-01, both worth starting from:
- [dvdisaster-light](https://github.com/teaching-droid/dvdisaster-light): RS03-only, CLI-only
  fork, bit-identical to 0.79.10-pl6, with a `ddrescue`-format map file and a read-until-complete
  `--rescue` mode. GPLv3. Tested (section 8): byte-identical to speed47 on all samples including
  custom `-n` sizes, same repair results; a drop-in replacement for `archive make` and recovery,
  now the recommended build. Proposed upstream (2026-10-01): split it into an RS03 codec library
  and a drive-reading library with the CLI on top, for GUIs and automation such as a jukebox:
  https://github.com/teaching-droid/dvdisaster-light/issues/1
- `lcsas-ecc` in [LCSAS](https://github.com/mikmorg/lcsas) (`recovery/src/lcsas-ecc/`, plus
  `docs/DVDISASTER_RS03_FORMAT.md`, a written RS03 spec): a 1,500-line C89 RS03
  verify/repair/augment tool, stdio only. Tested on our sample discs (research-notes.md, section
  8): it repairs unreadable *data* sectors up to the RS03 limit, natively and as a WASI `.wasm`,
  but fails whenever RS03's own header, CRC or ECC sectors are damaged, holds the whole image in
  memory, and only augments to the standard media sizes. A reference and a last resort; our
  portable decoder must tolerate damage to RS03's own sectors. Licence unclear: LCSAS says MIT, but the code is
  "transcribed to match dvdisaster" (GPLv3); ask the author or treat it as GPLv3.

## Decisions (2026-10-01)

- ~~Browser search data stays as it is~~ (superseded the same day, below).
- **No search page on the disc.** `search.html` and its JavaScript copy of the catalogue
  (`catalog/web/`) are removed; `index.html` stays (static, no JavaScript). The bet is on
  two things outlasting any browser API: **Python 3**, and a **WebAssembly runtime that runs
  WASI command-line programs** (several independent ones exist; udfmake already builds as one,
  `make wasi`). Every disc carries the tool's source in `tools/`, so from the disc alone:
  `python3 tools/bluray-archival-workflow/archive --home catalog find PATTERN`. Convenient
  searching is catalogue software's job (Katalog). If a browser view is wanted later, it can
  be a WASI program that serves the catalogue over local http, not data duplicated on the disc.
- When the C pieces needed to read or repair a disc exist (verify, RS03 repair), each goes on
  the disc as source plus a `.wasm` build, so a WASI runtime alone is enough to run them.
- **README.txt on every disc carries the tested recovery steps:** the exact image size (so a
  read that lost the RS03 area is noticed; read again with `--ignore-iso-size`), dvdisaster
  Light's `--rescue`, and reading a second copy into the same image. It opens with a plain
  paragraph for whoever finds the disc (from LCSAS's START_HERE idea).
- **No PAR2.** Protection is RS03 over the whole image (which also covers the filesystem's own
  records, unlike file-level PAR2) plus **identical full-disc copies at different sites**. Copies
  are burned from one `.iso`, so they are sector-identical: read one with a map of bad sectors
  (`ddrescue`), fill the gaps from the other, and RS03 repairs what both lost. Parity spread
  across discs (PAR2 sets) only pays without whole-disc copies.
- **Encryption, when it comes (not now): sealed discs only, opt-in.** The disc stays a normal
  disc (bag, README, `catalog.rec` and tools in the clear); only the payload is encrypted, as one
  piece, so no file names leak. Two candidates, chosen at implementation time:
  - **age over a filesystem image** (`data.img.age`). age has a small published spec, several
    independent implementations and is in every major distribution. The tool does it all
    (`age -r ... < image > image.age`). Encrypt to two recipients: your key and a rescue key
    printed on paper with the estate papers (age cannot mix a passphrase with key recipients in
    one file). Inner image: SquashFS (compressed, very mature, 7-Zip reads it) or a UDF image from
    our udfmake (Windows and macOS mount it natively; one filesystem format across the project).
    Costs: age stops at the first damaged 64 KiB chunk, and decrypting needs scratch space the
    size of the image (no random access).
  - **VeraCrypt container, bring your own** (the ovenmitts model): you make the container in
    VeraCrypt; the tool only burns it and writes the read-only mount command
    (`veracrypt --text --mount-options ro ...`) into README.txt. Sectors are encrypted
    independently (XTS), so damage stays local; it mounts in place; Linux's `cryptsetup --type
    tcrypt --veracrypt` opens it too (a second, in-kernel implementation). Costs: one password
    rather than several recipients, and VeraCrypt itself is a heavier install outside Debian/Ubuntu.
    Carry over ovenmitts's rules: refuse a container that is currently mounted
    (`veracrypt --text --list`); a **fresh container per archive generation** (diverged copies of
    one container share a master key, permanently on write-once media); keep an **external backup
    of the volume header**, its single point of total failure.
  - **Reminder: error correction is already handled below this layer.** RS03 over the whole
    image plus identical copies at other sites repair the ciphertext before anything is
    decrypted. So the encryption layer does not need to tolerate damage itself, and the choice
    should favour **simplicity of implementation** over self-healing features. VeraCrypt's local
    damage is a bonus, not a requirement, which leans towards age.

## Later: catalogue snapshot size

Each disc carries every earlier disc's manifests, listings and format IDs:
about 430 bytes per file in the archive. Up to about a million files that is under 3% of a
25 GB disc, and nothing changes. When an archive heads past that (5 million files would be
12%), do the following, in this order:

- [ ] Leave other discs' format IDs (`formats/*.csv`, about half the size) out of
      snapshots; each disc keeps its own.
- [ ] Compress only the *other* discs' copies (`listings/*.tsv.gz`, `manifests/*.sha256.gz`,
      `formats/*.csv.gz`; about 28% of plain). This disc's own catalogue, manifests and listing
      stay plain, so it stays readable without tools. `rebuild`/`find` read `.gz`.
- [ ] Do it automatically, only when the snapshot would exceed about 2% of the disc's data
      budget, so small archives stay entirely plain.

## Open decisions

- Licence for this repo (GPLv3 fits if the RS03 library happens)
