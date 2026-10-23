# arv gui, view by view

`arv gui` is arv in your web browser: a page served on 127.0.0.1 that runs the same commands as the
terminal and shows what they printed. Here is every view, with a picture, so you can find your way
round it before you start. How it works inside: [src/arv-gui/README.md](../src/arv-gui/README.md).

The pictures are of the sample archive ([samples/](../samples/README.md)): six small discs, their
copies and places, and the folders they were made from. Where a view shows nothing until it is used,
it was used first: a plan, a folder looked at, a search, a job. `sh dev-tools/screenshots.sh` takes
them all again after a change.

The header is on every view: the catalogue in use, how many things are owed (it opens Verify ›
Owed), the Assistant, Settings and Jobs.

## First run: Make an archive

![Make an archive: the suggestion of one archive for the computer, the home folder filled in with Browse and Where arv gui started beside it, the name main, the box to use it from anywhere ticked, and Make the archive](screenshots/gui/setup.png)

With no archive in the folder it was started in, or above it, and no default, `arv gui` offers to
make one (`arv init`): a folder, an optional name, and whether to use it from anywhere. The
interface then opens on it.

It suggests one archive for the whole computer: `.arv` in your home folder, named `main` and used
from anywhere (`arv init ~ --name main --default`). Then every folder you archive and every disc
you record lands in the same catalogue, wherever arv is started, the menu entry included. A
narrower one, for a portable drive or a NAS share, goes at that folder's root instead: Browse to it
(or "Where arv gui started") and untick the box. Inside that folder, its own `.arv` is used.

## Archive: onto discs

### From a folder

