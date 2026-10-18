# Working on arv

For agents (and people) changing arv's code. To *use* arv, read `arv help --all`: every command,
what reads and what writes, exit status, the outputs for scripts, and the rules for scripts and
agents.

## What it is, and where the reasons are

arv makes archive discs that describe themselves (BagIt, a UDF 2.50 image, RS03 error correction,
a recfile catalogue, its own source in `tools/`), and keeps the record of what exists and where.

- `README.md`: the commands, with examples. `docs/workflow.md`: the whole workflow.
- `docs/spec/smart-archive-format.md`: the disc format. `docs/concepts.md`: the words.
- `docs/philosophy.md`: the principles. The test for anything new (principle 7): does it keep the
  record, or do the work? Work belongs to tools built for it (burners, rsync, mtx, find).
- `ROADMAP.md`: what is next, later and done, a line each, linked to issues; update it with the plan.
- `research/plan.md`: decisions and designs, dated, with what was built and what was not, and why.
  Read the relevant entry before changing a design; add one when a decision is made.

## Layout

| Path | What |
|---|---|
| `src/arv/` | the CLI, C99 and POSIX, no libraries; `arv.c` holds the usage text and the command table |
| `src/arv-assist/` | describing and tagging (`describe`, `tag`, `models`): local AI, or questions without it (`ask.c`, counting words with tags in `words.c`), C |
| `src/arv-gui/` | the browser interface: a small local server (C) and its page, `gui.html` |
| `src/udfwrite/`, `src/bagit/`, `src/rs03/` | the image writer, BagIt, the error correction: programs of their own |
| `tests/reference/` | scenarios run by `make check`, and their expected output |
| `tests/test_gui.py` | the GUI's tests |
| `dev-tools/` | tools for developing arv (the demo, a disc probe, the embedder) |
| `docs/examples/` | scripts to adapt (a tape library loop) |
| `packaging/` | Debian and Arch |
| `disc-tools.txt` | what every disc carries in `tools/`, and `make install` puts in `share/arv`: a whitelist |

## Build and test

```sh
make                    # everything
make check              # every check, the reference scenarios and the GUI's tests (a few minutes)
make -C src/arv check   # the CLI's checks only
make -C src/arv check-tcc   # the same checks, arv built by TinyCC: arv stays C99 (research/plan.md)
```

`make check` must pass before a commit. Things that are easy to miss:

- **Output changed on purpose:** `sh tests/reference/generate.sh` rewrites
  `tests/reference/expected/`. Read the diff; it is the review. Event ids, image hashes and
  temporary paths are masked by `norm` in `tests/reference/scenarios.sh`.
- **A file in `src/arv/data/` changed:** `make -C src/arv data` regenerates `src/arv/data.c`
  (committed, so one `cc` line can build arv from a disc). `descriptors.rec` goes on every disc, so
  the reference outputs change too.
- **A path added to `disc-tools.txt`:** commit it before `make check`; `git archive HEAD` cannot
  see uncommitted files.
- **`src/arv/check.sh` runs under `set -e`:** a command expected to fail is written
  `cmd && no "..."`, or `cmd || rc=$?`.
- Test scripts are POSIX `sh`, not bash.

## Changing things

- **A command:** its entry in `cmds[]` and a line in its group of the usage text (`arv.c`). Help,
  tab completion, `arv help --all` and the man page all come from that text; `check.sh` fails if
  the manual leaves a command out. Add it to the reads/writes lists in `MANUAL_NOTES` (`arv.c`).
- **The disc format or the catalogue's fields:** the spec, `src/arv/data/descriptors.rec`, the
  CHANGELOG and `research/plan.md`, together. Nothing is released yet: before 1.0 the format may
  change freely, and old fields need not be read.
- **Events are appended, never edited.** A change to the record is a new event (`new_event`), and
  a field that changes says `Sets` and `Replaces` (`sets_field`). Events about one copy name it by
  `Replication` as well as its letter (`name_copy` in `record.c`).
- **arv never deletes** a file, an image or a disc.
- **Messages** say what happened and what to do next, with the command to run.
- **Docs** are plain and short: say what a thing does, then why. British spelling (catalogue,
  licence). Show the short form of a command (`arv burned`); the group form (`arv record burned`)
  is for finding one.
- **No personal data** in commits: no real home paths, user names, host names or a person's disc
  serials in captures or examples.
- Commit messages: what changed and why, in the body as a short list.
