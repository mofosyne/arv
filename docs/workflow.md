# The whole workflow

This document follows the whole system: from a folder on your computer to discs
on a shelf, through years of finding and checking them, to recovering from
damage or loss. It also says where each part of this repository fits.

Details live elsewhere and are linked:
- the command reference is in [README.md](../README.md);
- the on-disc format is in [smart-archive-format.md](smart-archive-format.md);
- the reasons for each choice are in [research-notes.md](../research/research-notes.md) and [plan.md](../research/plan.md).

## The whole thing on one page

```mermaid
flowchart TD
    subgraph prepare [Prepare a folder]
        A[Folder of files] --> N[arv names<br/>file names each image type keeps]
        N --> T[optional: arv tag / describe<br/>folder tags, title, description]
    end
    subgraph make [arv make]
        T --> M1[scan + hash<br/>check names]
        M1 --> M2[classify<br/>Set, Categories, Paths]
        M2 --> M3[plan discs<br/>fit or split to the medium]
        M3 --> M4[stage each disc<br/>bag, catalogue, viewers, tools]
        M4 --> M5[build image<br/>hybrid ISO or UDF 2.50]
        M5 --> M6[RS03 error correction<br/>then verify]
        M6 --> M7[home catalogue updated]
    end
    subgraph keep [Keep]
        M7 --> B[burn the .iso yourself]
        B --> R[arv burned / locate<br/>copies, where they are]
        R --> S[(discs on shelves)]
    end
    subgraph live [Live with it]
        S --> F[arv find / list<br/>which disc, where]
        S --> C[arv check on the drive<br/>every few years]
    end
    subgraph recover [Recover]
        C -- damage --> D[dvdisaster -r / -f<br/>repair from RS03]
        S -- home catalogue lost --> RB[arv rebuild /media/disc]
        S -- tools lost --> P[plain tools:<br/>sha256sum, a browser, a text editor]
    end
```

## Before the discs: everyday storage

Discs hold a curated selection ([philosophy.md](philosophy.md)); everything lives first on
everyday storage, which is robust enough for years, not decades. Suggested setup (the tools
named here are not part of this repository and were not tested in it):

| Need | Suggested | Why |
|---|---|---|
| Master copy that notices bit rot | The NAS on **ZFS or Btrfs**, scheduled scrubs, snapshots | checksums every block, repairs from redundancy; snapshots undo mistakes |
| A pile of mismatched hard drives | **SnapRAID + mergerfs**, or plain `rsync` mirrors checked with **chkbit** / hashdeep | files stay ordinary on each drive (any one drive readable alone); parity or checksums catch rot |
| History and an off-site copy | **restic/rustic**, Borg or Kopia; **rclone** for cloud | compact and versioned; tool-dependence is acceptable on this tier |
| Clean up before choosing | duplicate finders (Czkawka, rmlint, jdupes); photo managers (digiKam ratings and tags go into XMP) | less to sort; ratings can feed appraisal |
| "Which drive or disc is it on?" | **Katalog** | catalogues offline drives and, through our format, the discs |
| What is already archived, what matters | **this tool** | SHA-256 manifests mark archived files; appraisals rank the rest; the organiser (plan.md) proposes the next disc |

Organise the NAS in plain folders by the same kinds as the archive vocabulary (photos,
projects, records ...), so folders map to disc sets. git-annex also tracks every file's copies
across drives, but turns folders into symlinks into a hidden store, which NAS clients handle
poorly; worth it only for a subset you want watched file by file.

## 0. One-time setup

