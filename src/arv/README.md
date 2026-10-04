# arv

**arv is this program** (installed as `arv`): every command, from making a disc to restoring
one. C99 and POSIX, no libraries; bagit (`../bagit/`), udfwrite (`../udfwrite/`) and rs03
(`../rs03/`) are linked in. It was
ported from a Python arv (research/plan.md, "one codebase in C"), command by command, until every
disc, catalogue and line of output was the same; the Python core was then removed, and what it
did is frozen in [tests/reference/](../../tests/reference/). It builds from any disc's `tools/` with one
line:

```sh
cc -O2 -pthread -o arv tools/arv/src/arv/*.c tools/arv/src/bagit/bagit.c \
    tools/arv/src/udfwrite/udfwrite.c tools/arv/src/rs03/rs03.c
```

The same sources build an [Actually Portable Executable](https://justine.lol/ape.html) with
[cosmocc](https://cosmo.zip/pub/cosmocc/): `make ape COSMOCC=.../bin/cosmocc` writes
`build/arv.com`, 1.7 MB, one file that runs on Linux, macOS, Windows and the BSDs, x86-64 and
ARM64. `make check-ape` runs every check below with it (they pass), and every disc made where it
exists carries it as `tools/arv.com` (found as `$ARV_APE`, `arv.com` in the installed tree, or
`build/arv.com` in a checkout; `ARV_APE=none` leaves it off).

Four optional commands are programs of their own, which arv runs: `describe`, `tag` and `models`
run arv-assist ([../arv-assist](../arv-assist/), C), and `gui` runs arv-gui
([../arv-gui](../arv-gui/), Python), with the home arv found. arv looks for them in `$ARV_ASSIST`
and `$ARV_GUI`, next to itself (an installed `bin/`), in the checkout it was built in, then on
`PATH`, and says how to get them when they are missing. `make check` holds arv to
[tests/reference/](../../tests/reference/).

## Commands

```
arv init [FOLDER] [--pointer HOME] [--name NAME [--default]]
                                        a .arv home in FOLDER, or a pointer to one; registered by name
arv make [options] FOLDER              disc images, recorded in the home (in a terminal it asks
                                        what the options leave open; -y does not; --help lists them)
arv burned DISC-ID [--copies N] [--location PLACE] [--media-id ID] [--note TEXT]
arv check (--image FILE [--repair] | --device DRIVE) [DISC-ID]   fixity check, logged (an image:
                                        RS03 tested here, --repair mends it in place, logged only
                                        when its disc is in a writable catalogue; a drive:
                                        the image's sectors read back past the cache and
                                        compared with its SHA-256 at creation, or without
                                        one, scanned by dvdisaster Light)
arv note DISC-ID TEXT
arv locate [--add] DISC-ID PLACE...
arv access DISC-ID public|private|sealed
arv location list [-v] | add CODE [NAME] [--in PARENT] | move CODE [NAME] [--in PARENT]
arv collection list | show CODE | add|put|drop CODE [ITEM...] | move CODE [--in PARENT] [--name NAME]
arv appraise [TARGET] [--importance 'LEVEL for AUDIENCE']... [--basis TEXT] [--review DATE] [--due [DATE]]
arv sets [-v]                          the set vocabulary with disc counts
arv names FOLDER                       names the image cannot hold, or Windows would change
arv where                              which home, and how it was found
arv tags [--namespace NS]              folder tags in use, by namespace
arv keywords [--format tsv|exiftool] DISC-ID   set paths and folder tags as XMP keywords
arv rebuild [--prefer-disc] DISC       merge the catalogue a disc carries into the home

arv find PATTERN                       discs, folder tags and files on every disc known
arv list [--in CODE] [--at PLACE] [--made DATE] [--access LEVEL] [--covers DATE]
          [--unchecked-since AGE|DATE] [--one-place]   due a check (5y, 18m, 90d); kept in one place
arv id ID                              explain a disc id, check its check character

arv info DISC                          a disc's record, binding and appraisals
arv verify [-v] DISC                   every file against the BagIt manifests
arv ls DISC                            the listing: files, executables, links
arv restore [--no-links] DISC DEST     copy back, checking each file; dates, execute bits
                                        and links restored
```

The home is found so: `-C HOME` (before or after the command, like
`--home`), `$ARV_HOME`, a `.arv` folder, `.arv` pointer file or disc root from the folder being
archived or the current folder up, the machine config (`~/.config/arv/homes.rec`), then
`~/.local/share/arv`. DISC is the root of a mounted disc or an extracted image.

## What is where

| | arv | arv-assist and arv-gui (optional) |
|---|---|---|
| Making | UDF 2.50 discs, one or (`--split`) as many as the folder needs: links policy, BagIt, listing, catalogue snapshot (access levels, sealed discs, locations, collections), catalog.rec, README.txt, index.html, tools/, appraisals (`--importance`), Siegfried format ids (`formats.csv`), `--tools-history`, `--extra-tools`, `--ro-crate`, drafts (`--draft`: folder tags, captions), RS03 | the drafts' authors: `describe` (a local LLM), `tag` (a small built-in model) |
| Recording | `init` (with named homes and pointers; `--archive NAME`), `burned`, `check`, `note`, `locate`, `access`, `location`, `collection`, `appraise`, `rebuild` | `gui` (arv-gui) |
| Looking | `sets`, `names`, `where`, `tags`, `keywords` | `models` (the built-in model) |
| Reading | `find`, `list`, `id`, `info`, `verify`, `ls`, `restore` | |

RS03 error correction is linked in ([../rs03](../rs03/): dvdisaster's format, byte for byte what
dvdisaster Light writes), and so is udfwrite. Making a disc runs other programs only in a git
checkout (`git archive` and `tar`, to put arv's last commit in `tools/`; an installed arv copies
`PREFIX/share/arv` instead). dvdisaster Light is needed only to scan a disc whose image hash
was never recorded (`check --device`); a damaged disc is read into an image with it or GNU
ddrescue, and arv repairs the image.

Differences from the Python arv it was ported from (since removed), on purpose: discs record `Software: arv@<commit>`; `find`
folds case for ASCII letters only; the text of README.txt is wrapped without breaking at
hyphens.

## How it is checked

`make check` (no Python needed, except where said):
- `build/fixtures` runs every case in `tests/fixtures/` (coverage dates, disc ids and check
  characters, the vocabulary, Match rules, file names, volume labels, recfiles: 160 cases, the
  frozen from the Python arv), and `data.c` must be up to date;
- **the reference scenarios** ([tests/reference/](../../tests/reference/)): every command arv has,
  run in a fixed setting (clock, time zone, user, input dates, `tools/` source), must print and
  write what the Python arv did when `expected/` was generated: transcripts with exit codes and
  error messages, home catalogues, file lists, and every file staged for each disc (BagIt files,
  catalog.rec, the catalogue snapshot, README.txt, index.html, the RO-Crate files), 145 files
  compared byte for byte after normalising; the recfile writer must write the Python's bytes;
- **a real disc**: a disc made from a git repository with links and a script is read back with 7z,
  verified, and restored with a clean `git status`; damage is reported; the Library of Congress's
  bagit.py (upstream/) agrees it is a valid bag when
  python3 is there; `tools/` carries arv's commit, and with `--tools-history` its branches;
- RS03 is added and tested by arv, `check --image` passes it again and finds a damaged sector, and
  README.txt's one `cc` line builds arv from a disc's `tools/`; with dvdisaster Light on PATH, it
  accepts the image and repairs the damaged one back; with Siegfried,
  `formats.csv` and its event are written;
- SHA-256 and SHA-512 match `sha256sum` and `sha512sum` around every block boundary;
- `describe`, `tag` and `models` go to arv-assist, and `gui` to arv-gui with the home.

It builds without warnings with gcc and clang (`-std=c99 -Wall -Wextra -Wpedantic -Wshadow`),
and `make check` passes under AddressSanitizer and UndefinedBehaviorSanitizer (with
`ASAN_OPTIONS=detect_leaks=0`: memory still held at exit is left to the system).

## Files

| File | What |
|---|---|
| `arv.c` | the commands, and running arv-assist and arv-gui for theirs |
| `tags.c` | `tags`, `keywords` |
| `formats.c` | Siegfried format identification |
| `rocrate.c` | `--ro-crate` |
| `make.c` | `make` (cli.cmd_make and make.Maker) |
| `record.c` | `check`, `burned`, `note`, `locate` |
| `edit.c` | `access`, `location`, `collection`, `appraise`, `sets`, `names`, `where` |
| `disc.c` | reading a disc: `info`, `verify`, `ls`, `restore` |
| `catalogue.c` | `find`, `list`, `id` |
| `home.c` | finding the home (`--archive` too); `init` with named homes and pointers |
| `archive.c` | the catalogue model: records in order, snapshots, access levels, events |
| `bag.c` | the payload scan with the links policy, BagIt tag files, the listing |
| `html.c` | index.html |
| `names.c` | file names an image cannot keep; volume labels |
| `vocab.c` | the set vocabulary |
| `discid.c`, `edtf.c` | disc ids, coverage dates |
| `rec.c`, `json.c`, `util.c` | recfiles, JSON, helpers (the checksums are in `../bagit`) |
| `draft.c` | drafts (`make --draft`), shared with arv-assist |
| `data/` | the data files: descriptors.rec, readme.txt (each disc's README.txt), index.css, default_sets.rec, default_tags.rec |
| `data.c`, `data.h` | `data/`, embedded; generated by `make data` and committed (the one `cc` line needs it) |
| `../../dev-tools/` | the fixture runner, the data embedder and `ptyrun` (a pseudo-terminal for the interactive scenario); not part of the program |
