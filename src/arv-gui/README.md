# arv-gui

`arv gui`: the interface, in your web browser. Optional, in Python 3 (standard library only);
everything it does can be done with arv in a terminal.

Its three tabs follow the name: **Archive** (Mastering: discs by hand, `arv plan`; From a folder,
`arv make`), **Record** (Objects: everything kept and where every copy is, `arv objects --json`;
Discs; Search) and **Verify** (Owed: `arv todo`, also counted above the tabs; Check; Restore,
verify and rebuild). Jobs, the commands it ran, open from the header. File lists show a small
icon for the kind of file (image, video, audio, document, spreadsheet, archive, disc image, code),
guessed from the extension as a hint only: the disc's `formats.csv` (Siegfried) is the record.

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
| `arvgui/catalog.py`, `arvgui/recfile.py`, `arvgui/homes.py` | reading the catalogue (recfiles), and the list of homes |

Tests: `python3 -m unittest discover -s tests` (`tests/test_gui.py`; `make check` runs it).
