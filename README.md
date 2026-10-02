# Blu-ray Archival Workflow

Tools for writing long-term personal archives (photos, video, source code) to
Blu-ray discs that can still be read, checked and repaired decades from now
using only open, well-documented formats.

Why it works the way it does (curated discs on top of everyday storage, plain files, copies): [docs/philosophy.md](docs/philosophy.md).

**Start with [docs/workflow.md](docs/workflow.md)**: the whole flow, from a folder to discs on a
shelf, finding and checking them over the years, and recovering from damage or loss.
[docs/shelving.md](docs/shelving.md) covers arranging, labelling and storing the physical discs.

## Disc layout

```
<disc root>                  filesystem: ISO9660 (Rock Ridge + Joliet) + UDF 1.02 (default), or UDF 2.50
├── bagit.txt                BagIt signature (RFC 8493)
├── bag-info.txt             Bagging-Date, Bag-Group-Identifier, Bag-Count "n of N", Payload-Oxum
├── manifest-sha256.txt      per-file checksums (`sha256sum -c` compatible)
├── manifest-sha512.txt
├── tagmanifest-sha256.txt   checksums of the files above + catalog.rec
├── catalog.rec              GNU recutils catalogue for this disc
├── catalog/                 snapshot of the whole archive catalogue at burn time
│                            (archive.rec, volumes/<disc-id>/: manifest, listing, formats, tags)
├── index.html               offline viewer (no JavaScript)
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

`samples/` has seven small sample discs made with the full workflow (40 MB, with
RS03 error correction) and their catalogue: try `./arv --home samples/home list`.

`docs/plan.md` has the disc layout, phased plan and open decisions.

`docs/smart-archive-format.md` specifies the on-disc catalogue format (draft 0.2) so other
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

## The `arv` tool

`arv` (Archive, Record, Verify; also Norwegian for "inheritance"). Python 3, standard library
only (bagit.py is vendored). Needs `genisoimage` and `dvdisaster` ([dvdisaster Light](https://github.com/teaching-droid/dvdisaster-light),
or the [speed47 fork](https://github.com/speed47/dvdisaster), for BD-sized images; the two give byte-identical results).

### Install (Linux)

```sh
sudo apt install python3 genisoimage build-essential    # Debian/Ubuntu
# dvdisaster Light: build it from https://github.com/teaching-droid/dvdisaster-light
make install PREFIX=~/.local     # or: sudo make install   (/usr/local)
arv --help
```

`make install` copies the last commit (exactly the tree every disc carries in `tools/`) to
`PREFIX/share/arv`, and puts `arv` and `udfmake` in `PREFIX/bin`. `make uninstall` removes
them. Without installing, `./arv` in a checkout does the same.

### Where the catalogue lives: `.arv`

```sh
cd /nas && arv init --name family --default   # /nas/.arv: this archive's home catalogue
cd ~/git && arv init --pointer /nas/.arv      # ~/git/.arv: a one-line file, "Home: /nas/.arv"
arv where                                     # which home is used here, and why
```

The catalogue sits in a `.arv` folder at the root of the tree it describes, beside the files,
never in their place (deleting it leaves every file as it was). `arv` finds it like git finds
`.git`, but walks on past `.git` folders, so one `.arv` above `~/git/` covers every repository
without touching any of them. In order:

1. `--home PATH`, or `--archive NAME` (a home registered with `arv init --name`);
2. `$ARV_HOME`;
3. the nearest `.arv` folder, or `.arv` pointer file, above the folder being archived
   (`arv make FOLDER`) or the current folder; or, from the root of an archive disc, the disc's
   own `catalog/` (so `arv find` works on any disc);
4. the default home in `~/.config/arv/homes.rec` (paths are per machine; this file never goes on
   a disc, and deleting it loses nothing);
5. `~/.local/share/arv` (or `~/.local/share/bluray-archive` if you used an older version).

### Commands

```sh
arv location add HOME Home
arv location add BOX3 "Box 3, blue lid" --in HOME
arv make ./2025-01-13_Projects_2020_-_2025 --location BOX3
#  -> prompts for set (PROJ, from the folder name), categories (CODE, ELEC: from the files), title, ...
#  -> PROJ-01_2020-2025_K.iso  (bag + catalogue + index.html + tools/ + RS03 ECC, verified)
arv make ./Diaries --access sealed      # other discs' catalogues show only its id and location
arv make ./Photos --filesystem udf250   # UDF 2.50 image (Blu-ray style) instead of the hybrid ISO; needs lib/udfmake
arv names ./Photos                      # names each image type would shorten or change on Windows/macOS
# volume label: the disc id, then the title as far as it fits (32 bytes hybrid, 126 characters UDF 2.50);
# --label TEXT to choose the text, --label '' for the id alone
arv make ./Family_Photos --set PHOTOS --snapshot set   # disc for someone else: only this set's catalogue
arv make ./Photos_2010-2020 --set PHOTOS --split       # as many BD-R 25GB discs as needed
arv make ./Video --medium bd100 --min-redundancy 25     # M-DISC 100GB, at least 25% RS03
arv find IMG_2019            # which disc holds it, and where the disc is
arv list --covers 2019-07-15    # discs whose date range includes that day (or 2019, 2019-07)
arv sets -v                     # the vocabulary tree with disc counts, aliases and match rules
arv list --in MEMORIES          # discs anywhere under a vocabulary entry
arv id PHOTOS-07_2015-2024_Q   # explain / check an id (catches typos)
arv note 2020-2025_PROJECTS_01 "Only copy of the 2019 PCB gerbers"
arv locate 2020-2025_PROJECTS_01 BOX3 OFFSITE   # one location per place a copy is kept
arv burned 2020-2025_PROJECTS_01 --copies 1 --location OFFSITE  # after burning the ISO yourself
arv location move BOX3 --in OFFSITE             # moving a box moves its discs
arv location list -v                            # places as a tree, with the discs in each
arv collection add KYOTO-BEST --name "Best of Kyoto" TRIP-01_2019_4:"day2 Kinkaku-ji/"
arv collection show KYOTO-BEST                  # virtual folders across discs, with where each disc is
arv list --at HOME                              # discs anywhere inside a place
arv list --access private --made 2026          # what belongs in this year's private box
arv access 2020-2025_PROJECTS_01 public          # public / private (default) / sealed
arv tags                                         # every folder tag in use, by namespace
arv keywords PROJ-01_2020-2025_K --format exiftool > kw.args  # tags as XMP keywords
arv check --device /dev/sr0                        # scan a disc, log a fixity-check event
arv check --image 2020-2025_PROJECTS_01.iso
arv rebuild /media/disc                            # recreate/merge the home catalogue from a disc
arv index                                          # SQLite index: fast find at millions of files
arv gui                                            # the same, in your web browser
```

`arv gui` opens a local page (127.0.0.1 only, per-session token) with tabs for
the disc list and history, notes, location and burned copies, search, making a
disc (with a folder picker), checking discs and rebuilding the catalogue. Every
action runs the same `arv` command as the terminal and shows its output.

On the disc, `index.html` browses the disc without JavaScript. Searching across
discs is the job of catalogue software (such as Katalog) reading the catalogue,
or of this tool, which is on every disc: from the disc's root,
`python3 tools/bluray-archival-workflow/arv --home catalog find PATTERN`
searches every disc in its snapshot with nothing but Python.

- `--medium` (default `bd25`; also `bd50`, `bd100`, `bd128`, `auto`) sets the disc the image
  targets. RS03 fills the rest of the disc, and each disc keeps at least `--min-redundancy`
  (default 20%): about 20 GB of data per 25 GB disc. Sizes are measured exactly before
  writing. A folder that is too big either reports how many discs it needs or, with
  `--split`, becomes a set of complete bags (`Bag-Count: n of N`) that each know the whole set.
- Filling the disc needs [dvdisaster Light](https://github.com/teaching-droid/dvdisaster-light) or the [speed47 fork](https://github.com/speed47/dvdisaster) of dvdisaster;
  the distro 0.79.10 build shrinks RS03 to the smallest standard size (a warning says so).
  Both builds exit with status 1 after a *successful* `-f` repair; check with `-t`.
- With [Siegfried](https://www.itforarchivists.com/siegfried) (`sf`) installed, each file's
  PRONOM format is recorded in `catalog/volumes/<disc-id>/formats.csv`. `--ro-crate` adds
  `data/ro-crate-metadata.json` (RO-Crate 1.2, passes the validator's required checks).
- The source folder is never modified: tag files are staged separately and the
  folder is grafted into the image as `data/`.
- `--filesystem` picks the image: `hybrid` (default: ISO9660 + Rock Ridge + Joliet with a
  UDF 1.02 bridge, readable almost anywhere) or `udf250` (UDF 2.50 with a metadata partition,
  built by [`lib/udfmake`](lib/udfmake/), NetBSD's makefs as a C library and program; run
  `make -C lib/udfmake` once). Both carry the same files, catalogue and RS03 data.
- Disc ids look like `PHOTOS-07_2015-2024_Q`: set, number, coverage and a check character
  that catches typos. They are derived from the record's `Set`, `Sequence` and `Coverage`
  (EDTF: `2019`, `2015/2024`, `199X`, `1995~`) and used as the volume label. `arv id <ID>`
  explains and checks one; `arv list --covers 2019-07-15` finds discs by date. Older ids stay valid.
- Discs are classified with a word vocabulary in `<home>/config/sets.rec` (PHOTO, TRIP, SCAN, TAXES,
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
- A home (`.arv`) holds `config/` (vocabularies), `catalog/` (laid out like `catalog/` on
  every disc), `drafts/`, and `cache/` (rebuildable; marked with `CACHEDIR.TAG`). A home in the
  older flat layout is moved into this one on first use.
- Each disc carries a snapshot of the committed `HEAD` of this repo (not its history;
  `--tools-history` adds a git bundle), so commit before burning (uncommitted changes
  are flagged in the `Software` field).
- Tests: `python3 -m unittest discover -s tests` (set `ARCHIVE_TEST_ECC=1` to include dvdisaster).

## Optional: built-in tagging (`arv tag`)

Consistent folder tags from your own tag vocabulary, using a 37 MB embedding
model (bge-small-en-v1.5, MIT) run by llama.cpp's `llama-embedding` program as a
subprocess: no server, no API, no Python packages.

```sh
./arv models fetch            # pinned download, SHA-256 checked, into <home>/cache/models/
./arv models status           # model + runtime found?
./arv tag ./2025-01-13_Personal --save draft.json   # suggest, review, save
./arv make ./2025-01-13_Personal --draft draft.json
./arv tag 2018-2022_PERSONAL_01                     # re-tag a disc already in the catalogue
```

- The vocabulary is `<home>/config/tags.rec` (created from `archivetool/default_tags.rec`); edit
  the descriptions freely. Describe *content*, not the medium ("cats, dogs", not "photos of").
- Tags may have `Alias` words (typing `holiday` in review stores `travel`) and `Match` globs
  that tag folders without the model: `arv tag FOLDER --rules-only` needs no download.
- Namespaced tags keep facets apart: `person:alice`, `place:kyoto`, `event:wedding-2019`,
  `source:pixel-7`. `arv tags` lists all in use (and flags words that are aliases or not
  in the vocabulary); `arv keywords DISC` exports set paths and tags as hierarchical
  keywords (`MEMORIES|PHOTO|TRIP`, `place|kyoto`) for Lightroom and digiKam, or as an
  exiftool argument file that writes them into a restored copy.
- Tags you accept are remembered, and similar folders later get the same tags, even
  tags that are not in the vocabulary. No training involved.
- Runtime: `llama-embedding` from PATH (llama.cpp is packaged by Homebrew and many
  distributions), `--llama-embedding PATH`, or `arv models build-runtime`.
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
./arv describe ./2025-01-13_Personal --show-inventory   # exactly what the model will see
./arv make ./2025-01-13_Personal --llm                  # suggestions + questions, then the usual prompts
./arv describe 2018-2022_PERSONAL_01                    # improve a disc that already exists
./arv describe ./folder --save draft.json               # prepare, edit by hand, then:
./arv make ./folder --draft draft.json
./arv gui                                               # "Suggest" buttons in Make disc and disc details
```

