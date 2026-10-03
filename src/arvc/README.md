# arvc: arv's reader in C

The first part of the C port (research/plan.md, "one codebase in C"): everything needed to
**read** an arv disc, in C99 and POSIX with no libraries, so it builds anywhere, from the
disc's own `tools/`, with `cc -O2 -o arvc tools/arv/src/arvc/*.c`. It becomes `arv` (and later
an Actually Portable Executable) when the rest is ported; the Python `arv` stays the
reference until then.

```
arvc info DISC                         the disc's record, binding and appraisals
arvc find [-C CATALOG] PATTERN         discs, folder tags and files on every disc the catalogue
                                       knows (substring, or a glob with * ? [)
arvc list [-C CATALOG] [--in CODE] [--at PLACE] [--made DATE] [--access LEVEL] [--covers DATE]
arvc id [-C CATALOG] ID                explain a disc id and check its check character
arvc verify [-v] DISC                  every file against the BagIt manifests, and files
                                       in data/ that no manifest names
arvc ls DISC                           the listing: files, executables, links
arvc restore [--no-links] DISC DEST    copy data/ to DEST, checking every file as it is
                                       copied; restore dates, execute bits and links
```

DISC is the root of a mounted disc or an extracted image (the folder with `catalog.rec`).
CATALOG is a disc root, its `catalog/` folder, or a home (`.arv`); without `-C`: `$ARV_HOME`,
or the first `.arv` folder or disc root from the current folder up. `find` and `list` print
exactly what the Python `arv` prints (the check compares them), except that only ASCII
letters match regardless of case.

**Restore** puts the source folder back as it was (docs/smart-archive-format.md, "Links"):
- every file, checked against its SHA-256 while it is copied; a damaged file is still written
  and reported, an unreadable one is reported (repair the image with RS03 first);
- modification times and execute bits from the listing;
- links that were only noted (to folders, outside the folder, broken) recreated exactly as
  they were written, relative or absolute;
- a link to a file, which the disc holds as a copy, becomes a link again when its target was
  restored intact with the same bytes; otherwise the copy is kept and reported;
- nothing is written outside DEST: paths with `..`, absolute paths and folders reached
  through a link are refused, and links are made only after every file.

`--no-links` keeps copies and skips noted links, for file systems without links (exFAT, FAT).

`make check` first runs `build/fixtures` against `tests/fixtures/` (coverage dates, disc ids
and check characters, recfiles: the cases the Python code passes too). Then it makes a disc with `arv make` from a small git repository with links and a script,
verifies and restores it with arvc, and checks that `git status` in the restored folder is
clean: the restore is exact, links and execute bits included. It also damages a file, and
checks SHA-256 against `sha256sum` at every length around the block boundaries.

Files: `arvc.c` (commands), `rec.c` (recfiles), `sha256.c` (FIPS 180-4), `edtf.c` (coverage
dates), `discid.c` (disc ids), `fixtures.c` (the fixture runner).
