# Blu-ray Archival Workflow

Tools for writing long-term personal archives (photos, video, source code) to
Blu-ray discs that can still be read, checked and repaired decades from now
using only open, well-documented formats.

## Disc layout (planned)

```
<disc root>                  filesystem: ISO9660 (Rock Ridge + Joliet) + UDF bridge, or UDF 2.01
├── bagit.txt                BagIt signature (RFC 8493)
├── bag-info.txt             Bagging-Date, Bag-Group-Identifier, Bag-Count "n of N", Payload-Oxum
├── manifest-sha256.txt      per-file checksums (`sha256sum -c` compatible)
├── manifest-sha512.txt
├── tagmanifest-sha256.txt   checksums of the files above + catalog.rec
├── catalog.rec              GNU recutils catalogue for this disc   (planned)
├── catalog/                 snapshot of the whole archive catalogue at burn time (planned)
├── README.txt               plain-text recovery instructions      (planned)
└── data/                    the payload
[ dvdisaster RS03 ECC data appended after the filesystem ]
```

Each layer does its own job:

| Layer | Purpose | Tool |
|---|---|---|
| dvdisaster RS03 (augmented image) | **Repair** unreadable sectors | `dvdisaster` |
| BagIt manifests | **Detect** corruption per file, portable off-disc | `bagit.py`, or plain `sha256sum -c` |
| recfile catalogue | **Find** which disc holds what, without mounting | `recsel`, `recfix` |

## Current state

| Path | Status |
|---|---|
| `scripts/create-archive-iso.sh` | Working. Hybrid ISO9660/UDF 1.02 image via `genisoimage`, then RS03 augment |
| `scripts/create-archive-udf.sh` | Experimental. UDF 2.01 image via `mkudffs` + loop mount (needs sudo), then RS03 augment |
| `tests/smoke-test.sh` | End-to-end check: bag → image → ECC → damage → repair → validate bag |

`docs/metadata-standards.md` surveys archival metadata standards (Dublin Core,
PREMIS, METS, E-ARK, RO-Crate, OCFL, NDSA Levels...) and existing disc-cataloguing
software, and proposes this project's metadata profile.

`docs/research-notes.md` explains the choices: why UDF 2.50/2.60 is hard to
produce on Linux and adds little over RS03, why dvdisaster is not a library,
and how BagIt and recfiles split the work.

## Quick start

```sh
# dependencies (Debian/Ubuntu)
sudo apt install genisoimage dvdisaster udftools p7zip-full recutils
pip install bagit

# 1. turn the folder into a bag (in place: payload moves into data/)
bagit.py --sha256 --sha512 \
    --bag-group-identifier "PROJECTS-2025" --bag-count "1 of 3" \
    ./2025-01-13_Projects_2020_-_2025

# 2. build the image and add RS03 error correction
scripts/create-archive-iso.sh ./2025-01-13_Projects_2020_-_2025

# later: verify / repair a disc or image
dvdisaster -i image.iso -t     # test
dvdisaster -i image.iso -f     # repair (0.79.10 exits 1 even on success; re-run -t)
```

For BD-sized images, use the multithreaded
[speed47 dvdisaster fork](https://github.com/speed47/dvdisaster). The distro
0.79.10 build is single-threaded.

## Roadmap

- [ ] Single `archive` CLI (probably Python): bag → catalog.rec → image → ECC → verify
- [ ] Generate `catalog.rec` per disc (Dublin Core-named `Disc` fields, PREMIS-typed `Event` records), and merge into a master `archive.rec`
- [ ] Generate `archive.sqlite` search index from the manifests + recfiles (recutils is too slow for per-file records)
- [ ] Optional: PRONOM format IDs via Siegfried (`formats.yaml`) and a generated RO-Crate (`ro-crate-metadata.json`)
- [ ] Cumulative catalogue snapshot (`catalog/`) on every disc so the newest disc indexes all earlier ones; opt-out for discs given away
- [ ] Physical `Location` / `Copy` records and short disc IDs for retrieval
- [ ] Multi-disc splitting for sets larger than one disc (`Bag-Count: n of N`)
- [ ] `README.txt` + dvdisaster sources/binaries on each disc for self-contained recovery
- [ ] Target medium size / `--no-bdr-defect-management` options for RS03
- [ ] Optional UDF 2.50 (metadata mirror) through NetBSD `makefs -t udf`
- [ ] GUI front end over the CLI
