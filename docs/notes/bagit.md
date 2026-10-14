# Application note: checking a disc with BagIt tools

An arv disc is a BagIt bag ([RFC 8493](https://www.rfc-editor.org/rfc/rfc8493)) with the disc's
root as the bag's root. A BagIt tool can check that it is complete and undamaged without arv,
so you can trust the check without having to trust arv. This note uses bagit-python, the Library
of Congress validator. Every disc carries a copy of it.

## What is in the bag

| File | What |
|---|---|
| `bagit.txt` | says this is a bag (BagIt 1.0, UTF-8) |
| `bag-info.txt` | `External-Identifier`: the disc id. `External-Description`: its title and description. `Bag-Group-Identifier`: its collection or set. `Payload-Oxum`: the payload's bytes and file count. `Bag-Software-Agent`: the arv that made it |
| `manifest-sha256.txt`, `manifest-sha512.txt` | a hash of every file in `data/` |
| `tagmanifest-sha256.txt`, `tagmanifest-sha512.txt` | a hash of every other file: the catalogue, `README.txt`, `index.html` and all of `tools/` |
| `data/` | the payload, untouched |

The tag manifests cover the catalogue and the tools as well as the payload, so a full check also
says that the disc's own record and its copy of arv are intact.

## Checking a disc

From the root of a mounted disc, or of a disc's files copied to a folder:

```sh
python3 tools/arv/upstream/bagit-python/bagit.py --validate .           # every file, every hash
python3 tools/arv/upstream/bagit-python/bagit.py --validate --fast .    # file count and size only
```

Exit status 0 means valid and 1 means not. A message says which file failed and how:

```
ERROR - . is invalid: Bag validation failed: data/2020/b.bin sha256 validation failed: expected="fe32…" found="306e…"
ERROR - . is invalid: Payload-Oxum validation failed. Expected 2 files and 3003 bytes but found 2 files and 3005 bytes
```

`--fast` checks only `Payload-Oxum`. It finds a missing, extra or truncated file in seconds, but
not a changed byte, so use the full check for anything you rely on. `--quiet` hides the line it
prints for each file. If bagit-python is installed (`pip install bagit`), `bagit.py` works the
same way.

For an `.iso` file, mount it read-only, or unpack it and check the folder:

```sh
7z x -odisc PHOTO-01_2026_3.iso && python3 disc/tools/arv/upstream/bagit-python/bagit.py --validate disc
```

## Careful

- **Always give `--validate`.** Without it, `bagit.py DIR` *makes* a bag: it moves everything in
  DIR into a new `data/` folder and writes new manifests. On a disc that fails harmlessly
  (read-only). On a copy of a disc's files it rearranges the copy.
- A bag that validates is complete and undamaged. It does not prove the disc is the one the
  catalogue describes. `arv check` compares a copy with its image, and `arv audit` checks the
  catalogue.

## How this fits with arv

`arv check FOLDER` makes the same check with arv's own BagIt code (`src/bagit`), and arv's tests use
bagit-python to referee it (`upstream/README.md`). Use bagit-python when arv is not at hand, or
when you want a second opinion from code arv did not write. `sha256sum -c manifest-sha256.txt`
checks the payload with nothing installed.

Other BagIt software reads the same bag:
- the Library of Congress's bagit-java and Bagger;
- Archivematica, which takes an unzipped bag as a transfer, so a disc's files can go into an
  institutional archive as they are.

arv has not been tested against these yet. `bag-info.txt` carries the disc id and title, so the
bag is recognisable after the disc is gone.
