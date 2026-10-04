# Reference outputs: arv's behaviour, frozen

The Python arv was the reference while arv was ported to C. These files keep what it did, so the C
arv (`src/arvc`) is held to it with no Python installed, and so the reference outlives the Python
code.

| | |
|---|---|
| `scenarios.sh OUT ARV...` | runs every command arv has (make, `--split`, rebuild, the links policies, `--tools-history`, `--extra-tools`, `--ro-crate`, an interactive make on a pseudo-terminal, burned, note, locate, access, location, collection, appraise, sets, names, where, init, `--archive`, tags, keywords, find, list, id) in a fixed setting, and keeps their output and the files they write, normalised, in OUT |
| `expected/` | the scenarios as the Python arv ran them, and its recfile writer's output (`recfile/`) |
| `generate.sh` | rewrites `expected/` from the Python arv |
| `home/` | the catalogue the recording, editing and query scenarios start from (a frozen copy of `samples/home`) |
| `edit-cases.txt` | the editing commands run on it |

The setting is fixed so the outputs are the same on every machine and every day:
`SOURCE_DATE_EPOCH` (today's date, which both arvs honour), `TZ=UTC`, the user name, an empty
machine config, the dates of every input file, and `ARV_SOURCE`, a small stand-in for arv's own
source in `tools/`, so the discs do not change with every commit. What is left to normalise: the
folder the scenarios ran in, UUIDs, `arv@`/`arvc@`, temporary folder names, and the time the
RO-Crate files were written. Tag manifests, `extents.tsv` and the contents of `tools/` are not
kept (they hash or place the files that differ by the software name).

Not covered here, because the result depends on the machine: RS03 (dvdisaster), Siegfried, and a
git checkout as `tools/`. `src/arvc/check.sh` tests those with arvc directly, along with a real
image read back with 7z, restored and damaged.

## Use

```sh
make -C src/arvc check            # arvc against expected/ (and the rest of its checks)
sh tests/reference/generate.sh    # after an intended change: rewrite expected/ from the Python arv
git diff tests/reference/expected # read it before committing
```

While the Python arv exists, a change in behaviour goes into it first, then `generate.sh`, then
arvc until `make check` passes. Once the Python arv is gone, `expected/` is edited with the C
change itself: the diff is the record of what changed.
