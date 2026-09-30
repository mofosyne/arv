# Blu-ray Archival Workflow

Tools for writing long-term personal archives (photos, video, source code) to
Blu-ray discs that can still be read, checked and repaired decades from now
using only open, well-documented formats.

## Disc layout (planned)

```
<disc root>                  filesystem: ISO9660 (Rock Ridge + Joliet) + UDF bridge, or UDF 2.01
├── bagit.txt                BagIt signature (RFC 8493)
├── bag-info.txt             Bagging-Date, Bag-Group-Identifier, Bag-Count "n of N", Payload-Oxum
├── manifest-sha256.txt      per-file checksums (`sha256sum -c` compatible)
├── manifest-sha512.txt
├── tagmanifest-sha256.txt   checksums of the files above + catalog.rec
├── catalog.rec              GNU recutils catalogue for this disc
├── catalog/                 snapshot of the whole archive catalogue at burn time
│                            (archive.rec, manifests/, listings/, formats/, tags/, web/)
├── index.html  search.html  offline viewer and cross-disc search
├── README.txt               plain-text recovery instructions
│   data/ro-crate-metadata.json  optional RO-Crate description (--ro-crate)
├── tools/                   this tool (snapshot of the last commit), bagit.py
└── data/                    the payload
[ dvdisaster RS03 ECC data appended after the filesystem ]
```

Each layer does its own job:

| Layer | Purpose | Tool |
|---|---|---|
| dvdisaster RS03 (augmented image) | **Repair** unreadable sectors | `dvdisaster` |
| BagIt manifests | **Detect** corruption per file, portable off-disc | `bagit.py`, or plain `sha256sum -c` |
| recfile catalogue | **Find** which disc holds what, without mounting | `recsel`, `recfix` |

## Current state

| Path | Status |
|---|---|
| `scripts/create-archive-iso.sh` | Working. Hybrid ISO9660/UDF 1.02 image via `genisoimage`, then RS03 augment |
| `scripts/create-archive-udf.sh` | Experimental. UDF 2.01 image via `mkudffs` + loop mount (needs sudo), then RS03 augment |
| `tests/smoke-test.sh` | End-to-end check: bag → image → ECC → damage → repair → validate bag |

`samples/` has seven small sample discs made with the full workflow (37 MB, with
RS03 error correction) and their catalogue: try `./archive --home samples/home list`.

`docs/plan.md` has the disc layout, phased plan and open decisions.

`docs/smart-archive-format.md` specifies the on-disc catalogue format (draft 0.1) so other
cataloguing programs (e.g. Katalog) can read a disc and prefill their database without scanning it.

`docs/metadata-standards.md` surveys archival metadata standards (Dublin Core,
PREMIS, METS, E-ARK, RO-Crate, OCFL, NDSA Levels...) and existing disc-cataloguing
software, and proposes this project's metadata profile.

`docs/organising.md` collects what Katalog, Hydrus, Lightroom/digiKam, Paperless-ngx,
Johnny.Decimal and archival software teach about categories and structure, and what
was adopted (aliases, match rules, access levels, namespaced tags, location records).

`docs/research-notes.md` explains the choices: why UDF 2.50/2.60 is hard to
produce on Linux and adds little over RS03, why dvdisaster is not a library,
and how BagIt and recfiles split the work.

## The `archive` tool

