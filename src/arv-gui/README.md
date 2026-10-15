# arv-gui

`arv gui`: the interface, in your web browser. Optional, in Python 3 (standard library only);
everything it does can be done with arv in a terminal.

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

It serves one page on 127.0.0.1 and opens it. Every action runs arv itself (and arv-assist for
the local model's suggestions), so it adds no behaviour of its own; it only reads the catalogue
to show it. The page and every request carry a per-session token, and the Host header must be
the loopback address, so other web pages and other local users cannot drive it.

```sh
arv gui                       # the home arv finds from here
arv-gui --home PATH [--port N] [--no-browser] [--llm-url URL] [--llm-model NAME]
```

It finds arv and arv-assist in `$ARV` and `$ARV_ASSIST` (arv sets both when it runs arv-gui), in
this checkout's `src/*/build/`, then on `PATH`.

| File | What |
|---|---|
| `arv-gui` | the program |
| `arvgui/gui.py`, `arvgui/gui.html` | the server and the page |

Tests: `python3 -m unittest discover -s tests` (`tests/test_gui.py`; `make check` runs it).
