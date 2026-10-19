# Roadmap

Where arv is going, and what is waiting. The reasons behind each step are in
[research/plan.md](research/plan.md), dated; what has changed so far is in
[CHANGELOG.md](CHANGELOG.md). Nothing is released yet: before 1.0 the disc format may still change.

## Now: proving it, before 1.0

1.0 is tagged once real discs have been burned, read back, and the format needs no further change.

- [ ] **First real burns:** read back on Linux, Windows and macOS (UDF 2.50 from arv's own writer) ([#5](https://github.com/mofosyne/arv/issues/5))
- [ ] **Reading them back after time on the shelf**
- [ ] **RS03's weak spot** after the filesystem, tested on a full-size BD-R layout ([#4](https://github.com/mofosyne/arv/issues/4))
- [ ] **Jukeboxes and tape libraries** on real hardware ([#36](https://github.com/mofosyne/arv/issues/36))

## Next: designed, not built

- [ ] **Thumbnails, after discs carry them:** shown in the GUI and used by describing, a contact
      sheet for a folder of pictures, and a caption for every picture (research/plan.md,
      "thumbnails on discs")
- [ ] **The assistant, later:** Follow (the view opens what it looked at), the record-changed poll,
      pictures at a terminal by `chafa`, an MCP server; the conversation kept in the home rather than
      the browser (research/plan.md, "an assistant for archival work")
- [ ] **An interface for other software**, once the commands settle: stable machine output for
      Katalog and archival tools ([#31](https://github.com/mofosyne/arv/issues/31)); Katalog importing
      arv discs from their catalogue ([#16](https://github.com/mofosyne/arv/issues/16))

## Later, if wanted

Each waits until someone needs it.

- [ ] **Disc signatures:** proof a disc came from this archive, with OpenSSH's `ssh-keygen -Y` ([#55](https://github.com/mofosyne/arv/issues/55))
- [ ] **Encryption** of chosen items, with the keys' custody in the record ([#21](https://github.com/mofosyne/arv/issues/21));
      it trades away "any computer can read a disc", so only per item, and with a standard tool
- [ ] **A disc's spare room** for second copies of the most valuable collections ([#20](https://github.com/mofosyne/arv/issues/20))
- [ ] **Reference data as distribution packages:** gazetteers, outlines, word lists, format signatures ([#53](https://github.com/mofosyne/arv/issues/53))
- [ ] **The embedding engine and its model** on every disc, so `arv tag` runs offline from a disc ([#1](https://github.com/mofosyne/arv/issues/1))
- [ ] **Tape:** yatm (LTO through LTFS) alongside arv's tape support ([#45](https://github.com/mofosyne/arv/issues/45))
- [ ] **More than one person writing the record**, with accounts: for organisations, if ever

## Nice to have

- [ ] EAC-CPF export of people and organisations, for handing names to an institution
- [ ] Korean written as separate jamo, recombined into syllables (Unicode's composition tables)
- [ ] `arv tags` columns lined up for wide characters (京 takes two)

## Work for other projects

- [ ] NetBSD makefs (UDF): the bug report checked and sent ([#6](https://github.com/mofosyne/arv/issues/6)),
      and real metadata mirroring for read-only images ([#7](https://github.com/mofosyne/arv/issues/7))

## Done

- [x] One `arv` program, in C (C99 and POSIX, no libraries): first in Python, ported command by
      command against the same outputs, the Python core then removed; checked with TinyCC too
- [x] Self-describing discs: BagIt manifests, a recfile catalogue (Dublin Core-named fields,
      PREMIS-typed events), the whole archive's snapshot, `README.txt`, `index.html` and arv's own
      source on each; access levels decide what other discs show
- [x] UDF 2.50 through arv's own writer, and RS03 error correction in arv itself, byte-identical
      to dvdisaster, so the arv on a disc can repair it anywhere (`tools/arv.com`)
- [x] Sizes for each medium and redundancy, multi-disc sets, file extents per volume
- [x] The record: collections and editions, data objects in versions, copies by letter with
      places, carriers, barcodes and disc serials, appraisals, checks, and `arv todo`
- [x] Homes (`.arv`), events with ids, merging two catalogues (`arv rebuild`), `arv audit`
- [x] Disc plans (discs composed by hand), packed objects
- [x] Format ids (PRONOM, through Siegfried)
- [x] The interface (`arv gui`): its server in C, every command in a browser
- [x] Describing: with a model, or without one by questions over your vocabulary; known names;
      places, GPS and a map; tags in any script; scans told apart
- [x] Models: found and chosen in Settings, each source on or off, your own key said at each use
- [x] The assistant (`arv chat`, and a panel in the GUI): reads on its own, proposes changes with their risk;
      Edit on a proposal, and the conversation kept across reloads; Undo, "apply tags without
      asking", how sure it was (token probabilities), rules from what you accept
- [x] `arv programs`: which optional programs are here, and how to install the rest
- [x] Thumbnails on discs: a small picture of each photo, video and PDF, named by its file's hash,
      shown in `index.html`, kept at home
- [x] A man page from the commands' own help (`arv help --man`), and tab completion
- [x] ~~`archive.sqlite` search index~~ retired: `find` scans the plain-text lists fast enough
- [x] ~~dvdisaster's source on each disc~~ superseded: arv repairs RS03 itself
