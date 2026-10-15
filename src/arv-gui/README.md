# arv-gui

`arv gui`: the interface, in your web browser. Optional, in C99 and POSIX (a program of its own,
so arv does not grow); everything it does can be done with arv in a terminal.

Its three tabs follow the name: **Archive** (From a folder, `arv make`; Mastering: discs by hand,
`arv plan`), **Record** (Objects: everything kept, what each object says it is, and where every copy is, `arv objects --json`;
Collections: each collection's editions, safe, replaced, kept or retired, with Keep and Retire, which
show `arv retire`'s preview before anything is recorded; Discs, where a disc's copies are listed by
letter, `arv objects DISC-ID --json`, recorded (`arv burned`, `arv stored`), known by a barcode a
scanner types in or a serial (`arv identify`), given a new letter when two catalogues gave out one
twice (`arv relabel`) or checked (`arv check --copy`); Search, which finds a copy by its barcode too)
and **Verify** (Owed: `arv todo`, also counted above the tabs, where the questions waiting for an
answer are listed but not counted; Check; Inventory: what a library's `mtx status` or a scanned
shelf read, against the copies recorded there, `arv inventory`; Audit: every event as written, copies
recorded twice and fields two catalogues changed apart, `arv audit`; Restore, and rebuild from a
disc or merge another catalogue). Jobs, the commands it ran, open from the header. File lists show a small
icon for the kind of file (image, video, audio, document, spreadsheet, archive, disc image, code),
guessed from the extension as a hint only: the disc's `formats.csv` (Siegfried) is the record.

With no archive here or above and no default (a first run), it opens on **Make an archive**: a
folder (where it was started, or browsed to), an optional name, and whether to use it from
anywhere; that runs `arv init`, and the interface opens on the new archive.

It serves one page on 127.0.0.1 and opens it. **The page does everything through arv**: it builds
each command (`arv list --json`, `arv burned ID --copies 1 ...`) and shows what arv printed. The
server knows no features. It runs what the page asks, arv or arv-assist (the local model's
suggestions), to its end or as a job whose output the page follows, and lists folders for the
file pickers. So a new feature is a change to `gui.html` alone, and the server can be rewritten in
any language against the same four routes:

| Route | What |
|---|---|
| `GET /` | the page |
| `POST /api/run` | `{"argv": [...], "stdin": "...", "program": "arv" or "arv-assist", "job": false}`: the exit status and what it printed (`out`, `err`); with `"job": true`, a job |
| `GET /api/job?id=N&since=K`, `GET /api/jobs` | a job's output from line K, and whether it is done; every job |
| `GET /api/browse?path=P&files=1` | a folder's folders (and files, with sizes) |
| `GET /api/setup` | the home it was given and where it was started; the page asks `arv where --json` for the rest |

The page and every request carry a per-session token, and the Host header must be the loopback
address, so other web pages and other local users cannot drive it. Commands run without a shell,
and the first word (after `--home PATH`) must be a command, never an option or `gui`.

```sh
arv gui                       # the home arv finds from here
arv-gui --home PATH [--port N] [--no-browser] [--llm-url URL] [--llm-model NAME]
```

It finds arv and arv-assist in `$ARV` and `$ARV_ASSIST` (arv sets both when it runs arv-gui), beside
itself (an installed `bin/`), in this checkout's `src/*/build/`, then on `PATH`. It reads the page at
each load, not built in: `$ARV_GUI_PAGE`, else `../gui.html` from where it is (a checkout's
`src/arv-gui/build/`), else `../share/arv/src/arv-gui/gui.html` (installed). So no generated file is
committed, and the page can be changed without building anything.

**How it serves:** plain HTTP/1.0 on 127.0.0.1 only. There is no TLS: the traffic never leaves the
machine. Each connection is served by a process of its own, so a slow command (`plan show --archived`
hashes every item) holds up nothing else. A job's command, output and exit status are files in a
folder of its own under `$TMPDIR` (`arv-gui-XXXXXX/jobs/N/`), removed when it stops (Ctrl+C). What a
command printed reaches the page as JSON even when it is not UTF-8 (a stray byte becomes U+FFFD).
It links arv's modules (src/arv, all but `arv.c`) for their JSON reader and strings, as arv-assist does.

| File | What |
|---|---|
| `arv-gui.c` | the server (`make` builds `build/arv-gui`) |
| `gui.html` | the page: every view, and every command it builds |
| `Makefile` | builds it |

Tests: `python3 -m unittest discover -s tests` (`tests/test_gui.py`; `make check` runs it). They
speak HTTP to `build/arv-gui`, as the page does.
