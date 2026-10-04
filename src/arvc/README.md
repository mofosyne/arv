# arvc: arv in C

The C port of arv (research/plan.md, "one codebase in C"). **The installed `arv` is this
program:** it runs everything below, and hands what is not ported yet to the Python arv (see the
table). The whole archive, record, verify cycle and the catalogue commands work without Python,
and every disc, catalogue and line of output is the one the Python arv gives. C99 and POSIX, no
libraries; udfwrite (`../udfwrite/`) is linked in. It builds from any disc's `tools/` with one
line:

```sh
cc -O2 -o arvc tools/arv/src/arvc/*.c tools/arv/src/udfwrite/udfwrite.c
```

Called as `arv` (the installed link), it hands to the Python arv (`share/arv/arv`, or `arv-py`)
any command or option it does not have, an interactive `make` in a terminal without `-y` (the
Python one asks its questions), and `--help`. Called as `arvc`, it never hands over. The Python arv stays the
reference until everything is ported, and `make check` holds the two to the same output.

## Commands

```
arvc init [FOLDER] [--pointer HOME] [--name NAME [--default]]
                                        a .arv home in FOLDER, or a pointer to one; registered by name
arvc make [options] FOLDER              one disc image, recorded in the home (no prompts;
                                        arvc make --help lists the options)
arvc burned DISC-ID [--copies N] [--location PLACE] [--media-id ID] [--note TEXT]
arvc check (--image FILE | --device DRIVE) [DISC-ID]   dvdisaster fixity check, logged
arvc note DISC-ID TEXT
arvc locate [--add] DISC-ID PLACE...
arvc access DISC-ID public|private|sealed
arvc location list [-v] | add CODE [NAME] [--in PARENT] | move CODE [NAME] [--in PARENT]
arvc collection list | show CODE | add|put|drop CODE [ITEM...] | move CODE [--in PARENT] [--name NAME]
arvc appraise [TARGET] [--importance 'LEVEL for AUDIENCE']... [--basis TEXT] [--review DATE] [--due [DATE]]
arvc sets [-v]                          the set vocabulary with disc counts
arvc names FOLDER                       names the image cannot hold, or Windows would change
arvc where                              which home, and how it was found
arvc tags [--namespace NS]              folder tags in use, by namespace
arvc keywords [--format tsv|exiftool] DISC-ID   set paths and folder tags as XMP keywords
arvc rebuild [--prefer-disc] DISC       merge the catalogue a disc carries into the home

arvc find PATTERN                       discs, folder tags and files on every disc known
arvc list [--in CODE] [--at PLACE] [--made DATE] [--access LEVEL] [--covers DATE]
arvc id ID                              explain a disc id, check its check character

arvc info DISC                          a disc's record, binding and appraisals
arvc verify [-v] DISC                   every file against the BagIt manifests
arvc ls DISC                            the listing: files, executables, links
arvc restore [--no-links] DISC DEST     copy back, checking each file; dates, execute bits
                                        and links restored
```

The home is found as the Python arv finds it: `-C HOME` (before or after the command, like
`--home`), `$ARV_HOME`, a `.arv` folder, `.arv` pointer file or disc root from the folder being
archived or the current folder up, the machine config (`~/.config/arv/homes.rec`), then
`~/.local/share/arv`. DISC is the root of a mounted disc or an extracted image.

## What the MVP covers, and what stays in Python for now

| | arvc | Python arv only |
|---|---|---|
| Making | UDF 2.50 discs, one or (`--split`) as many as the folder needs: links policy, BagIt, listing, catalogue snapshot (access levels, sealed discs, locations, collections), catalog.rec, README.txt, index.html, tools/, appraisals (`--importance`), Siegfried format ids (`formats.csv`), `--tools-history`, `--extra-tools`, `--ro-crate`, RS03 | drafts and the local AI helpers (`--llm`, `describe`, `tag`) |
| Recording | `init` (with named homes and pointers; `--archive NAME`), `burned`, `check`, `note`, `locate`, `access`, `location`, `collection`, `appraise`, `rebuild` | `gui` |
| Looking | `sets`, `names`, `where`, `tags`, `keywords` | `describe`, `tag`, `models` (AI helpers) |
| Reading | `find`, `list`, `id`, `info`, `verify`, `ls`, `restore` | (`restore` is C only) |

