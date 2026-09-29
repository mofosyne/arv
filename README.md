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
│                            (archive.rec, manifests/, listings/, formats/, web/)
├── index.html  search.html  offline viewer and cross-disc search
├── README.txt               plain-text recovery instructions
│   data/ro-crate-metadata.json  optional RO-Crate description (--ro-crate)
├── tools/                   this tool (tree + git bundle), bagit.py
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

`docs/plan.md` has the disc layout, phased plan and open decisions.

`docs/metadata-standards.md` surveys archival metadata standards (Dublin Core,
PREMIS, METS, E-ARK, RO-Crate, OCFL, NDSA Levels...) and existing disc-cataloguing
software, and proposes this project's metadata profile.

`docs/research-notes.md` explains the choices: why UDF 2.50/2.60 is hard to
produce on Linux and adds little over RS03, why dvdisaster is not a library,
and how BagIt and recfiles split the work.

## The `archive` tool

Python 3.8+, standard library only (bagit.py is vendored). Needs `genisoimage`
and `dvdisaster` (use the [speed47 fork](https://github.com/speed47/dvdisaster) for BD-sized images).

```sh
./archive make ./2025-01-13_Projects_2020_-_2025 --location "Shelf A / Box 3"
#  -> prompts for set, title, description, subjects, note
#  -> 2020-2025_PROJECTS_01.iso  (bag + catalogue + index.html + tools/ + RS03 ECC, verified)
./archive make ./Family_Photos --set PHOTOS --snapshot set   # disc for someone else: only this set's catalogue
./archive make ./Photos_2010-2020 --set PHOTOS --split       # as many BD-R 25GB discs as needed
./archive make ./Video --medium bd100 --min-redundancy 25     # M-DISC 100GB, at least 25% RS03
./archive find IMG_2019            # which disc holds it, and where the disc is
./archive list
./archive note 2020-2025_PROJECTS_01 "Only copy of the 2019 PCB gerbers"
./archive locate 2020-2025_PROJECTS_01 "Offsite: parents' house"
./archive burned 2020-2025_PROJECTS_01 --copies 2      # after burning the ISO yourself
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
- Disc ids are `<year range of file mtimes>_<SET>_<nn>` and are used as the volume label.
- The home catalogue lives in `$BLURAY_ARCHIVE_HOME` (default
  `~/.local/share/bluray-archive`): `archive.rec` plus `manifests/<disc-id>.sha256`.
- Each disc carries a copy of the committed `HEAD` of this repo, so commit
  before burning (uncommitted changes are flagged in the `Software` field).
- Tests: `python3 -m unittest discover -s tests` (set `ARCHIVE_TEST_ECC=1` to include dvdisaster).

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
