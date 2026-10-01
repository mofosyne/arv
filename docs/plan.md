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
- Encryption: optional later, not implemented now.
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

## Separate track: standalone RS03 library

Neither dvdisaster nor the speed47 fork has a library or API; it is one GPLv3 C
program. The RS03 on-disc format is effectively frozen, so a library is
practical: extract the RS03 encoder/decoder from speed47's `src/`, keep
bit-for-bit compatibility (tested against the dvdisaster CLI), and expose
create/verify/repair. The result stays GPLv3, which constrains the licence
of anything linking to it (this repo's licence is still undecided).

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
