# Changelog

arv's versions, and the disc format's (`Version` in each disc's `catalog.rec`; the spec is
[docs/spec/smart-archive-format.md](docs/spec/smart-archive-format.md)). Discs made by any version stay
readable: a reader that knows a later format reads earlier discs, and every disc carries the
exact source that made it in `tools/` (its `Software` field names the commit).

## 1.0: when it is tagged

1.0 is tagged once the first real burns pass, because only real hardware can show that the
format needs no further change. Then the format is frozen at `Version: 1.0`, and later changes
only add fields.

- [ ] The first-burn drill passes ([docs/burning.md](docs/burning.md), issue #5): read back
      against the image hash; Linux, Windows and macOS open the disc and run `tools/arv.com`;
      a damaged copy read with ddrescue is repaired to the very image.
- [ ] The RS03 weak spot (issue #4) is measured on a full-size BD-R image.
- [ ] `FORMAT_VERSION` becomes `1.0` and `VERSION` becomes `arv 1.0` (both in src/arv/arv.h), the samples are remade and published, and the commit is tagged `v1.0`.

## Unreleased (format 0.5), October 2026

What 1.0 is planned to be, as of now. Format 0.5 is what every disc made today carries.

**More places: regions and towns, from data the machine has**
- Regions: ISO 3166-2's 5,046 subdivisions (states, prefectures, counties) from `iso-codes` when it
  is installed (it usually is), each in its country, keyed by name (`place:hokkaido`,
  `place:kyoto-prefecture` beside Kyoto the city, `place:georgia-state` beside the country).
- More places: any `places/NAME.rec` in the XDG data folders (`~/.local/share/arv/places/`,
  `/usr/share/arv/places/`). `docs/examples/places-geonames.sh` writes GeoNames' towns there
  (CC BY 4.0; 26,000 to 220,000 places, with positions); arv never downloads.
- A place from extra data that a tag uses is copied into the home's `places.rec` (describing,
  making a disc, `--parent`), so the record and its discs never depend on what is installed.
- `--ask`: a region and a city of one name (Osaka) are not two choices: the city is offered.
- Cities in their regions: the gazetteer's cities and GeoNames' places carry `Region` (the region's
  name, as their source writes it), and with iso-codes' regions arv puts each in its region: a
  photo's GPS near Uji gives Uji, in Kyoto Prefecture, in Japan, and `arv find place:kyoto-prefecture`
  finds it. A name decides; a code only when no name is given (GeoNames' codes are not all ISO's).
  Keys made from names fold accents (`place:ile-de-france`).
- `arv programs` lists the place data too; the packages suggest `iso-codes`.

**Scans told apart**
- `describe --ask` and `--clues` say how many of the PDFs they read have no text (scans), with what
  to do: `ocrmypdf --skip-text` (only the pages with no text), then describe again. Scans are tagged
  `source:scan`: the folder, when most of its PDFs are, else each scan, so `arv find source:scan`
  lists what is left to OCR.

**Tags in any script, one spelling each**
- A tag is folded as it comes in (drafts, `plan tag`, `retag`, `describe`, `tags add`): Latin,
  Greek and Cyrillic in lower case, Latin accents folded (`place:Zürich` and a decomposed
  `Zu\u0308rich` are both `place:zurich`, the gazetteer's), other scripts kept (`place:京都`). The
  spelling to read stays in a known name's `Label`. `arv find` folds the same way: "Zürich" finds
  `zurich`. Not done: Hangul written as separate jamo is not composed.

**arv programs: which optional programs are here**
- `arv programs [--json]` lists every program arv can use (its helpers; git, Siegfried and the
  compressors for making; exiftool, poppler and ffmpeg for describing; the browser opener; and the
  tools for work arv leaves to others: burning, ddrescue, OCR), which are found, what each is for,
  what is missing without it, and the packages to install (Debian, Arch). None is needed for a
  disc. The GUI shows them in its Settings; the Debian and Arch packages suggest the same programs.

**Models: found, chosen, and your own key**
- The GUI's Settings (in the header, beside Jobs) list the model servers on this computer (Ollama,
  llama.cpp's llama-server, LM Studio, vLLM, Jan) with their models and what each can do (pictures,
  embeddings, context), and the external providers whose keys are set (never the key). One is
  chosen for describing, and one for pictures, with a click; the choice is kept for this computer
  in `~/.config/arv/models.rec` (`arv-assist llm-status --all`, `arv-assist llm-use`). Each row says
  Ready or Not ready (and why), what is in use is marked, Model help has one On/Off for every
  model, and Test asks a fixed question, nothing from any file (`arv-assist llm-test`): did it
  answer, how fast, in JSON, or for a pictures model, did it see the red square. The Programs view
  moved there.
- Your own key: OpenAI, Anthropic, Google Gemini, Mistral, OpenRouter, Groq and DeepSeek through their
  OpenAI-compatible addresses, the key from the provider's usual variable or `ARCHIVE_LLM_KEY`, sent
  as a bearer token, never written down, never over plain http to another computer. https goes
  through curl (arv has no TLS), its options on standard input.
- A model on this computer is used as it is; an external provider only when chosen (the GUI asks
  first, and says what it will see), and then every use says so: on standard error, in the
  describe panels, and as "External AI" in the GUI's header. Pictures go only to a model on this
  computer.

**In the GUI: the questions, the names, the places**
- "Ask me (no model)" in the describe panels asks `--ask`'s questions in the page: the likely answers
  ticked, with why, a box to type others. `arv describe --answers FILE|-` is how: the answers so far
  in, the next question out as JSON, or the draft when none is left; what is learned (names,
  examples, rules) is kept only with the last answer.
- A Names view (Record): the known names made here, with rename, another form, merge, split (tick
  the uses that are the other one) and retire. `arv tags names [--json]` lists them;
  `arv tags show KEY --json` gives each use, in the order split asks about them.
- The Map view's form takes a place's box and reach.
- Likely duplicates offered for merging: `arv tags names` lists pairs of names in one namespace a
  letter or two apart ("Ann Smith", "Anne Smith"), initials ("A. Smith") or found as the same form
  ("Bob"); a name with a qualifier was told apart on purpose and is left out. The Names view offers
  each with merge either way, or "Not the same" (a qualifier each).
- `arv find place:japan` finds what is tagged with places in Japan too (Kyoto, Fushimi Inari), by the
  gazetteer's parents: the home's, a disc's own, or the built-in one.

**Places: a gazetteer, the photos' GPS, and a map**
- `config/places.rec`, copied into the home on first use: Natural Earth's 242 countries and 1,251
  populated places (public domain; `dev-tools/places.py` makes it), as known names of `place`, each
  with `Kind`, `Parent` (the place it is in), `Country` and `At` (ISO 6709). Discs carry it with the
  other vocabularies; a public disc leaves out the private places.
- `arv tags add place:KEY --parent PLACE --at LAT,LON --extent 'LAT,LON LAT,LON' --reach 2km --kind
  site`: a place of your own, its point, the box it covers (a park, a farm) and how near a photo must
  be to be there. One with a position or a box is private unless `--public`; they stay in the
  gazetteer, never in the record.
- `arv describe --ask` offers the gazetteer's places found in a folder's names (marked) or text
  (unmarked), saying which ("Lima, Peru"), and the place its photos' GPS is in (`exiftool`): a place
  of your own whose box holds it or within its reach (3 km unless it says), else a city within 40 km
  in the same country, else the country itself, by its outline (Natural Earth's 1:110m countries,
  built into arv-assist), so a photo across a border from a city is not put in it, and an enclave
  (Lesotho) is itself. A place you type becomes an entry, in the known place named with it or the
  place the GPS gives. Descriptions use labels.
- `arv map [--json | --geojson | --svg]`: the places the tags name, where each is (its own position
  or a broader place's), on how many paths and discs. Each disc's `index.html` shows its places on
  a map drawn from Natural Earth's land, with nothing to load; the GUI has a Map view.
- `describe --ask`: an answer can mix numbers and typed values (`1, fushimi-inari`).

**Known names: what --ask learns, kept as entries (authority control)**
- A namespaced tag can be an entry of the vocabulary: its key (made once), `Label`, `Alias`,
  `Qualifier` (two of one name), `Related`, `Same` (wikidata, viaf, isni), `Basis`, `Private`,
  `Retired`. `arv describe --ask` makes one when you confirm a person, family, organisation,
  software or work, offers known ones it finds in a folder's names and text, and takes one without
  asking when it is unambiguous, used before and its context agrees.
- `arv tags show|add|label|alias|merge|split|retire KEY`: each change an event about `name:KEY`;
  merge and split move the uses with ordinary tag changes (split asks about each). `arv tags` shows
  labels and qualifiers.
- People's and families' entries are private by default: left out of a public disc's vocabulary.
- The default vocabulary gains the namespaces `family`, `organisation`, `software` and `work`.

**Describing without a model: arv describe --ask**
- `arv describe FOLDER|DISC-ID|OBJECT --ask` asks a few questions instead of a model: the topics
  your vocabulary has, the event, places, people, what made it and when. Each offers the answers
  the files point to, marked: dates from their names or EXIF (`exiftool`, when it is there), a
  camera's model, the vocabulary's tags and aliases in the folders' names, values used before in
  the archive. Enter takes the marked one; a number picks; anything else is the answer. At most 20
  questions; it can go through the first-level folders too. The title and description are made
  from the answers, to accept or edit, and kept as the person's (`Authorship: human`).
- `--clues` takes the clues alone, as a draft (`automatic`); the GUI's describe panels have "Fill
  from the files (no model)".
- **It learns, by counting:** a word of a folder's names that went with a tag before marks that tag
  ("'kicad' in 9 of 10 folders tagged electronics"), and rare words are offered first, common ones
  left out (`cache/words.tsv`, from the record). Text files and PDFs are read too (`pdftotext`,
  `pdfinfo` when installed), their most telling words counted and a PDF's title put in the
  description; what `--ask` is told is kept (`config/word-examples.tsv`). A word that keeps going
  with a tag is offered as a rule, a `Match` in `tags.rec`.
- In text, the language's commonest words are left out (a built-in list of 1,500 English words, then
  the words most of the archive's described texts share), your vocabulary's words are kept, pairs
  that go together count as phrases ("self assessment"), and words written as names are ("Anne
  Smith") are offered as people and places.
- A draft applied to a data object now sets its own tags too (as `arv retag` would, a one-valued
  namespace's new value replacing the old).

**Packed objects: a plan can keep an object as one file**
- `arv plan pack NAME PATH tar+zstd` keeps that object on its disc as one file
  (`data/PATH.tar.zst`); `--disc N PACKING` packs each object of a disc, and `--together` puts a
  disc's packed objects in one `data/pack.*`; `none` keeps one as its files. Packings: `tar`,
  `tar+zstd`, `tar+xz`, `tar+gzip`, `tar+lzip`, `tar+bzip2`, `zip`, and for a file a compressor
  alone. arv writes the tar (the same files, the same bytes; each checked against its hash as it
  goes in); the compressor or `zip` is the system's.
- Format: `Object` gains `Packing` and `Pack`. `Path` is where the object unpacks to; `Tree`,
  `Files` and `Bytes` are its files', so the same files are the same version packed or not. A
  file added as it is that is an archive (`.zip`, `.tar.gz`, `.7z` ...) gets `Packing` alone.
- `plan show` (and `--json`) says how each item is packed and into what; `objects` names the pack;
  `status` counts files inside packs as on discs; `objects --json` gives `packing` and `pack`.
- Each disc lists the files in its packs, with their SHA-256 (`catalog/volumes/ID/packed.tsv`).
  `plan make` reads each pack back as soon as it is written; `arv check` of a disc's files reads
  inside its packs; `arv restore` unpacks each pack where it is, with its files' times.
- `plan show NAME --packed` packs what the plan packs, in a temporary folder, and gives each pack's
  size and the discs' fill with them (`packs` in `--json`); the GUI's "Packed sizes" asks it.

**The room on a disc, in the spec**
- The disc format spec gives each medium's sectors and how much image fits at 10, 20, 30 and 50%
  RS03 (18.7 GiB on a BD-R 25 at the default 20%), and how it follows from RS03's 255 layers.

**arv in the desktop's menu**
- `make install` (and the Debian and Arch packages) puts `arv.desktop` and arv's mark as its icon
  where desktops look. It runs `arv gui --idle 15`: started from a menu there is no terminal to stop
  it in, so it stops by itself once its page has been closed for 15 minutes and no job is running.

**Nearly everything without the command line**
- `arv gui`: Places (the tree and its temperatures: add, rename, move), What changed (`arv status`
  for any folder), a folder made a collection, who may see a disc (`arv access`), a data object
  renamed or retagged, and in Mastering an item's new place, its tags and a disc's layout. Owed is
  read from `arv todo --json`.
- `arv objects --json` gives each object's `name` now (after `arv rename`), `readAs` and `tags`;
  `arv plan show --json` each item's own `tags`.

**Every read has --json** (issue #31)
- `todo --json` (each finding with its `kind`, what it is about and the line a person reads;
  questions apart), `status --json` (what changed, every path), `info --json` (a disc, an image or
  a disc id) and `help --json` (every command, its usage and summary). The manual lists every JSON
  output.
- Exit codes stay 0, 1 and 2, and many things in a row are a shell loop: research/plan.md says why.

**The interface in C** (issue #48)
- `arv-gui` is a small C program: HTTP/1.0 on 127.0.0.1 with the same token and checks, the same
  five routes, a process for each connection, and jobs' output in files under `$TMPDIR`, removed
  when it stops. The page is read from `share/arv` (or beside it in a checkout) at each load, so
  it can be changed without building; a copy is also built in (`dev-tools/embed.c`, generated at
  build time, never committed) for `arv-gui` on its own. Python is now needed only for the tests.
- arv stays C99, now tested: `make -C src/arv check-tcc` builds it with TinyCC from the line every
  disc's README.txt gives, and the same checks pass (so do arv-assist and arv-gui).

**Output for scripts, versioned** (toward issue #31)
- Every JSON output starts with `"format": "arv-json 1"`: whose it is, and the version of its shape
  (within 1, fields are only added).
- `--json` on a command that changes the record (`burned`, `note`, `make`, `location add` ...)
  prints the events it made, each with its `EventId`; what it says to a person goes to standard
  error. A script learns a new copy's letter from the event, not from a sentence.

**The interface runs arv, and only arv**
- `arv gui`'s server no longer knows any feature: the page builds every command and the server
  runs it (`POST /api/run`), as a quick call or a job, and lists folders. A new feature is a
  change to the page alone, and the server is small enough to be rewritten in C (issue #48).
  Drafts reach `arv make` and `arv describe` on standard input (`--draft -`, `--apply -`).
- `arv location list --json`: the places, for the interface's inventory.

**The UDF standards by reference** (issue #32)
- The zip of UDF and ECMA standards (70 MB) is out of the repository: its terms were never written
  down. The UDF profile (`docs/spec/archival-udf.md`) links to ECMA-167 at Ecma and to UDF 2.50
  at the Internet Archive, OSTA's website having closed, and to Wikipedia's UDF article, which links
  the same collection the zip held.

**Copies, libraries and two catalogues in the interface** (issue #38)
- `arv gui`: a disc's copies can be given a barcode (a scanner types it in), a serial or a carrier
  (`arv identify`), and a new letter when two catalogues gave out one twice (`arv relabel`, offered
  for each). Search finds a copy by its barcode. Verify has Inventory (`arv inventory`: paste
  `mtx status` or scan a shelf) and Audit (`arv audit`: each finding with what it means). Rebuild
  takes another catalogue too. The disc's own Check form works again (it sent the drive as the
  wrong field).
- `arv find --json` lists the copies found by an identifier or carrier (`copies`), as the text does.
- `TWO DISCS` and `TWO BCA` say what to do: put the disc in a drive, and `arv drives` says which it is.
- The website says what copies are known by, how to use the archive on two machines, and how
  jukeboxes and tape libraries fit.

**Fast on a large catalogue** (issue #37)
- Every command reads the catalogue once and indexes it: a thing's events, a disc by its id, an
  object's records. At 10,000 discs (30,000 copies), `list`, `objects`, `todo` and `audit` take
  under a second, where at 2,000 they took from 9 seconds to over 5 minutes. `rebuild` merges by
  id instead of comparing every pair. Output is unchanged.
- `dev-tools/bigcat.py` makes a large synthetic home to measure on.

**Less to learn: a clarity round** (issue #42)
- Removed, each the work of another tool or unused: `make --ro-crate`, `--tools-history`,
  `--git-since`, `--ignore-names`, `--no-verify`, `--media`, `--sf-home`, `--extra-tools`,
  `--snapshot disc`; `plan refresh` and `plan add --copy`; `restore --no-links`; `verify files`;
  `tags --vocab`; `keywords --format tsv`; `init --pointer`; the copy identifiers `label:` and
  `hub:` (nothing used them; the copy letter is what is written on a disc); `arv models` (tag uses
  any OpenAI-compatible embeddings server, `--embed-url`). Formats are identified when `sf` is on
  `PATH` (`--no-formats` skips it).
- Old formats are no longer read: earlier disc-id schemes, copies recorded before letters, events
  written before ids. `arv audit` reports an event with no `EventId` (`NO ID`): arv cannot name or
  merge it.
- arv-gui reads only through arv (`--json`), not with a parser of its own; it no longer moves or
  deletes anything in an old home.
- In arv's JSON a field that can repeat (`Note`, `Subject`, `Category`, `Path`, `Agent`,
  `Identifier`, `Replaces`) is always a list.
- One way to do each thing:
  - `arv check SOURCE [DISC-ID]` checks any copy: a drive (every sector against the image's
    SHA-256), an image file (against the same; its RS03 data says which sectors when it differs;
    `--repair` mends it) or a disc's files, mounted or copied (every file against the manifests).
    It is logged, naming the copy when it is known, by its recorded path too. `--drive` reads a file
    as a whole disc (a ddrescue dump). `arv verify DISC`, `check --device` and `check --image` are
    gone, and `arv stored --copy X` at the copy's own path says to use `arv check`.
  - `arv info` is `id`, `info` and `ls` in one: a disc root (`--files`: its listing), an image, or a
    disc id.
  - A collection's revisions are its editions only: `arv checkpoint` (and `status --record`), which
    recorded a folder's state with no discs, is gone, with the `Stage` field. `status`, `log` and
    `diff` work on editions as before.
  - `--barcode` names the copy's own barcode everywhere; a tape's is `stored --carrier barcode:CODE`.
  - `todo --overdue` takes an age as `list --unchecked-since` does (`5y`, `18m`, `90d`), and both
    read a disc's last check the same way (a repair that made it whole counts).
  - Every command's `--help` says what it does; none share a line of the usage any more.
  - The README is shorter: what arv is, how to install it, a first disc start to finish, and the
    manual (`arv help --all`) and the workflow for the rest.
- Issue #42: remove `arv selection`, `Selection` records and selection appraisal targets. Snapshots
  and rebuilds no longer carry or merge selections; appraisals remain for discs, files, folders and sets.
- Issue #42: locations belong to copies only. `Disc.Location` and `make --location` are removed;
  `arv locate DISC-ID COPY PLACE` appends a copy-specific location change, and disc summaries and
  place filters derive their views from copies. `burned` and `stored` still set a copy's initial place.

**The whole manual, for people and agents alike**
- `arv help --all`: the manual as one markdown document, built from the same usage text as
  `--help`, so it cannot drift: every command, which only read and which change the record, exit
  status, the outputs meant for scripts, the rules for a script or an agent using arv, and the
  home's files. `arv help --man` is the same as a man page, which `make install` installs
  (`man arv`); `arv help COMMAND` is `arv COMMAND --help`. `make check` fails if the manual
  leaves a command out. No separate "LLM help": a model needs what a careful person setting up a
  script needs, all of it at once.
- `AGENTS.md` (and `CLAUDE.md`, which points to it): for agents working on arv's code: where the
  reasons are, the layout, how to build and test, and what is easy to miss (regenerating the
  reference outputs and `data.c`, committing a new `disc-tools.txt` path first). Both go on
  every disc with the source.

**Tags after burning are events**
- `describe --apply` and `tag --apply` no longer rewrite a disc's `tags.tsv` at home: the disc's
  file stays as burned, and the change is the event's (`Tags: PATH<tab>TAGS`, `Caption:`, with
  `Sets` and `Replaces`, one event an apply, only the paths that changed). So tag changes are
  appended, merge with another catalogue by event id, and two made apart are a conflict
  (`arv audit`, `arv rebuild`). `find`, `tags` and `keywords` read the tags now: as burned, with
  each path's newest change. A later disc's catalogue carries the other discs' tags as they are
  now, for readers that do not read events.

**Layouts: a disc's tree made from its objects' tags**
- `arv plan tag NAME SOURCE TAGS`, and `--list FILE|-` (`SOURCE<tab>TAGS` lines, all or none, as
  exiftool writes them): tags for files still on a drive, kept in the plan, checked against the
  vocabulary (one value of a one-valued namespace, a `date:` in EDTF). A file or folder has the
  tags of the folders above it and its own; of a one-valued namespace its own value replaces its
  folder's.
- `arv plan layout NAME LAYOUT [--disc N]`: a disc's tree from its items' tags, each item (a data
  object) placed whole in namespace folders: `year/2019/event/kyoto-trip/IMG_0001.JPG`,
  `project/weather-station/station/`. `/` nests, `,` falls back; an item with none of the
  alternatives keeps its arranged place. A date is filed by its start, and with no `date:` tag by
  its earliest file's time (`From: file-date`). Recorded on the disc (`Layout`); each object
  placed records where it was arranged (`Arranged`). `plan show` says where each item goes; two
  at one path stop `plan make` (`plan rename` gives one another name).
- `arv plan add NAME FOLDER --each`: each file in the folder an item, and so a data object, of its
  own, to be placed on its own (a photo is an archive's item; a folder of them added whole is one).
- The plan's tags go into the disc's `tags.tsv` (a folder object's once, on its folder); a `date:`
  tag counts for the disc's coverage instead of the file's time.

**A plan as a collection's next edition**
- `arv plan new NAME --collection CODE` (or `--collection` given to any plan action): `plan make`
  makes the plan's discs the collection's next edition, with its revision and manifest, as a
  make of the collection's folder would. The collection says what is kept; the plan, how this
  edition is arranged, so `diff` between editions shows the arrangement as moves.

**A data object's name and tags after burning**
- `arv rename OBJECT NEWNAME`: a data object's name on discs now (`Slug`), a name and never a
  place; `arv retag OBJECT TAGS`: its tags now. Both are events about the object (`Object:
  object:UUID`, with `Sets` and `Replaces`), so they follow it to every disc it is on and every
  disc made from it, not a path on one disc. OBJECT is its Uuid or name; two of the same name are
  refused, listed. The discs stay as burned.
- `describe --apply` and `tag --apply` record a change to an object's own path as its tags, so
  they follow it too. `find`, `tags`, `keywords` and `objects` read them, and a plan of the same
  source places it by its tags now (a layout), names it by its name now, and writes them into the
  new disc's `tags.tsv`.

**The disc's README for readers without English, and two more checks** (issue #41)
- A short note near the top of every disc's `README.txt` in ten languages (Chinese, Spanish,
  Hindi, Arabic, French, Russian, Portuguese, Japanese, German, Bengali): this is an archive disc,
  its files in `data/` are ordinary files, the instructions are in English and a translation tool
  or AI assistant can translate them, and the commands work as written. Drafts until a native
  speaker checks each (`research/plan.md`).
- VERIFY gains `shasum -a 256 -c manifest-sha256.txt` for macOS, and a line that counts the
  payload's bytes and files with `find`, `wc` and `awk`, to compare with `Payload-Oxum`: a missing
  or extra file, which `sha256sum -c` does not notice.
- `dev-tools/embed.c` writes bytes above 127 so that `data.c` compiles cleanly with UTF-8 data.

**A remake is a new plan, of the same objects**
- `arv restore` records where it restored a disc to (a `dissemination` event, in the home that
  knows the disc; the folder holds exactly the disc's files). A disc plan of a file or folder there
  continues the data object at the same path on the disc: the same bytes are another copy of its
  version, changed ones its next version, and a layout files it by its tags now (set on it, or its
  disc's at its path) and names it by its name now. Provenance, not bytes.
- `arv plan add NAME SOURCE --of OBJECT`: SOURCE continues that object (a copy you made of it,
  which arv cannot tell from its bytes); `plan show` says what each item continues.

**Byte-identical files reported**
- `make` notes files whose bytes another file on the same disc has (each is written in full), and
  `arv plan show NAME --duplicates` lists them, as arranged, before anything is made: dropping a
  copy is the person's choice. Sharing the bytes on the disc (UDF hard links) waits for this
  report to show it is worth a format feature, and for the first burns.

**Identity is the lineage, not the bytes**
- A data object is the same version as another only within its lineage (where it was read from).
  The same bytes read from elsewhere are a new object that records `SameBytes: UUID VERSION`; arv
  says so when it is made, and `status` says "its bytes are the data object ...". Before, any
  matching `Tree` merged two objects, though identical bytes can be two things with two
  provenances. `retire` names the same bytes on a disc that stays.
- `arv same OBJECT OTHER`: a person says two objects recorded apart are one (a copy made of it).
  An event about OBJECT, `SameAs: object:OTHER`; from then on OBJECT's records are read as copies
  of OTHER's versions with the same bytes, on every load, so merges and rebuilds agree. Refused
  for an object with bytes the other never had: a history of its own.

**Declared namespaces**
- The tag vocabulary (`config/tags.rec`) declares namespaces: `Namespace` records with
  `Description`, `Values: one|many` and `From: file-date` (where a value may be suggested).
  `date`, `event` and `project` take one value a file, its own replacing its folder's; `person`,
  `place` (places nest) and `source` take many. Vocabulary readers take only `Tag` records.
- `arv tags` says under each namespace what it holds and how many values, marks one that is not
  declared, and lists the paths to correct: two values of a one-valued namespace on one path, and
  a `date:` that is not EDTF (`date:2019-07`, `date:1995/2008`).

**Renaming in a plan, and lists**
- `arv plan rename NAME PATH NEWPATH`: an item's new place on its disc (a folder, all of it); a
  file or folder inside a planned folder is split off under the new name, as `git mv` would.
- `arv plan add NAME --list FILE|-`: `SOURCE<tab>PATH[<tab>DISC]` a line, as exiftool, a
  spreadsheet or a script sorted them; all are added, or none if any line is wrong.

**Splitting a folder in a disc plan**
- A file or folder inside a planned folder can be added on its own, under another name or on
  another disc. It used to go on the discs twice, silently; now the more specific of two items
  holding a file places it, and the folder goes without it. `plan show` lists what each folder
  leaves to other items; sizes count each file once; dropping the specific item puts the file
  back. The folder's data object records what it left out (`Except:`, format 0.5), so `arv
  status` compares the folder with the object without them.

**Tags on files** (format 0.5)
- A line of a disc's `tags.tsv` names a folder or a file, and a file has its folders' tags and its
  own, so a few files across folders can share a tag (`best`, `person:alice`). Drafts take a
  file's path (`make --draft`, `describe --apply`); `find` shows the file, `tags` counts files
  apart from folders, and `keywords` writes one file's keywords. "Best of" is a tag.
- A draft's tags and captions are `tags` and `captions` (were `folder_tags` and `folder_captions`):
  each maps a folder or a file to its tags, or its caption.

**Read without arv: notes on BagIt and recutils** (format 0.5)
- `docs/notes/bagit.md`: checking a disc with the Library of Congress's validator, which every
  disc carries (`tools/arv/upstream/bagit-python/bagit.py --validate .`), what the full and fast
  checks find, and why `--validate` must always be given. The disc's `README.txt` names it.
- `docs/notes/recutils.md`: querying the catalogue with `recsel`: where each disc is, every copy,
  a barcode's disc, a disc's history; and what only arv works out (counting copies, letters,
  conflicts). Read with recutils, change with arv.
- The record descriptors now say which fields name another record (`%type: Location rec
  Location` in `Event`, `Disc rec Disc` in `Event`, and `Parent` in `Location`), so `recsel -j`
  joins a copy to its disc and its place.

**Each copy has a letter** (format 0.5)
- A copy of a disc (burned, an image file, or a folder) is its `replication` event, now named by
  a letter, `Copy: A`, `B` ...: given in order (or chosen, `arv burned --copy X`), written on the
  hub and case after the id, and unique among the disc's copies. Copies stay sector-identical
  images, so two damaged ones can still repair each other; the letter is only on the label and in
  the catalogue.
- Each copy keeps its own place (`Location`), and its `MediaId` and `Bca` (a BD-R's factory
  serial, `--bca`) when known. `arv check --copy X` names the copy it read, and a passed check
  makes that copy known good. `arv todo` lists burned copies never read back by letter;
  `arv objects` and the GUI show each copy with its letter and place.
- `--copies N` records N copies (N events), and the disc's `Copies:` counter is gone: a disc's
  copies are counted from its events, so two catalogues of one archive can be merged without
  losing any.
- `dev-tools/disc-probe.c` asks a drive (Linux) for the media's maker and type and for the BCA
  serial, read only, blank or burned: step 7 of the first-burn drill. A Pioneer BDR-XD08 reads
  the BCA of Verbatim BD-R without AACS authentication, and two discs of a pack differ.
- So on Linux `arv burned --device` reads the disc's BCA serial and records it with the copy
  (`Bca:`, 32 hex digits), refusing a disc already recorded as a copy; `arv check --device` knows
  which copy is in the drive by it (and says so: `ID copy B: OK`). `--bca` gives it by hand.

**Easier to use** (a UX review: arv used from scratch as a new user would)
- `arv COMMAND --help` prints that command's own lines of the usage (and what DISC and CATALOG
  mean, when it uses them), not the whole of it. `arv make --help` keeps its full list.
- An unknown command says so and names the nearest (`arv stauts`: did you mean status?);
  arguments a command does not take are named, with that command's usage. Neither prints the
  whole usage any more.
- Commands come in four groups, as `arv --help` shows them: `arv archive`, `arv record`,
  `arv verify` and `arv home` each list theirs, and a group's name may come first (`arv record
  burned` is `arv burned`), for finding a command whose name you forgot; the short form stays the
  one the docs use. A command named under the wrong group says where it is.
- `arv` on its own says whether there is an archive here, and gives the five commands to start
  with, the groups and `arv --help`, instead of the whole usage.
- Tab completion for bash and zsh (`src/arv/completion/`, installed by `make install`): it asks
  arv itself (`arv __complete`), so it follows the commands, groups, subcommands and options of
  the arv installed, and falls back to file names.
- `arv gui` with no archive opens on **Make an archive** (a folder, a name, whether to use it from
  anywhere), which runs `arv init`, instead of stopping.
- **What discs carry in `tools/` is a whitelist,** `disc-tools.txt`: arv's source, tests, the tools
  the tests build, the docs and the reasons, about 5 MB. The UDF and ECMA standards archive remains
  in the repository for offline reference but is not carried on discs; upstream source is retained
  in the repository for planned community contribution. A new file goes on discs only when listed.
- **Reading a disc without a UDF reader:** every disc's `README.txt` says how ("WITHOUT A UDF
  READER"): each file is stored whole, so `dd` cuts it out of the image at the start sector the
  map gives (`catalog/volumes/<id>/extents.tsv`, kept at home and on every later disc), and the
  manifest beside it checks it. The spec documents `extents.tsv`, and `make check` cuts every file
  of a real image out this way.
- `arv make` with an installed or copied source copies only what `disc-tools.txt` lists and never
  build output; it used to copy its own work folder into itself when run from inside such a tree.
- `just install` / `just uninstall` (yourself, `~/.local`), `just install-system` /
  `just uninstall-system` (`/usr/local`, sudo), `just where`, `just deb`, `just arch`: no prefix to
  remember.
- Packages: `packaging/debian/` (a native Debian package, `just deb`) and `packaging/arch/PKGBUILD`
  (`arv-git`). `make install` works from a release tarball too (no git: the tree as it is, the
  commit from `COMMIT`, which `git archive` fills in), and `make clean` exists.
- `arv --version` (and `arv version`) names the commit it was built from and the disc format:
  `arv 0.4 (arv@<commit>), disc format 0.5`.
- `arv make` says before the long part how big the images will be once RS03 fills them to the
  medium, and how much room the output folder has; it stops at once, recording nothing, when they
  would not fit. RS03 (and `arv check --image`'s test) shows a percentage on a terminal.
- `arv todo` with no discs yet says so, and how to start, rather than "nothing owed".
- No more fallback home: with no `.arv` here or above and no default registered, arv stops and
  says how to make one (`arv init FOLDER`, `--name NAME --default`), as git does outside a
  repository, instead of quietly starting an archive in `~/.local/share/arv`. The older
  `~/.local/share/bluray-archive` and `$BLURAY_ARCHIVE_HOME` are no longer read either.
- `arv burned` without `--device` says when the disc already had copies recorded that day (run
  twice by mistake counts them twice). A location that looks like a code but is not one of the
  archive's (`--location ATIC`) is noted, on `burned`, `stored`, `locate` and `make`; it is still
  recorded as written.

- Plurals: "1 file", "3 issues", "1 link" (also in `arv list`, `arv status`, and a disc's README.txt and index.html).
- `arv burned` ends with what to write on the hub and case: `TRIP-01_2019_4 B`. `arv make`'s
  size line and the burned line are split in two, so they fit a narrow terminal.
- The docs follow: the website's demo is re-recorded (bare `arv` first, copies A and B, owed
  copies by letter), and the site, README, workflow and concepts say how copies are lettered,
  how `--device` finds the copy in the drive, and how to install, complete and uninstall arv.
- A logo: a disc from above, its three tracks Archive, Record and Verify, and the clear hub ring
  where the id and copy letter are written (`docs/img/logo.svg`, `logo-mark.svg`; light and dark),
  on the README, the website (and its tab icon) and `arv gui`'s tab.
- `arv gui`: named arv; Archive opens on "From a folder"; each disc in Owed opens it in Record ›
  Discs (to record copies and places); a long catalogue path is shortened in the header (the
  whole on hover); a collection's discs say which edition they hold.

**Data objects describe themselves; questions wait for an answer** (format 0.5; research/plan.md, 2026-10-12)
- When a data object is archived, arv reads what its own files say about it, by rules and with no
  model: `Title` from its README's first heading (or a package manifest's name), `Description`
  from the manifest's description (or the README's first paragraph of prose), `Rights` from the
  manifest or a `LICENSE`/`COPYING` it recognises, and for a git repository `Coverage` from its
  first to its last commit and `Origin`, where it was published (`host/path`, no credentials).
  No author names or emails. `arv objects`, `arv find` and the GUI's Objects tab show them; every
  disc carries them in its `Object` records.
- `arv describe NAME` describes a data object with a local model, as a disc is described, and
  `--apply` takes a draft for it. A disc's draft may describe its objects (`"objects"`, by path).
  The event names the object (`Object: object:UUID`), full snapshots carry it, and later versions
  keep what a person or a model wrote.
- A model's questions in a draft made while nobody was there to answer are kept (in the home's
  `drafts/questions.rec`, never on a disc; at most 5 a disc or object), and so is arv's own about a
  git repository with no README. `arv todo` lists them last, never as owed; `arv describe ID
  --answer` asks them with no model (answers become notes, Enter skips one for good); and
  `arv describe ID` at a terminal asks them before the model's own.
- Not added: a chat agent in arv. `describe` already has its useful part, and an agent working
  through the archive belongs outside arv, writing drafts (research/plan.md says why).

**Event ids, and the discs as witnesses** (format 0.5)
- Every event has an `EventId`: the hash of what it says, as written (PREMIS's eventIdentifier).
  The same event has the same id in every catalogue and on every disc, so `arv rebuild` merges
  events by id; two events with the same content are kept apart by a `Nonce`. Older events get
  their id when the catalogue is next written.
- `arv audit` checks every event against its id, and with discs given (`arv audit /media/disc`),
  the home against what each disc carries: an event changed or lost at home is listed. A burned
  disc is write-once and kept elsewhere, so it witnesses what was written.
- `arv rebuild` merges another catalogue of the same archive too (its `.arv`, or the folder that
  holds one), by event id; notes on a disc add up from both. What both gave out is reported: a
  disc id naming two different images is not merged (exit 1), and two copies lettered alike are
  named.
- **A copy of the catalogue takes no changes.** `config/paths.rec` records where the catalogue
  lives; a change written to a copy elsewhere stops, with what to do (`arv make` stops before the
  long part). `arv where --here` accepts a new place (the catalogue moved, or a share is mounted
  at another path); `arv where` lists the places, and says when this one is not among them.
- research/plan.md (2026-10-10): the catalogue's history is per thing, not a line; catalogues
  merge by event id; tampering is made evident, with the discs as witnesses.

**Scanning barcodes**
- `arv find` searches copies by what they are known by: `arv find ARV-000123` (a scanned case)
  says which disc and copy it is, where it is kept and when it was last checked; a tape's
  barcode finds every copy on it.
- `--barcode CODE`, for a handheld scanner (it types like a keyboard): on `burned`, `check`,
  `identify` and `relabel` it is `--identifier barcode:CODE`; on `stored`, the tape's
  (`--carrier barcode:CODE`). `arv inventory PLACE` read from the keyboard is a hand shelf read.
- docs/burning.md: barcodes go on the case, not the disc, and what a case's barcode does and
  does not prove.

**Relabelling copies, and conflicts between catalogues** (format 0.5)
- An event about one copy names it by `Replication` (its replication's EventId, which never
  changes) as well as its letter; a copy's letter is the newest given. `arv relabel DISC-ID COPY
  NEW [--at PLACE | --identifier ID]` gives a copy a new letter, keeping its history: for two
  copies lettered alike by two catalogues (rebuild says which command to run), or a label written
  wrong. Letters once given are not given again.
- A change to a field (a disc's access, places, title, description or subjects; a place's name or
  parent or temperature) says `Sets: FIELD` and `Replaces:` the
  changes it follows. Two catalogues that each changed a field are a `CONFLICT`, reported by
  `arv rebuild` and `arv audit`; the home's value stands until the field is set again, even to
  the value it has, which settles it.

**Jukeboxes and tape libraries**
- `arv todo --tsv`: what is owed, one copy a line (no-copy, read-back, overdue, missing,
  one-carrier), with its form, identifiers, carrier, place, path and last check: a worklist for a
  script.
- `arv inventory [-v] PLACE [FILE]` reads a library's barcodes (`mtx status`, or a plain list)
  and reconciles them with the copies recorded at PLACE: missing (exit 1), unknown, recorded
  elsewhere, and recorded there with no barcode. It reports and records nothing.
- `docs/examples/library-check.sh`: an example `mtx` loop (on every disc too) that loads each
  due copy by barcode, checks it (a disc in the drive; a copy on an LTFS tape at its path), and
  unloads it, one load a tape. `-n` shows what it would do.
- `arv stored ID PATH --copy X` at the copy's own path checks it again (a check, no move).

**Copies on tapes and drives: carriers** (format 0.5)
- A stored copy may name its `Carrier`: the tape or drive that holds it and other images
  (`arv stored ... --carrier barcode:TAPE01L8`, or later `arv identify ID X --carrier ...`).
  Many discs on one tape is no clash; two copies of one disc on one carrier are lost together,
  so they count as one (`arv objects`, `arv todo`, safety), and `arv todo` and `arv stored`
  say so. A copy moved to another tape (`arv stored --copy X --carrier ...`) counts again.
- `serial:` joins the identifier schemes: a manufacturer's serial (a hard drive's, an LTO
  cartridge's memory), naming one medium like `bca:` and `barcode:`.

**What is in the drives, and where the stored copies went**
- `arv drives [--check] [DRIVE...]` names the disc and copy in each optical drive (by its BCA
  serial, or as the only burned copy), and says when a drive is empty or holds another disc.
  Linux finds the drives; elsewhere they are named. `--check` checks each arv disc found.
- `arv info IMAGE.iso` says which disc an image file is and whether it is a recorded copy, so
  `find ... -exec arv info {} \;` looks through a drive or NAS (arv does not crawl disks itself).
- `arv todo` lists stored copies not found at their recorded path. `arv stored ID NEWPATH --copy
  X` checks a moved copy at its new path and records the move (and a new place with
  `--location`), instead of recording a new copy.

**Copies known by what is on them** (format 0.5)
- A copy's **identifiers** say how that physical disc is recognised, as `scheme:value`:
  `bca:` (its factory serial, read by the drive), `barcode:` (a sticker, scanned), or `serial:`
  (a manufacturer's serial). Repeatable, and added later by any event that names the copy.
  `Identifier: bca:...` replaces `Bca:`.
- `arv burned` and `arv check` take `--identifier` (repeatable; `--bca` is gone). `arv identify
  DISC-ID COPY ID...` adds one later (a sticker put on, a serial read on another machine). An
  identifier already on another copy is refused, as is a second BCA serial on one copy.
- Copies are counted as **physical discs**: two records that share a `bca:` or `barcode:` are
  one disc recorded twice (in two catalogues, say), counted once in `arv objects`, `arv todo`
  and safety. `arv audit` and `arv rebuild` report a disc recorded twice, an identifier on
  copies of two discs, and a copy with two BCA serials.
- `MediaId` is now `MediaType` (`--media-type`): the maker and type many discs share, not one
  disc's identity.
- `arv objects DISC-ID` and the GUI show each copy's identifiers.

**Copies in the GUI, and one disc's copies**
- `arv objects DISC-ID` lists a disc's copies, each by its letter, with its form, place, when it
  was last checked and its BCA serial; `--json` gives the same (and `arv objects --json` now
  carries `at`, `made`, `checked` and `bca` for every copy).
- `arv gui`, a disc's details: its copies in a table by letter; recording a burned copy (how many,
  where, the letter, and with a drive named, read back first), a copy on a drive or NAS
  (`arv stored`), and checking one copy (by letter, or found by its BCA serial; an image can be
  repaired). The Check tab takes a copy letter and repair too.
- The GUI's Collections tab counts a disc read back when a copy was checked since it was burned,
  as `arv objects` does; it used to count only a read-back at burning.

**Describing from anywhere**
- A draft (the JSON `arv describe --save` writes) is documented as the way in for anything that
  describes the archive: a person, a script, or a model that is not arv's. README, "Describing
  from anywhere", lists its keys.
- `arv describe DISC-ID --apply -` reads the draft from standard input. `--suggested` keeps a
  model's draft a suggestion, for a script that runs without anyone reading it; applying used to
  always record it as accepted by a person.
- Applying folder tags merges them into the disc's: the folders named change, the others keep
  theirs. They used to be replaced, so tagging one folder dropped every other folder's tags.
  A folder the disc does not have is skipped with a warning.
- Not added, on purpose: tags on single files and typed links between items
  (research/plan.md, 2026-10-10).

**Discs carry the vocabularies** (format 0.5)
- A disc with a full catalogue snapshot carries the home's `config/sets.rec` and
  `config/tags.rec` in `catalog/config/`, so the words its records use travel with it.
  `arv rebuild` restores them into a home that has none, and says so when the home's differ.

**`arv make FOLDER` records a data object**
- A folder that is not a collection's workflow folder and fits on one disc is a data object, as
  a plan's items are: `arv objects` lists it, `arv status FOLDER` says whether it changed since,
  and making it again after a change records the next version. A folder split over several discs
  is not one (its parts are on different discs).

**Laying a disc out** (issue #23)
- docs/workflow.md, "Laying the folder out": the top level is kinds, from the set vocabulary; the
  level below is a fact in the data (year of capture, project, record type, source); below that,
  only a boundary that already exists. Interpretation stays virtual, and a disc's layout changes
  only with a new edition of its content. docs/philosophy.md calls the disc's layout the third
  order, beside the shelf and the catalogue. Advice only: arv still never moves files.
- `arv status FOLDER` and `arv make` note top-level folders whose names say nothing about what
  they hold (`misc/`, `New folder (2)/`), and, when most of the top level is kinds from the set
  vocabulary, the folders that are not. Folders named by a year count as facts; a repository's
  own tree is left alone. Nothing is recorded, so the format does not change.

**Retiring one disc, and a data object's last copy**
- `arv retire DISC-ID` retires a disc that is not part of an edition (a disc plan's, or
  `arv make FOLDER`'s). It refuses while any of its files is on no other disc that stays, and names
  each data object on it: the version, whether another disc has it, and whether it is the newest.
  An older version that a newer one replaces may go; the only copy of the newest is refused.
  `--yes --accept-loss` records the files as `Lost:` on the disc, and `arv find` lists them as
  `LOST`. An edition's disc is refused: editions are retired together (`arv retire CODE`).

**Collections in the GUI**
- Record › Collections lists each collection's editions: their discs and copies, and whether each
  edition is safe, replaced (ready to retire), kept or retired (with files lost). Keep runs
  `arv collection keep`; Retire shows `arv retire`'s own preview first and records nothing until
  confirmed, and a loss only when "accept the loss" is ticked.

**What of a plan is on discs already**
- `arv plan show NAME --archived` hashes each item (through the hash cache) and says what is on
  discs already: an item archived before (which data object version, on which discs), every file
  of it on discs from elsewhere, some of its files, or none. `--json` carries it as `archived`.
  The GUI's Mastering tab has a button for it ("What is on discs already?") and flags each item.

**Format identification for disc plans**
- `arv plan make` identifies formats with Siegfried when `sf` is on `PATH`, as `arv make` does;
  `--no-formats` skips it. Each item is identified where it is, with rows in `formats.csv` at its
  place on the disc.

**Icons for kinds of file in the GUI**
- The Mastering file browser, the plan's discs, Search results and Objects show an icon for the
  kind of file (📷 image, 🎬 video, 🎵 audio, 📄 document, 📊 spreadsheet, 🗜️ archive, 💿 disc
  image, ⌨️ code; 📁 folder), with a label for screen readers. Guessed from the extension, as a
  hint only: nothing is recorded, and Siegfried's `formats.csv` stays the record of formats.

**Appraise first: the levels ask for care** (issue #24)
- Each importance level now asks for a number of copies and places: essential, 3 copies in 3
  places; important, 2 in 2; useful, 1; incidental, none (everyday storage is enough).
  `arv todo` reports a disc holding less than the highest level in force on it, on anything on
  it, or on its set; a model's unreviewed suggestion does not count. A disc not yet appraised is
  held to two places, as before.
- docs/philosophy.md: "Appraise first: most things are not preserved", with the levels, what
  each asks for, and the order of the work.

**Archive, Record, Verify: the interface follows the name**
- `arv --help` groups every command under Archive (what goes on discs), Record (what exists, and
  where) and Verify (still good, and can be got back); the commands themselves are unchanged.
- The GUI has three tabs to match, each with its views (Archive: Mastering, From a folder; Record:
  Objects, Discs, Search; Verify: Owed, Check, Restore). It opens on Verify's Owed (`arv todo`), and
  a count of what is owed sits above the tabs. Jobs open from the header. New in the GUI: verifying
  and restoring a disc's files.

**Safer making, and a portable arv**
- `arv make` checks each file's bytes against its manifest as they are written to the image; a
  file changed after hashing stops the make and nothing is recorded (before, the disc would have
  failed `arv verify` later).
- arv finds a `.arv` home beside itself (`tools/arv.com` and `.arv/` on one drive), so it runs
  from a portable drive without a machine config and writes nothing outside the drive.

**Disc plans: discs composed by hand** (no format change)
- `arv plan` composes discs from files and folders anywhere (a film on the PC, photos on the
  NAS): which goes on which disc, and where under `data/`. A plan points at its sources and copies
  nothing; `arv plan show` measures them against each disc's room, and `arv plan make` makes
  one image a disc (`arv make --plan`). The GUI's new Mastering tab is the same, with drag and drop.
  Plans point at their sources; `plan show` and `plan make` say which items changed since they were
  planned. Sources inside the home's drive are kept relative.
  A made plan is kept as a template: `arv plan list` hides it unless `--all`, and `arv plan again
  NAME NEW` starts the same plan again as a new plan (the GUI: "Make it again").
- Each planned file or folder becomes a **data object** (`Object` record, format 0.5): known by
  the hash of its content, in linked versions, with no `.arv` marker. `arv status` says whether
  each object archived from a folder is unchanged, changed or gone, and recognises a moved folder
  by content; `arv find` lists objects. Source paths stay in the home catalogue.
- `arv objects [NAME]`: everything kept and where every copy of it is (each data object's
  versions, each collection's newest edition; their discs, every copy's form and temperature,
  read back or not; the original on the PC/NAS). `arv todo` also lists data objects whose newest
  version has no cold copy, or that are no longer where they came from with fewer than two copies.
  The GUI's new Objects tab shows the same (`arv objects --json`).
- `arv retire --yes` refuses while any file is on the retiring discs only; `--accept-loss`
  retires anyway and records those files as `Lost:` on the edition (`arv log`, `arv find`).
  `arv find` marks files on retired discs `[retired DATE]`.

**Collections over time** (format 0.5; design in research/plan.md, being built)
- A *collection* is something kept and made into discs again and again, from one workflow
  folder with one history: `Collection` records (with a Uuid) and `Revision` records (git-like:
  `Tree` and `Node` hashes; checkpoints, and editions, each a set of discs holding part of the
  collection and a copy of the catalogue; `--keep` for one never to retire).
- `arv burned --device DRIVE` reads a burned copy back against its image and records it only
  if identical (arv never drives the burner: `arv make` prints the command). An edition is safe
  once each disc has such a copy; `arv retire CODE` then retires the editions it replaces
  (not kept ones), after listing any files found only on them. `arv todo` lists what is owed.
- Copies have a form (disc, iso, folder) and a temperature: hot (in active use), warm (online
  or reachable, left alone: an image on a NAS) or cold (offline: discs on a shelf); places carry
  a default (`arv location add|move --temperature`), and `arv todo` lists discs with no cold copy.
- Git repositories go on a disc as working files plus a compacted `.git` (one pack; no hooks,
  reflogs but the stash's, or credentials in remote URLs); `git.tsv` lists each repository's roots,
  heads and commits. `arv status` recognises a repository by its history (archived, ahead of a
  branch on a disc, diverged, not archived) and `arv find COMMIT` names the discs holding a commit.
- `arv stored DISC-ID PATH`: a copy kept on a drive or NAS, as the image file or as a folder,
  checked (read back, or verified as the bag) and then recorded; warm unless its place says.
- Each home catalogue (archive) has an identity: a `Home` record with a Uuid, made by `arv init`;
  every disc names it (`HomeUuid`), a home rebuilt from a disc keeps it, and `arv rebuild`
  refuses a disc of another archive unless `--any-archive`. `arv where` shows it.
- `arv collection init|list|show`; `arv make` on a workflow folder makes the next edition.
  `arv status` (what changed since the last revision, or for any folder which files are on
  which discs), `arv checkpoint`, `arv log`, `arv diff`, `arv link [--past]`; a hash cache
  (path, device, inode, size, time) so a second `status` reads nothing (`--deep` rereads).


**Making discs**
- One standard output: a closed **UDF 2.50** image by arv's own writer (src/udfwrite), with a
  real metadata mirror, contiguous files in a fixed order, and reproducible bytes
  ([docs/spec/archival-udf.md](docs/spec/archival-udf.md)). Hybrid ISO 9660 images and the NetBSD makefs writer are gone; makefs lives on in `upstream/` for the fixes offered to NetBSD.
- **RS03 error correction added by arv itself** (src/rs03), byte for byte what dvdisaster
  writes, then every sector tested. No dvdisaster needed to make a disc.
- Every disc carries arv's source and **`tools/arv.com`**, one executable for Linux, macOS,
  Windows and the BSDs, that verifies, restores, searches and repairs.
- The home catalogue records each finished image's size and SHA-256 (`ImageSectors`,
  `ImageSha256` in its Binding record).

**Checking and repairing**
- `arv check --device` reads a burned disc back against its image hash, past the system's cache.
- `arv check --image IMAGE --repair` repairs a damaged image in place, as dvdisaster does and to
  the same bytes, and works on an image that is in no catalogue at all.
- `arv list --unchecked-since 5y` and `--one-place`: the discs due a check, and those kept in
  only one place.

**Documentation for the long term**
- [docs/spec/rs03-format.md](docs/spec/rs03-format.md): the error-correction format, written so that a
  repair tool can be written from it alone (checked by doing exactly that), with test vectors.
- [docs/burning.md](docs/burning.md): burning, checking the burn, and the drill for a new drive
  or media.
- Each disc's `README.txt` explains reading a damaged disc with ddrescue or dvdisaster and
  repairing it with the arv on the disc.

**The program**
- arv is a C99 program with no libraries (src/arv). It was ported from the Python arv one
  command at a time and held to the same outputs (tests/reference), and the Python core was
  then removed. The optional local AI helpers (`describe`, `tag`, `models`) are arv-assist, in C
  too (src/arv-assist); only the optional web interface, arv-gui (src/arv-gui), is in Python.
  `make` builds everything; arv runs the two helpers for their commands.
- arv's own BagIt validator and digests (src/bagit; also a `bagit` program for any bag), checked
  against the Library of Congress's bagit.py; the disc no longer carries `tools/bagit.py`.

## Earlier formats

From the spec's versioning section:
- **0.5** (2026-10-05): the new `Collection` and `Revision` records. `Selection` was added in
  the draft and removed before 1.0 (issue #42); no discs have been burned.
- **0.4** (2026-10-03): `Authorship` on events, `Appraisal` records, listing version 2 (links,
  executables).
- **0.3**: the medium's fields moved into `Binding` records.
- **0.2**: per-volume files grouped by volume (`catalog/volumes/<id>/`).
- **0.1**: the first sample discs, never burned; per-volume files kept by kind.

The design and the reasons behind each decision are in [research/plan.md](research/plan.md).