Making a disc still runs two programs: dvdisaster (RS03; vendoring dvdisaster Light is
issue #19) and, in a git checkout, `git archive` and `tar` to put arv's last commit in
`tools/`. An installed arvc copies `PREFIX/share/arv` instead.

Differences from the Python arv, on purpose: discs record `Software: arvc@<commit>`; `find`
folds case for ASCII letters only; the text of README.txt is wrapped without breaking at
hyphens.

## How it is checked

`make check`:
- `build/fixtures` runs every case in `tests/fixtures/` (coverage dates, disc ids and check
  characters, the vocabulary, Match rules, file names, volume labels, recfiles: 181 cases, the
  ones the Python code passes too), and `data.c` must be up to date;
- **making:** two discs made by the Python arv and by arvc from the same folders, into fresh
  homes: every file on the discs is the same (UUIDs and the version string aside; `extents.tsv`
  of the earlier disc moves by the version string's extra byte), bagit.py validates them, and the
  home catalogues are the same; with dvdisaster on PATH, a disc with RS03 is made and tested;
- **recording:** `burned`, `note`, `locate` and `check` on copies of a catalogue leave it byte for
  byte as the Python arv leaves it; so do the 35 editing runs in `dev/edit-cases.txt` (`access`,
  `location`, `collection`, `appraise`, `sets`, `names`, `where`), whose output and exit codes
  are compared too;
- **splitting:** a folder spread over three discs with `--split`: the same rebalancing steps,
  the same files on each disc and the same home catalogue as the Python arv's;
- **rebuild:** a home rebuilt from discs, then a hand edit replaced with `--prefer-disc`: the
  same catalogue, file lists and output as the Python arv's;
- **arv:** called as `arv`, ported commands run in C and the others in the Python arv;
- **reading:** `find`, `list` and `id` print what the Python arv prints (20 queries); a disc made
  from a git repository with links and a script is restored by arvc and `git status` in the
  restore is clean; damage is reported; SHA-256 and SHA-512 match `sha256sum` and `sha512sum`
  around every block boundary; the recfile writer writes the Python's bytes.

It builds without warnings with gcc and clang (`-std=c99 -Wall -Wextra -Wpedantic -Wshadow`),
and `make check` passes under AddressSanitizer and UndefinedBehaviorSanitizer.

## Files

| File | What |
|---|---|
| `arvc.c` | the commands |
| `make.c` | `make` (cli.cmd_make and make.Maker) |
| `record.c` | `check`, `burned`, `note`, `locate` |
| `edit.c` | `access`, `location`, `collection`, `appraise`, `sets`, `names`, `where` |
| `disc.c` | reading a disc: `info`, `verify`, `ls`, `restore` |
| `catalogue.c` | `find`, `list`, `id` |
| `home.c` | finding the home; `init` |
| `archive.c` | the catalogue model: records in order, snapshots, access levels, events |
| `bag.c` | the payload scan with the links policy, BagIt tag files, the listing |
| `html.c` | index.html |
| `names.c` | file names an image cannot keep; volume labels |
| `vocab.c` | the set vocabulary |
| `discid.c`, `edtf.c` | disc ids, coverage dates |
| `rec.c`, `sha256.c`, `sha512.c`, `util.c` | recfiles, checksums, helpers |
| `data.c`, `data.h` | the files shared with the Python arv (descriptors.rec, readme.txt, index.css, default_sets.rec), generated by `make data` from `src/arv/` and committed |
| `dev/` | the fixture runner and the data embedder (not part of the program) |
