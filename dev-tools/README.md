# dev-tools: for developing arv, not part of it

Nothing here is needed to make, read, check or repair a disc. These help build and test arv
and keep its documentation honest.

| File | What |
|---|---|
| `embed.c` | compiles the shared data files (README.txt template, vocabularies) into `src/arv/data.c`: `make -C src/arv data` |
| `fixtures.c` | runs the language-neutral cases in `tests/fixtures/` against arv's C code (`make -C src/arv check`) |
| `fake-llm.c` | a model and embeddings server that answers from files, so `make -C src/arv-assist check` needs no model |
| `ptyrun.c` | runs a program in a pseudo-terminal, so the checks can answer an interactive `arv make` |
| `disc-probe.c` | asks an optical drive (Linux) what it reads of the disc in it: the media's maker and type, and the BCA serial that would tell one copy from another. Read only; for the first-burn drill (`cc -o disc-probe dev-tools/disc-probe.c && ./disc-probe /dev/sr0`); results in [research/bca/](../research/bca/README.md) |
| `rs03-spec-check.py` | RS03 augmenting written from [docs/spec/rs03-format.md](../docs/spec/rs03-format.md) alone, in another language on purpose: it shows the spec is complete (`make -C src/rs03 check` compares it with arv) |
| `demo-cast.py` | records `docs/demo/arv.cast`, the asciinema demo on the website, by running a typical session (Archive, Record, Verify) for real |
| `places.py` | writes `src/arv/data/default_places.rec` (the gazetteer: Natural Earth's countries and populated places, public domain) `src/arv/data/world.txt` (the land as one SVG path, for maps) and `src/arv-assist/data/countries.txt` (the countries' outlines, for where a photo's GPS is): `python3 dev-tools/places.py`, then `make -C src/arv data` |
| `common-words.sh` | writes `src/arv-assist/data/common-words.txt`, the commonest English words, from public-domain books (Project Gutenberg), which `arv describe --ask` leaves out of a text's words: `sh dev-tools/common-words.sh` |
| `bigcat.py` | makes a large synthetic home (2000 discs by default, three copies each, checks, a move, one object a disc) to time arv on: `python3 dev-tools/bigcat.py HOME [DISCS] [FILES]` (issue #37) |
| `screenshots.sh` | retakes `docs/screenshots/` (arv gui and a disc's index.html) from the samples, with headless Chromium |
| `architecture-svg.py`, `shelving-svg.py` | draw `docs/img/architecture.svg` and `shelving.svg`; edit and run them to change the pictures |
