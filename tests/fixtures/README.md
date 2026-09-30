# Language-neutral test fixtures

Inputs and expected outputs as plain files, so the same cases can check this
Python implementation and any later port (for example to C). The files are the
contract; the Python code is one implementation of it.

## Format

TSV, one case per line; see `tsv.py` (about 40 lines to reimplement):
- lines starting with `#` are comments, and the first one names the columns;
- fields are separated by tabs;
- inside a value, `\t`, `\n` and `\\` stand for tab, newline and backslash;
- `ERROR` means the input must be rejected;
- `yes`/`no` are booleans.

## Files

| File | Covers |
|---|---|
| `check-chars.tsv` | disc id check character (Luhn mod 36) |
| `disc-id-compose.tsv` | Set + Sequence + Coverage → disc id (`set-seq-coverage/1`) |
| `disc-id-parse.tsv` | disc id → fields and validity; every single typo and neighbour swap of one id is invalid; legacy ids |
| `coverage.tsv` | EDTF coverage: normalised form, form used in ids, first and last day covered |
| `covers.tsv` | does a coverage overlap a query date or range |
| `tags.tsv` | folder tag normalisation, namespaces, XMP hierarchical form |
| `match-rules.tsv` | vocabulary `Match` globs against paths |
| `vocab.rec`, `vocab-paths.tsv`, `vocab-words.tsv` | a fixed set vocabulary: paths in the DAG, aliases (`resolve` for typed words, `guess` for folder names) |
| `recfile/*.rec`, `*.expected.tsv` | recfile parsing: every field of every record, in order |

## Changing them

- **Adding a case:** add its input to `generate.py`, run it, and check the new expected value by hand.
- **Changing behaviour on purpose:** run `python3 tests/fixtures/generate.py`, then read `git diff tests/fixtures` before committing.

`tests/test_fixtures.py` runs every case and fails if `generate.py --check` would change a file.