- Any OpenAI-compatible server works: `--llm-url` / `$ARCHIVE_LLM_URL` (default
  `http://127.0.0.1:11434/v1`, Ollama), `--llm-model` / `$ARCHIVE_LLM_MODEL`
  (default: the server's first model).
- **Privacy:** the model sees an inventory (folder and file names, counts, sizes,
  dates, types, and up to 6 short README-style text files), never file contents.
  Only loopback servers are allowed unless you pass `--llm-allow-remote`.
- Folder tags (and image captions) go in `catalog/volumes/<disc-id>/tags.tsv` and are searched by
  `arv find` and the GUI.
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

For BD-sized images, use a multithreaded build: [dvdisaster Light](https://github.com/teaching-droid/dvdisaster-light) (RS03 only, faster encoders,
`--rescue` reading of damaged discs) or the [speed47 fork](https://github.com/speed47/dvdisaster). They produce byte-identical
images (tested, docs/research-notes.md section 8). The distro 0.79.10 build is
single-threaded.

## Roadmap

- [x] Single `archive` CLI (Python, stdlib only): bag → catalog.rec → image → ECC → verify
- [ ] Generate `catalog.rec` per disc (Dublin Core-named `Disc` fields, PREMIS-typed `Event` records), and merge into a master `archive.rec`
- [ ] Generate `archive.sqlite` search index from the manifests + recfiles (recutils is too slow for per-file records)
- [x] PRONOM format IDs via Siegfried (`catalog/volumes/<id>/formats.csv`) and optional RO-Crate (`--ro-crate`)
- [ ] Cumulative catalogue snapshot (`catalog/`) on every disc so the newest disc indexes all earlier ones; opt-out for discs given away
- [ ] Physical `Location` / `Copy` records and short disc IDs for retrieval
- [x] Multi-disc splitting for sets larger than one disc (`Bag-Count: n of N`)
- [ ] `README.txt` + dvdisaster sources/binaries on each disc for self-contained recovery
- [x] Target medium size, minimum redundancy, defect-management sizes for RS03
- [ ] Optional UDF 2.50 (metadata mirror) through NetBSD `makefs -t udf`
- [x] GUI front end over the CLI (`arv gui`, local web UI, standard library only)
- [ ] Standalone RS03 library extracted from dvdisaster (GPLv3)