![Archive › From a folder: the Kyoto trip's folder chosen, its title, description and subjects filled from its files, with fields for the creator, rights, a note and tags below](screenshots/gui/make.png)

One folder made into discs (`arv make`). Its title, description, subjects and tags can be filled
from the files' names, dates and kinds, or from a few questions, with no model; or by a model, if
one is set up. Below them: the disc it is for, the error correction, and whether to split it across
discs.

### Mastering: discs by hand

![Archive › Mastering: the plan HOLIDAYS, with two discs, each with a fill bar and its folders (the Kyoto trip, 16 of 17 files on discs already; 2021 Beach and 2021 Christmas, every file on discs), and a file browser on the right](screenshots/gui/master.png)

Discs composed by hand from files and folders anywhere (`arv plan`): drag them from the browser on
the right onto a disc, or tick them and press Add. Each item can be moved, renamed, tagged and
packed; each disc has a fill bar and a layout. "What is on discs already?" marks what earlier discs
hold. Make the disc images when it is ready.

### What changed

![Archive › What changed: the Kyoto trip's folder, not a collection's: 16 of its 17 files are on discs, most like TRIP-01_2019_4, and one file, notes, day 4.txt, on no disc](screenshots/gui/status.png)

A folder against what is archived (`arv status`): a collection's folder against its last edition,
or any folder against every disc. Here, the Kyoto trip's folder, with a file added since its disc.

## Record: what, and where

### Objects

![Record › Objects: the collection Family photos, two editions, its newest on FAMILY-02_2020-2021_I, with copy A a cold disc and copy B a warm image read back, kept at the parents' house and on the NAS](screenshots/gui/objects.png)

Everything kept, and where every copy of it is: each data object's versions and each collection's
newest edition, the discs holding them, and each copy, cold, warm or hot, and whether it was read
back. Renamed and retagged here (`arv rename`, `arv retag`).

### Collections

![Record › Collections: Family photos, edition 2 safe with two copies and a Keep button, edition 1 retired, and a form to make a folder a collection](screenshots/gui/collections.png)

Each collection and its editions (`arv collection`). An edition is safe once each of its discs has a
copy read back; a newer safe edition replaces older ones, unless they are kept. Any folder can be
made a collection here (`arv collection init`).

### Discs

![Record › Discs: the six sample discs, each with its title, date, files, copies, where it is kept and its last check](screenshots/gui/discs.png)

Every disc (`arv list`): its title, its copies, where they are kept and when it was last checked.
Click a disc to open it:

![One disc opened, the whole page: TRIP-01_2019_4, Kyoto, July 2019: its details, who may see it, notes, its copies A and B, what each copy is known by, forms to record a burned copy, a copy on a drive or NAS, a copy's new place and a check, Improve the description, and its history](screenshots/gui/discs-detail.png)

What it says it is; who may see it (`arv access`); notes (`arv note`); its copies by letter, and
the forms that record one (`arv burned`, `arv stored`), name it by a barcode or serial
(`arv identify`), move it (`arv locate`) or check it (`arv check --copy`); its description improved
(`arv describe`); and its history, every event.

### Places

![Record › Places: the tree of places, Home with Study and Box 1, the fire safe and the NAS, and Parents' house, each cold, warm or as above, and a form to add a place](screenshots/gui/places.png)

The places copies are kept in, as a tree, each cold, warm or hot (`arv location`). A copy's place
says how safe it is: `arv todo` asks for a cold copy, and copies in two places.

### Map

![Record › Map: a map of Japan with Kyoto marked](screenshots/gui/map.png)

Where things are from: the places their tags name (`place:kyoto`), on a map (`arv map`). Places of
your own are added below it (`arv tags add`).

### Names

![Record › Names: no known names yet, and a form to add one: a tag such as person:anne-smith, a name, and what tells it apart](screenshots/gui/names.png)

The people, families, organisations, software and works you have confirmed (`arv tags`): each a key
that stays and a name that can change. Describing makes them as you answer. The samples have none.

### Search

![Record › Search for beach: two discs, Family photos editions 1 and 2, and four files in 2021 Beach on FAMILY-02_2020-2021_I, with where they are kept](screenshots/gui/search.png)

Files and discs by a name, words in a path or a glob, and a copy by its scanned barcode (`arv find`).

## Verify: still good, and back

### Owed

![Verify › Owed: copies burned and never read back, a stored copy not found where recorded, discs with fewer copies or places than their appraisal asks, and discs kept in one place, each a link to the disc](screenshots/gui/owed.png)

What is owed (`arv todo`), under headings that say what to do; a disc's name opens it. The questions
waiting for an answer are listed too, never counted.

### Check

![Verify › Check: a drive, an image file or a disc's folder, an optional copy letter, repair the image, and Check](screenshots/gui/tools.png)

A copy read back (`arv check`): a drive, every sector against the image; an image file; or a disc's
files, against their checksums. It runs as a job.

### Inventory

![Verify › Inventory: a place, a box for the barcodes read there, and Compare](screenshots/gui/inventory.png)

What a jukebox, a tape library or a shelf holds, against the copies recorded there
(`arv inventory`): what `mtx status` printed, or barcodes scanned one a line. Nothing is recorded.

### Audit

![Verify › Audit: the catalogue's events, none changed since written, and Nothing found wrong](screenshots/gui/audit.png)

Every event against its id, copies recorded twice, and fields two catalogues changed apart; with
discs, the home against the catalogue each carries (`arv audit`).

### Restore

![Verify › Restore: restore files from a mounted disc to a new folder, and rebuild the catalogue from a disc or merge another](screenshots/gui/restore.png)

Files copied back from a disc (`arv restore`); the catalogue rebuilt from a disc, or another
catalogue of the same archive merged in (`arv rebuild`).

## Settings

### Models

![Settings › Models: model help on, the describing model not ready (no server running), and the model servers on this computer (Ollama, llama.cpp, LM Studio, vLLM, Jan), none running](screenshots/gui/models.png)

Which model helps with describing, and which looks at pictures (`arv-assist llm-use`): a model
server on this computer, or an external provider with your own key, used only when you choose it.
None is needed: here, none is running.

### Programs

![Settings › Programs: arv's own helpers, here, and the programs for making discs, each here or missing, with what it adds](screenshots/gui/programs.png)

The programs arv can use and which are here (`arv programs`), with the command that installs the
missing ones. None is needed to make, read, check or repair a disc.

## Jobs

![Jobs: arv plan make HOLIDAYS -y --no-ecc, finished, with what it printed: the items hashed, thumbnails made, two images built and hashed](screenshots/gui/jobs.png)

What takes a while (making images, checking a copy, restoring) runs as a job: its output followed
here as it comes. Here, the plan above, made without error correction.

## Assistant

![Record › Discs with the Assistant open on the right: asked whether the Kyoto trip is safe, it ran arv find place:kyoto and arv todo --json, answered where both copies are and that neither has been read back, then proposed a check of copy A, which waits for Apply](screenshots/assistant.png)

From the header, beside any view (`arv-assist chat`): it reads the record on its own; a change it
proposes waits for you to apply it, as it came or edited. Its reads in this picture are real; the
model's replies are scripted.