| Need | For | How |
|---|---|---|
| Python 3 | everything (standard library only) | usually installed |
| `arv` | the tool | `make install PREFIX=~/.local` in this repository (or run `./arv` from it); `make uninstall PREFIX=~/.local` removes it and leaves your catalogues alone |
| `dvdisaster` | RS03 error correction | [dvdisaster Light](https://github.com/teaching-droid/dvdisaster-light) or the [speed47 fork](https://github.com/speed47/dvdisaster) fill a whole BD (byte-identical results); the stock 0.79.10 build works but pads to the smallest standard size |
| `udfmake` | the default UDF 2.50 image | built by `make`, installed by `make install` (a C compiler; nothing else) |
| `genisoimage` | only for `--filesystem hybrid` | `apt install genisoimage` |
| optional | format IDs, tagging, descriptions | Siegfried (`sf`); `arv models fetch` for `arv tag`; a local LLM server for `arv describe` |

The **home catalogue** is a `.arv` folder at the root of the tree it describes, for example
the NAS share above your photos and projects:

```sh
cd /nas && arv init --name family --default   # creates /nas/.arv
cd ~/git && arv init --pointer /nas/.arv      # another tree of the same archive: a pointer file
arv where                                     # which home is used here, and why
```

`arv` finds it by walking up from the folder being archived or the current folder (past any
`.git`), then falls back to the default home registered on this machine. Without `arv init`, a
home is created on first use in `~/.local/share/arv`. It holds:
- `config/`: your vocabularies (`sets.rec`, `tags.rec`);
- `catalog/`: `archive.rec` (every disc, event and location) and `volumes/<disc-id>/` (each
  disc's manifest, listing, formats and tags), laid out exactly like `catalog/` on a disc;
- `drafts/`: descriptions in progress;
- `cache/`: the SQLite index and local models; rebuildable, and marked so backup tools skip it.

Back this folder up. It is small, and every disc also carries a copy of it (see
[Recover](#6-recover)).

Set up **where discs live** once. You can extend it later:

```sh
arv location add HOME "Home"
arv location add HOME-PRV-2026 "Private, made 2026" --in HOME
arv location add PARENTS "Parents' house"
arv location add PARENTS-PRV-2026 "Private copies, made 2026" --in PARENTS
```

## 1. Prepare a folder

One folder becomes one disc, or a set of discs if it is too big. The folder is
never modified. Group things the way you would look for them later: a trip, a
project, a year of paperwork.

1. **Check the names:** `arv names FOLDER`.
   - Linux sees exact names on either image type.
   - The hybrid image shortens names to 103 characters for Windows and macOS, and replaces `* : ; ? \`.
   - UDF 2.50 keeps names up to 254 characters.
   - Rename anything you care about now.
2. **Optional: tags and descriptions.**
   - `arv tag FOLDER --save d.json` suggests folder tags from your vocabulary. It uses match rules and a small built-in model, and you review each suggestion.
   - `arv describe FOLDER --save d.json` asks a local LLM for a title, description and questions.
   - Both write a draft that `make` uses with `--draft d.json`.
   - Tags can be namespaced: `person:alice`, `place:kyoto`.

## 2. Make the disc image

```sh
arv make FOLDER --location HOME-PRV-2026 [--set trip] [--category scan] [--access private]
                    [--medium bd25|bd100] [--filesystem udf250|hybrid] [--split] [--draft d.json]
```

The choices that matter:

| Option | Default | Choose otherwise when |
|---|---|---|
| `--set` / `--category` | guessed from the folder name and the files (vocabulary aliases and match rules) | the guess is wrong; `arv sets -v` shows the vocabulary |
| `--medium` | `bd25` (about 20 GB of data at 20% RS03) | `bd100` for BDXL M-DISC |
| `--filesystem` | `udf250`: the Blu-ray standard; names up to 254 characters everywhere (no metadata mirror yet) | `hybrid`: an extra ISO 9660 tree for very old systems; shorter names on Windows/macOS; named `<id>.hybrid.iso` |
| `--access` | `private`: your own discs' catalogues only | `public` to appear on discs you give away; `sealed` so other discs carry only its id and location |
| `--snapshot` | `full`: every disc carries the whole catalogue | `set` for a disc given to someone else (public discs of that set only) |
| `--split` | off: stop if it doesn't fit | the folder needs several discs (`Bag-Count: n of N`) |
| `--label` | the title: the volume label is `ID Title`, cut to 32 bytes (hybrid) or 126 characters (UDF 2.50) | another text after the id, or `''` for the id alone |

### What `arv make` does, step by step

1. **Scan and hash** every file (SHA-256 and SHA-512). Symlinks and ambiguous
   names are refused. **Check names** for the chosen image type: stop on names it
   cannot hold, and list names Windows/macOS will see changed.
2. **Classify.** One `Set` (the id prefix) and any number of `Category` codes
   from `sets.rec`, with every vocabulary path recorded (`MEMORIES/PHOTO/TRIP`).
   **Coverage** is the date range of the files (EDTF), or `--coverage`.
3. **Plan.** It gives the disc an id derived from Set, Sequence and Coverage,
   plus a check character (`TRIP-01_2019_4`). It then fits the files onto the
   medium at the minimum RS03 redundancy, splitting across discs with `--split`.
   Sizes are exact: for UDF a real image is built and kept, and
   `genisoimage -print-size` measures a hybrid one.
4. **Stage each disc** in a work folder:
   - BagIt files (`bagit.txt`, `bag-info.txt`, manifests, tag manifests);
   - `catalog.rec`: this disc's record, its locations and events, starting with the `Archive` entry record for other software;
   - `catalog/`: the snapshot of the whole catalogue, limited by access, with manifests, listings, tags and search data;
   - `index.html`: offline, no network, no JavaScript;
   - `README.txt`: recovery instructions in plain text;
   - `tools/`: this repository at its last commit, plus `bagit.py`.
5. **Build the image.** The folder is grafted in as `data/` and never copied.
   UDF 2.50 uses `udfmake`, fed one folder of symlinks; the hybrid image uses
   `genisoimage`.
6. **Protect.** dvdisaster adds RS03 error correction in the space left on the
   medium, then `dvdisaster -t` verifies the result.
7. **Record.** The disc record and its events (PREMIS types: message digest
   calculation, creation, fixity check...) go into the home catalogue, with the
   manifests, listings and tags. The SQLite index is refreshed.

Output: `<disc-id>.iso`, ready to burn.

## 3. Burn and record

Burn the `.iso` yourself, with any burning program, as a disc-at-once burn of the
whole image. M-DISC BD-R is the standard medium here. Then record what you did:

```sh
arv burned TRIP-01_2019_4 --copies 2
arv burned TRIP-01_2019_4 --copies 1 --location OFFSITE --note "for the parents"
arv check --device /dev/sr0          # reads the whole disc once; logs a fixity-check event
```

Write the disc id on the disc and the case. The id's last character is a check
character, so a mistyped id is caught: `arv id TRIP-01_2019_5` tells you it's wrong.

## 4. Store

How to arrange the discs so the shelf matches the catalogue is covered in
**[shelving.md](shelving.md)**. In short: physically by access level, then a box
per year made, in the order made (sealed discs in the safe); virtually by kind,
through the catalogue. Locations are recorded down to the box; copies of the same
image go to different sites; the id goes on the disc hub and the volume label on
the spine.

```sh
arv list --access private --made 2026     # the discs that belong in this year's private box
arv locate TRIP-01_2019_4 HOME-PUB-2020 PARENTS-PUB-2020   # one location per place copies are kept
arv location move HOME-PUB-2020 --in PARENTS               # moving a box moves its discs
arv location list -v
```

## 5. Live with the archive

| Question | Answer |
|---|---|
| Which disc has this file, and where is it? | `arv find IMG_2019` (a glob works: `'*.kicad_pcb'`) |
| What do I have from July 2019? | `arv list --covers 2019-07` |
| Everything under a category or a place | `arv list --in MEMORIES`, `arv list --at OFFSITE` |
| Which tags do I use? | `arv tags`; `arv find place:kyoto` |
| Group things across discs | `arv collection add BEST --name "Best of" DISC:folder/ DISC:file`, `arv collection show BEST`: virtual folders; other software can show them as a tree ([spec](smart-archive-format.md#building-a-virtual-file-system-from-the-catalogue)) |
| Without this tool installed? | every disc carries it: `python3 tools/arv/arv --home catalog find PATTERN` from the disc's root searches every disc it knows about; or `grep -ri PATTERN catalog/volumes/*/listing.tsv` |
| Changes after burning | `arv note`, `arv locate`, `arv access` (home catalogue; later discs carry them) |

**Check discs every few years** with `arv check --device /dev/sr0`.
- A disc that needed repair is a warning sign: copy it to new media.
- `arv list` shows copies and locations, so you know which discs have only one copy.

Every new disc carries the whole catalogue as of its burn date. So **the newest
disc is always a backup of the catalogue**.

## 6. Recover

| What happened | What to do |
|---|---|
| A disc reads with errors | Follow REPAIR in the disc's `README.txt`: `dvdisaster -d /dev/sr0 -r -i disc.iso` (dvdisaster Light: add `--rescue`), check the image has the size the README states (if smaller, read again with `--ignore-iso-size`), then `dvdisaster -i disc.iso -f`. Too damaged? Copies are sector-identical: read another copy into the same image (`-r -j 1`, only missing sectors are read) and repair again. Then burn a new copy. Tested in research-notes.md section 8. |
| The home catalogue is lost | `arv rebuild /media/disc` with the newest disc: discs, events, locations, file lists. Then rebuild from later discs, or re-enter notes. |
| This tool is lost | every disc has `tools/` (the code at burn time) and `README.txt`. Without Python: `sha256sum -c manifest-sha256.txt` verifies, `index.html` browses, `grep` searches `catalog/volumes/*/listing.tsv`, and `catalog.rec` is plain text. |
| dvdisaster is lost | a copy can go in `tools/extra/` with `--extra-tools`; keep one off-disc too. The RS03 format is written up in LCSAS's DVDISASTER_RS03_FORMAT.md (research-notes.md section 7). |
| Decades later, unknown software | [smart-archive-format.md](smart-archive-format.md) (on every disc under `tools/`) explains every file; BagIt is RFC 8493; recfiles are plain text. |

The rule behind all of this: **the discs describe themselves**. Nothing on a
disc needs this tool, the home catalogue or the network to be found, verified,
read or repaired.

## 7. How the repository fits together

```
arv                        the command (runs src/arv/)
src/arv/                   the workflow, Python standard library only
  cli.py                   commands
  make.py                  the make pipeline (plan, stage, build, protect)
  bag.py  catalog.py  recfile.py  discid.py  sets.py  names.py   formats and rules
  image.py                 genisoimage / udfmake / dvdisaster
  html.py  listing.py  gui.py   viewer, file listings, the local web UI
  tagger.py  describe.py  llm.py  vision.py  models.py            optional AI helpers (local only)
  default_sets.rec  default_tags.rec                              starting vocabularies
src/udfmake/               UDF 2.50 image builder in C: NetBSD makefs, extracted (our copy)
src/udfmake/upstream/      upstream reference: draft bug report, one patch per bug, reproduction
samples/                   seven small sample discs and their catalogue; the scripts that make them
tests/                     unit and integration tests
tests/fixtures/            language-neutral test cases (TSV): the contract for a future port
docs/                      this file, the format spec, architecture, philosophy, the website
research/                  research notes, standards survey, plan and decisions, RS03 experiments
scripts/                   the original shell scripts, before arv
justfile, Makefile         everyday commands (just), build and install (make)
```

The layers, from most to least durable:
1. **Formats:** BagIt, recfiles, TSV, EDTF, and [the spec](smart-archive-format.md). They outlive any code.
2. **C tools:** `udfmake`, and later an RS03 library. Low-level, reused as they are.
3. **Python workflow:** it can change freely while the workflow settles, and may be ported to C later ([plan.md](../research/plan.md), decisions 2026-09-30).

## 8. Developer flows

- **Tests:** `python3 -m unittest discover -s tests`. Add `ARCHIVE_TEST_ECC=1` to include dvdisaster.
- **Fixtures:** after an intended behaviour change, run `python3 tests/fixtures/generate.py`, then read `git diff tests/fixtures` before committing ([README](../tests/fixtures/README.md)).
- **Sample discs:** `samples/make-samples.sh` (needs dvdisaster Light or the speed47 fork, and `src/udfmake`) replaces `samples/discs` and `samples/home`.
- **udfmake:**
  - `make -C src/udfmake check` (also `asan`, `static`);
  - changes to NetBSD's code go in `src/udfmake/netbsd/`, and each also gets a patch in `src/udfmake/upstream/patches/`;
  - `src/udfmake/upstream/repro/repro.sh` shows each patch against unmodified upstream.
- **Format changes:** update [smart-archive-format.md](smart-archive-format.md) first, and bump its version for anything a reader must know.