Python 3.8+, standard library only (bagit.py is vendored). Needs `genisoimage`
and `dvdisaster` (use the [speed47 fork](https://github.com/speed47/dvdisaster) for BD-sized images).

```sh
./archive location add HOME Home
./archive location add BOX3 "Box 3, blue lid" --in HOME
./archive make ./2025-01-13_Projects_2020_-_2025 --location BOX3
#  -> prompts for set (PROJ, from the folder name), categories (CODE, ELEC: from the files), title, ...
#  -> PROJ-01_2020-2025_K.iso  (bag + catalogue + index.html + tools/ + RS03 ECC, verified)
./archive make ./Diaries --access sealed      # other discs' catalogues show only its id and location
./archive make ./Photos --filesystem udf250   # UDF 2.50 image (Blu-ray style) instead of the hybrid ISO; needs lib/udfmake
./archive make ./Family_Photos --set PHOTOS --snapshot set   # disc for someone else: only this set's catalogue
./archive make ./Photos_2010-2020 --set PHOTOS --split       # as many BD-R 25GB discs as needed
./archive make ./Video --medium bd100 --min-redundancy 25     # M-DISC 100GB, at least 25% RS03
./archive find IMG_2019            # which disc holds it, and where the disc is
./archive list --covers 2019-07-15    # discs whose date range includes that day (or 2019, 2019-07)
./archive sets -v                     # the vocabulary tree with disc counts, aliases and match rules
./archive list --in MEMORIES          # discs anywhere under a vocabulary entry
./archive id PHOTOS-07_2015-2024_Q   # explain / check an id (catches typos)
./archive note 2020-2025_PROJECTS_01 "Only copy of the 2019 PCB gerbers"
./archive locate 2020-2025_PROJECTS_01 BOX3 OFFSITE   # one location per place a copy is kept
./archive burned 2020-2025_PROJECTS_01 --copies 1 --location OFFSITE  # after burning the ISO yourself
./archive location move BOX3 --in OFFSITE             # moving a box moves its discs
./archive location list -v                            # places as a tree, with the discs in each
./archive list --at HOME                              # discs anywhere inside a place
./archive access 2020-2025_PROJECTS_01 public          # public / private (default) / sealed
./archive tags                                         # every folder tag in use, by namespace
./archive keywords PROJ-01_2020-2025_K --format exiftool > kw.args  # tags as XMP keywords
./archive check --device /dev/sr0                        # scan a disc, log a fixity-check event
./archive check --image 2020-2025_PROJECTS_01.iso
./archive rebuild /media/disc                            # recreate/merge the home catalogue from a disc
./archive index                                          # SQLite index: fast find at millions of files
./archive gui                                            # the same, in your web browser
```

`archive gui` opens a local page (127.0.0.1 only, per-session token) with tabs for
the disc list and history, notes, location and burned copies, search, making a
disc (with a folder picker), checking discs and rebuilding the catalogue. Every
action runs the same `archive` command as the terminal and shows its output.

On the disc, `index.html` browses the disc without JavaScript, and `search.html`
searches this disc and every disc in its catalogue snapshot (all offline, from
`file://`; ~1M files: about 1 s for the first search, then about 0.3 s).

- `--medium` (default `bd25`; also `bd50`, `bd100`, `bd128`, `auto`) sets the disc the image
  targets. RS03 fills the rest of the disc, and each disc keeps at least `--min-redundancy`
  (default 20%): about 20 GB of data per 25 GB disc. Sizes are measured exactly before
  writing. A folder that is too big either reports how many discs it needs or, with
  `--split`, becomes a set of complete bags (`Bag-Count: n of N`) that each know the whole set.
- Filling the disc needs the [speed47 dvdisaster fork](https://github.com/speed47/dvdisaster);
  the distro 0.79.10 build shrinks RS03 to the smallest standard size (a warning says so).
  Both builds exit with status 1 after a *successful* `-f` repair; check with `-t`.
- With [Siegfried](https://www.itforarchivists.com/siegfried) (`sf`) installed, each file's
  PRONOM format is recorded in `catalog/formats/<disc-id>.csv`. `--ro-crate` adds
  `data/ro-crate-metadata.json` (RO-Crate 1.2, passes the validator's required checks).
- The source folder is never modified: tag files are staged separately and the
  folder is grafted into the image as `data/`.
- `--filesystem` picks the image: `hybrid` (default: ISO9660 + Rock Ridge + Joliet with a
  UDF 1.02 bridge, readable almost anywhere) or `udf250` (UDF 2.50 with a metadata partition,
  built by [`lib/udfmake`](lib/udfmake/), NetBSD's makefs as a C library and program; run
  `make -C lib/udfmake` once). Both carry the same files, catalogue and RS03 data.
- Disc ids look like `PHOTOS-07_2015-2024_Q`: set, number, coverage and a check character
  that catches typos. They are derived from the record's `Set`, `Sequence` and `Coverage`
  (EDTF: `2019`, `2015/2024`, `199X`, `1995~`) and used as the volume label. `archive id <ID>`
  explains and checks one; `archive list --covers 2019-07-15` finds discs by date. Older ids stay valid.
- Discs are classified with a word vocabulary in `<home>/sets.rec` (PHOTO, TRIP, SCAN, TAXES,
  PROJ, CODE, ...), a hierarchy where an entry can have several parents (SCAN is under PHOTO
  and RECORDS). One `--set` gives the id prefix; `--category` (repeatable) adds more codes,
  e.g. `--set PROJ --category CODE --category ELEC`. The folder name suggests a set
  (`Holiday` -> TRIP), and the disc records every vocabulary path (`MEMORIES/PHOTO/TRIP`).
  Entries have `Alias` words (`--set holidays` means TRIP), a `ScopeNote`, and `Match`
  globs (`*.kicad_pcb` -> ELEC) that suggest the set and categories from the files
  themselves without any model (`--no-rules` to skip).
- Where discs are kept is a tree of `Location` records (site, room, shelf, box), and a
  disc lists one location per place its copies are kept. `--access` controls what
  *other* discs' catalogues show of a disc: `public` (also on discs given to other
  people with `--snapshot set`), `private` (default: your own discs only), `sealed`
  (only its id, set, dates and location; no title, notes or file list).
- The home catalogue lives in `$BLURAY_ARCHIVE_HOME` (default
  `~/.local/share/bluray-archive`): `archive.rec` plus `manifests/<disc-id>.sha256`.
- Each disc carries a snapshot of the committed `HEAD` of this repo (not its history;
  `--tools-history` adds a git bundle), so commit before burning (uncommitted changes
  are flagged in the `Software` field).
- Tests: `python3 -m unittest discover -s tests` (set `ARCHIVE_TEST_ECC=1` to include dvdisaster).

## Optional: built-in tagging (`archive tag`)

Consistent folder tags from your own tag vocabulary, using a 37 MB embedding
model (bge-small-en-v1.5, MIT) run by llama.cpp's `llama-embedding` program as a
subprocess: no server, no API, no Python packages.

```sh
./archive models fetch            # pinned download, SHA-256 checked, into <home>/models/
./archive models status           # model + runtime found?
./archive tag ./2025-01-13_Personal --save draft.json   # suggest, review, save
./archive make ./2025-01-13_Personal --draft draft.json
./archive tag 2018-2022_PERSONAL_01                     # re-tag a disc already in the catalogue
```

- The vocabulary is `<home>/tags.rec` (created from `archivetool/default_tags.rec`); edit
  the descriptions freely. Describe *content*, not the medium ("cats, dogs", not "photos of").
- Tags may have `Alias` words (typing `holiday` in review stores `travel`) and `Match` globs
  that tag folders without the model: `archive tag FOLDER --rules-only` needs no download.
- Namespaced tags keep facets apart: `person:alice`, `place:kyoto`, `event:wedding-2019`,
  `source:pixel-7`. `archive tags` lists all in use (and flags words that are aliases or not
  in the vocabulary); `archive keywords DISC` exports set paths and tags as hierarchical
  keywords (`MEMORIES|PHOTO|TRIP`, `place|kyoto`) for Lightroom and digiKam, or as an
  exiftool argument file that writes them into a restored copy.
- Tags you accept are remembered, and similar folders later get the same tags, even
  tags that are not in the vocabulary. No training involved.
- Runtime: `llama-embedding` from PATH (llama.cpp is packaged by Homebrew and many
  distributions), `--llama-embedding PATH`, or `archive models build-runtime`.
- Fallback engine: `--embed-url` for any OpenAI-compatible `/v1/embeddings` server.
  Remembered examples are kept per model, because vectors from different models
  cannot be compared.
- Measured on sample folders: code, scans, video, celebrations and captioned images
  tagged correctly; folders with only camera file names get weak guesses, which you
  clear in review. Pair with `--vision` captions for opaque folders.

## Optional: local LLM help with descriptions and tags

A local model can draft the title, description, subjects and **folder tags**, and
ask you specific questions ("Who is in the Kyoto photos?"). Nothing is written
until you accept it, your answers are kept verbatim as notes, and each accepted
change is logged as a PREMIS `metadata modification` event naming the model.
Everything works without it, and no extra Python packages are needed.

```sh
ollama serve & ollama pull qwen2.5:7b          # or llama.cpp llama-server, LM Studio, vLLM
./archive describe ./2025-01-13_Personal --show-inventory   # exactly what the model will see
./archive make ./2025-01-13_Personal --llm                  # suggestions + questions, then the usual prompts
./archive describe 2018-2022_PERSONAL_01                    # improve a disc that already exists
./archive describe ./folder --save draft.json               # prepare, edit by hand, then:
./archive make ./folder --draft draft.json
./archive gui                                               # "Suggest" buttons in Make disc and disc details
```

- Any OpenAI-compatible server works: `--llm-url` / `$ARCHIVE_LLM_URL` (default
  `http://127.0.0.1:11434/v1`, Ollama), `--llm-model` / `$ARCHIVE_LLM_MODEL`
  (default: the server's first model).
- **Privacy:** the model sees an inventory (folder and file names, counts, sizes,
  dates, types, and up to 6 short README-style text files), never file contents.
  Only loopback servers are allowed unless you pass `--llm-allow-remote`.
- Folder tags (and image captions) go in `catalog/tags/<disc-id>.tags` and are searched by
  `archive find`, the GUI and the disc's `search.html`.
- **Images (`--vision`, or the checkbox in the GUI):** a few images per folder (and a
  frame per video when `ffmpeg` is installed) are shown to a local vision model. Its
  captions ("a red VW Beetle on a cobblestone street") feed the description and folder
  tags, and are stored with the tags, so `find beetle` works even when the folder is
  called `folder_B`. Because this sends image contents, it only ever uses a server on
  this computer; there is no remote override. Thumbnails use Pillow or ffmpeg when
  installed, otherwise small JPEG/PNG/WebP/GIF files are sent as-is and others skipped.
  Use `--vision-model` if your text model can't see images (e.g. `qwen2.5vl`, `gemma3`,
  `llava`). Tested with SmolVLM-500M on llama.cpp: accurate one-line captions in ~5 s per
  image on CPU, but it ignores the requested tag format, so tags come from the text
  model reading the captions. Larger vision models return tags directly.
- Model size matters. Tested on CPU: a 1.5B model gave generic text; a 3B model
  (qwen2.5-3b, about 1 minute per round) gave useful tags and good questions, and one
  answered question produced a specific title. Use a 7-8B model if your hardware allows.

## Quick start (original scripts)

```sh
# dependencies (Debian/Ubuntu)
sudo apt install genisoimage dvdisaster udftools p7zip-full recutils
pip install bagit

# 1. turn the folder into a bag (in place: payload moves into data/)
bagit.py --sha256 --sha512 \
    --bag-group-identifier "PROJECTS-2025" --bag-count "1 of 3" \
    ./2025-01-13_Projects_2020_-_2025

# 2. build the image and add RS03 error correction
scripts/create-archive-iso.sh ./2025-01-13_Projects_2020_-_2025

# later: verify / repair a disc or image
dvdisaster -i image.iso -t     # test
dvdisaster -i image.iso -f     # repair (0.79.10 exits 1 even on success; re-run -t)
```

For BD-sized images, use the multithreaded
[speed47 dvdisaster fork](https://github.com/speed47/dvdisaster). The distro
0.79.10 build is single-threaded.

## Roadmap

- [x] Single `archive` CLI (Python, stdlib only): bag → catalog.rec → image → ECC → verify
- [ ] Generate `catalog.rec` per disc (Dublin Core-named `Disc` fields, PREMIS-typed `Event` records), and merge into a master `archive.rec`
- [ ] Generate `archive.sqlite` search index from the manifests + recfiles (recutils is too slow for per-file records)
- [x] PRONOM format IDs via Siegfried (`catalog/formats/`) and optional RO-Crate (`--ro-crate`)
- [ ] Cumulative catalogue snapshot (`catalog/`) on every disc so the newest disc indexes all earlier ones; opt-out for discs given away
- [ ] Physical `Location` / `Copy` records and short disc IDs for retrieval
- [x] Multi-disc splitting for sets larger than one disc (`Bag-Count: n of N`)
- [ ] `README.txt` + dvdisaster sources/binaries on each disc for self-contained recovery
- [x] Target medium size, minimum redundancy, defect-management sizes for RS03
- [ ] Optional UDF 2.50 (metadata mirror) through NetBSD `makefs -t udf`
- [x] GUI front end over the CLI (`archive gui`, local web UI, standard library only)
- [ ] Standalone RS03 library extracted from dvdisaster (GPLv3)
