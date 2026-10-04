# Reference outputs: arv's behaviour, frozen

The Python arv was the reference while arv was ported to C. These files keep what it did, and
every one of them was the same from both, so the C arv (`src/arvc`) is held to it; the Python core
has since been removed, and the reference outlives it.

| | |
|---|---|
| `scenarios.sh OUT ARV...` | runs every command arv has (make, `--split`, `--draft`, rebuild, the links policies, `--tools-history`, `--extra-tools`, `--ro-crate`, an interactive make on a pseudo-terminal, burned, note, locate, access, location, collection, appraise, sets, names, where, init, `--archive`, tags, keywords, find, list, id) in a fixed setting, and keeps their output and the files they write, normalised, in OUT |
| `expected/` | the scenarios' output and files, and the recfile writer's output (`recfile/`): first written by the Python arv |
| `generate.sh` | rewrites `expected/` from the C arv, after a change made on purpose |
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
make -C src/arvc check            # arv against expected/ (and the rest of its checks)
sh tests/reference/generate.sh    # after an intended change: rewrite expected/ (just bless)
git diff tests/reference/expected # read it before committing: it is the record of what changed
```
