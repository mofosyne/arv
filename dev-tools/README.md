# dev-tools: for developing arv, not part of it

Nothing here is needed to make, read, check or repair a disc. These help build and test arv
and keep its documentation honest.

| File | What |
|---|---|
| `embed.c` | compiles the shared data files (README.txt template, vocabularies) into `src/arvc/data.c`: `make -C src/arvc data` |
| `fixtures.c` | runs the language-neutral cases in `tests/fixtures/` against arv's C code (`make -C src/arvc check`) |
| `ptyrun.c` | runs a program in a pseudo-terminal, so the checks can answer an interactive `arv make` |
| `rs03-spec-check.py` | RS03 augmenting written from [docs/spec/rs03-format.md](../docs/spec/rs03-format.md) alone, in another language on purpose: it shows the spec is complete (`make -C src/rs03 check` compares it with arv) |
| `architecture-svg.py`, `shelving-svg.py` | draw `docs/img/architecture.svg` and `shelving.svg`; edit and run them to change the pictures |
