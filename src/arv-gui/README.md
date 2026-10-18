# arv-gui

`arv gui`: the interface, in your web browser. Optional, in C99 and POSIX (a program of its own,
so arv does not grow); everything it does can be done with arv in a terminal.

Its three tabs follow the name.

- **Archive:** From a folder (`arv make`); Mastering, discs by hand (`arv plan`: add, move, drop, an
  item's new place and its tags, a disc's layout, make, again); What changed (`arv status`: a
  collection's folder against its last edition, or any folder against every disc).
- **Record:** Objects (everything kept, what each says it is, where every copy is; renamed and
  retagged there, `arv rename`, `arv retag`); Collections (editions, keep and retire, and a folder
  made a collection, `arv collection init`); Discs (a disc's copies by letter, recorded with
  `arv burned` or `arv stored`, known by a barcode a scanner types in, `arv identify`, given a new
  letter when two catalogues gave out one twice, `arv relabel`, checked, `arv check --copy`, and who
  may see it, `arv access`); Places (the tree of places and their temperature, `arv location`);
  Search (files, discs, and a copy by its barcode).
- **Verify:** Owed (`arv todo --json`, also counted above the tabs; the questions waiting for an
  answer are listed but not counted); Check; Inventory (`arv inventory`: a library's
  `mtx status` or a scanned shelf against the copies recorded there); Audit (`arv audit`); Restore,
  and rebuild from a disc or merge another catalogue.

Jobs, the commands it ran, open from the header. File lists show a small icon for the kind of
file, guessed from the extension as a hint only: the disc's `formats.csv` (Siegfried) is the record.

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

Installed, it is also **arv** in the desktop's menu (`arv.desktop`, with arv's mark as its icon),
which runs `arv gui --idle 15`: with no terminal to stop it in, it stops by itself once its page
has been closed for 15 minutes and no job is running (the page says it is open once a minute).

```sh
arv gui                       # the home arv finds from here
arv-gui --home PATH [--port N] [--no-browser] [--idle MINUTES] [--llm-url URL] [--llm-model NAME]
```

The Assistant panel (on the right, from the header) runs `arv-assist chat --json` for each turn, the
conversation kept in the page (and in the browser's localStorage, until New); a proposal it shows
is applied as any other command, through `/api/run`, as it came or as the person edited it (its risk
judged again by `arv-assist chat --risk`). The server knows nothing of it.

Its options for models (`--llm-url` and the rest) are given to every arv-assist command it runs,
and win over the choice made in its Settings, which is kept for this computer by `arv-assist
llm-use`. While an external provider is chosen, the header says so ("External AI: HOST").

It finds arv and arv-assist in `$ARV` and `$ARV_ASSIST` (arv sets both when it runs arv-gui), beside
itself (an installed `bin/`), in this checkout's `src/*/build/`, then on `PATH`. The page is read at
each load from `$ARV_GUI_PAGE`, else `../gui.html` from where it is (a checkout's
`src/arv-gui/build/`), else `../share/arv/src/arv-gui/gui.html` (installed), so it can be changed
without building anything. With none of those (`arv-gui` copied on its own), it serves the copy
built into it: `make` writes `build/page.c` from `gui.html` with `dev-tools/embed.c`, never
committed. Built by hand without that file, it has no copy and needs the page beside it.

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
| `arv.desktop` | the menu entry `make install` puts in `share/applications` |

Tests: `python3 -m unittest discover -s tests` (`tests/test_gui.py`; `make check` runs it). They
speak HTTP to `build/arv-gui`, as the page does.
