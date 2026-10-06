# arv-gui

`arv gui`: the interface, in your web browser. Optional, in Python 3 (standard library only);
everything it does can be done with arv in a terminal.

Its tabs: the discs, search, making a disc from a folder, **Mastering** (a disc plan: discs composed
by hand with drag and drop from a file browser, each with a fill bar; `arv plan`), checks and jobs.

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
