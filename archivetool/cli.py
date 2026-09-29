"""archive: build self-describing, error-corrected archival disc images.

Commands:
  make   bag a folder, write the catalogue + viewer, build the image, add RS03 ECC
  find   search every disc's catalogue and file list (no discs needed)
  list   list discs in the home catalogue
  note   add a note to a disc
  locate set where a disc is physically stored
  burned record that copies were burned
  check  verify a disc or image with dvdisaster and log a fixity-check event
  rebuild merge the catalogue carried on a disc into the home catalogue
  index  build the SQLite search index
  describe  improve titles, descriptions and tags with a local LLM (optional)
  tag    suggest folder tags from your tag vocabulary (small built-in model)
  models fetch / check the built-in model
  gui    graphical interface in your web browser
"""

import argparse
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile

from . import bag, catalog, describe, discid, image, index, llm, make, media, models, recfile, sets, tagger

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_NAME = "bluray-archival-workflow"


# ---------------------------------------------------------------- helpers

def log(msg):
    print(msg, file=sys.stderr)


def ask(prompt, default=None, interactive=True):
    if not interactive:
        return default
    suffix = " [%s]" % default if default else ""
    try:
        answer = input("%s%s: " % (prompt, suffix)).strip()
    except EOFError:
        answer = ""
    return answer or default


def folder_defaults(src):
    """Title/set defaults from a folder named like 2025-01-13_Projects_2020_-_2025."""
    name = os.path.basename(os.path.normpath(src))
    stripped = re.sub(r"^\d{4}-\d{2}-\d{2}_", "", name)
    title = " ".join(w[:1].upper() + w[1:] for w in stripped.replace("_", " ").split())
    first = re.sub(r"[^A-Za-z0-9]", "", stripped.split("_")[0]) or "ARCHIVE"
    return title or name, first.upper()


def software_version():
    """Return (version string, is_git_checkout)."""
    if shutil.which("git") and os.path.isdir(os.path.join(REPO_ROOT, ".git")):
        try:
            commit = subprocess.run(["git", "-C", REPO_ROOT, "rev-parse", "--short=12", "HEAD"],
                                    capture_output=True, text=True, check=True).stdout.strip()
            dirty = subprocess.run(["git", "-C", REPO_ROOT, "status", "--porcelain"],
                                   capture_output=True, text=True, check=True).stdout.strip()
            return "%s@%s%s" % (REPO_NAME, commit, "+uncommitted" if dirty else ""), True
        except subprocess.CalledProcessError:
            pass
    return "%s@unknown" % REPO_NAME, False


def stage_tools(tools_dir, is_git, extra_tools, history=False):
    """Copy this tool (a snapshot of the last commit), bagit.py and any extra tools onto the disc.

    The repository's history is only added when ``history`` is set (as a git bundle), so
    files removed from the repository never keep riding along on new discs.
    """
    os.makedirs(tools_dir, exist_ok=True)
    tree = os.path.join(tools_dir, REPO_NAME)
    if is_git:
        data = subprocess.run(["git", "-C", REPO_ROOT, "archive", "--format=tar", "HEAD"],
                              capture_output=True, check=True).stdout
        with tarfile.open(fileobj=io.BytesIO(data)) as tar:
            if hasattr(tarfile, "data_filter"):
                tar.extractall(tree, filter="data")
            else:
                tar.extractall(tree)
    if is_git and history:
        bundle = os.path.join(tools_dir, REPO_NAME + ".bundle")
        proc = subprocess.run(["git", "-C", REPO_ROOT, "bundle", "create", bundle, "--all"],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            log("Warning: git bundle failed, disc gets the plain tree only:\n" + proc.stderr.strip())
            if os.path.exists(bundle):
                os.remove(bundle)
    if not is_git:
        shutil.copytree(REPO_ROOT, tree, ignore=shutil.ignore_patterns("__pycache__", "*.pyc", ".git", "*.iso"))
    shutil.copyfile(os.path.join(REPO_ROOT, "archivetool", "vendor", "bagit.py"),
                    os.path.join(tools_dir, "bagit.py"))
    if extra_tools:
        shutil.copytree(extra_tools, os.path.join(tools_dir, "extra"))


README_TEMPLATE = """\
{title}
{underline}

Disc id:  {id}{part}
Set:      {set}
Burned:   {date}
Contents: {files} files, {bytes} bytes (in data/)
Made by:  {software}

This disc is a BagIt bag (RFC 8493) with dvdisaster RS03 error correction
data stored after the filesystem.

BROWSE
  Open index.html in any web browser. It lists every file on this disc
  and{other_discs}. search.html searches file names on this disc{search_scope}.

VERIFY (detect damage)
  From the root of the mounted disc, either of:
    sha256sum -c manifest-sha256.txt
    python3 tools/bagit.py --validate .

REPAIR (fix damage)
  Use dvdisaster (https://github.com/speed47/dvdisaster; a copy may be in
  tools/extra/, but keep one off-disc too):
    dvdisaster -d /dev/sr0 -r -i disc.iso   # read the disc, even if damaged
    dvdisaster -i disc.iso -f                # repair using the embedded RS03 data
    dvdisaster -i disc.iso -t                # check
  Then burn or mount disc.iso and verify as above.

CATALOGUE
  catalog.rec             this disc's record (GNU recutils format, plain text)
{catalog_lines}
TOOLS
  tools/{repo}/           the program that made this disc
{bundle_line}  tools/bagit.py          BagIt validator (public domain)
"""


def write_readme(path, disc, snapshot_scope, history=False):
    if snapshot_scope == "disc":
        other = ""
        cat_lines = "  catalog/listings/       file list of this disc with sizes and dates\n"
    else:
        other = " the discs made before it (%s catalogue)" % snapshot_scope
        cat_lines = ("  catalog/archive.rec     all discs in the archive as of the burn date\n"
                     "  catalog/manifests/      sha256 file lists of those discs\n"
                     "  catalog/listings/       file lists with sizes and dates\n")
    title = disc.get("Title")
    text = README_TEMPLATE.format(
        title=title, underline="=" * len(title), id=disc.get("Id"), set=disc.get("Set"),
        part=("  (part %s)" % disc.get("Part")) if disc.get("Part") else "",
        date=disc.get("Date"), files=disc.get("Files"), bytes=disc.get("Bytes"),
        software=disc.get("Software"), other_discs=(" lists" + other) if other else " its notes",
        catalog_lines=cat_lines, repo=REPO_NAME,
        bundle_line=("  tools/%s.bundle     the same with full history: git clone <bundle>\n" % REPO_NAME)
        if history else "",
        search_scope="" if snapshot_scope == "disc" else " and on every disc in the catalogue",
    )
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


# ---------------------------------------------------------------- commands

def cmd_make(args):
    src = os.path.abspath(args.source)
    if not os.path.isdir(src):
        raise SystemExit("Error: %s is not a directory" % src)
    image.require("genisoimage", *(["dvdisaster"] if not args.no_ecc else []))
    if args.output and args.output_dir:
        raise SystemExit("Error: use either --output or --output-dir")
    interactive = sys.stdin.isatty() and not args.yes
    home = catalog.Home(args.home)
    cat = home.load()

    log("Scanning and hashing %s ..." % src)
    entries = bag.scan_payload(src)

    draft = {}
    if args.draft:
        draft = describe.load_draft(args.draft)
    elif args.llm:
        draft = describe.make_draft(args, src, entries, interactive)
    if draft:
        # the draft fills the fields the command line leaves empty; no prompts for those
        args.title = args.title or draft["title"]
        args.description = args.description or draft["description"]
        args.subject = args.subject or draft["subjects"] or None
        args.note = (args.note or []) + draft["notes"] or None

    default_title, default_set = folder_defaults(src)
    try:
        coverage = discid.to_edtf(args.coverage) if args.coverage else catalog.coverage_years(entries)
    except discid.IdError as err:
        raise SystemExit("Error: --coverage: %s (examples: 2019, 2015/2024, 2019-07/2019-08, 199X, 1995~)" % err)
    vocab = sets.load(home)
    default_set = sets.guess(vocab, default_set) or default_set
    set_code = make.sanitize_set(args.set or ask("Set code (see 'archive sets')", default_set, interactive))
    set_info = sets.lookup(vocab, set_code)
    if set_info:
        log("Set: %s  (%s)" % (set_info.label, sets.path_name(vocab, set_info)))
    else:
        hint = sets.near(vocab, set_code)
        log("Warning: set %s is not in %s%s. The disc is made anyway, without a class; add the set "
            "there to classify it." % (set_code, sets.vocab_path(home),
                                       " (did you mean %s?)" % ", ".join(hint) if hint else ""))
    meta = {
        "set": set_code,
        "set_class": set_info.cls if set_info else None,
        "coverage": coverage,
        "title": args.title or ask("Title", default_title, interactive),
        "description": args.description or ask("Description (optional)", None, interactive),
        "creator": args.creator or ask("Creator", os.environ.get("USER"), interactive),
        "location": args.location or ask("Physical location (optional)", None, interactive),
        "subjects": args.subject or [s.strip() for s in (ask("Subjects, comma separated (optional)", None, interactive) or "").split(",") if s.strip()],
        "notes": args.note or [n for n in [ask("Note (optional)", None, interactive)] if n],
        "folder_tags": draft.get("folder_tags") or {},
        "folder_captions": draft.get("folder_captions") or {},
        "draft_agent": draft.get("agent"),
    }
    version, is_git = software_version()
    maker = make.Maker(args, meta, entries, src, home, cat, version, is_git, stage_tools, write_readme)
    return maker.run()


def cmd_find(args):
    home = catalog.Home(args.home)
    cat = home.load()
    if index.is_fresh(home):
        disc_hits, file_hits = index.find(home, cat, args.pattern)
    else:
        if os.path.exists(home.sqlite_path):
            log("Note: archive.sqlite is out of date; scanning manifests (run 'archive index' to refresh)")
        disc_hits, file_hits = catalog.find(home, cat, args.pattern)
    for d in disc_hits:
        print("DISC  %s  %s  [%s]" % (d.get("Id"), d.get("Title"), d.get("Location", "location unknown")))
    tag_hits = catalog.find_tags(home, cat, args.pattern)
    for d, folder, tags in tag_hits:
        print("TAG   %s  [%s]  data/%s/  (%s)" % (d.get("Id"), d.get("Location", "?"), folder, ", ".join(tags)))
    for d, path in file_hits[: args.limit] if args.limit else file_hits:
        print("%s  [%s]  %s" % (d.get("Id"), d.get("Location", "?"), path))
    if args.limit and len(file_hits) > args.limit:
        print("... %d more file matches (use --limit 0 for all)" % (len(file_hits) - args.limit))
    return 0 if disc_hits or file_hits or tag_hits else 1


def cmd_list(args):
    cat = catalog.Home(args.home).load()
    for d in cat.discs:
        if args.covers:
            try:
                if not discid.covers(d.get("Coverage"), args.covers):
                    continue
            except discid.IdError as err:
                raise SystemExit("Error: --covers: %s" % err)
        print("%s\t%s\t%s\t%s files\t%s" % (d.get("Id"), d.get("Date"), d.get("Title"),
                                           d.get("Files"), d.get("Location", "")))
    return 0


def cmd_sets(args):
    """The set vocabulary as a tree, with how many discs each set has."""
    home = catalog.Home(args.home)
    vocab = sets.load(home)
    counts = {}
    for d in home.load().discs:
        counts[d.get("Set")] = counts.get(d.get("Set"), 0) + 1
    for s in vocab:
        indent = "" if s.cls % 100 == 0 else ("  " if s.cls % 10 == 0 else "    ")
        n = counts.pop(s.code, 0) if s.code else 0
        print("%03d  %s%-8s %-28s %s" % (s.cls, indent, s.code or "", s.name, ("%d disc%s" % (n, "" if n == 1 else "s")) if n else ""))
    for code, n in sorted(counts.items()):
        print("---  %-10s (not in %s) %d disc%s" % (code, sets.vocab_path(home), n, "" if n == 1 else "s"))
    return 0


def cmd_id(args):
    """Explain a disc id: its parts, whether the check character is right, and what it matches."""
    cat = catalog.Home(args.home).load()
    parsed = discid.parse(args.disc_id)
    if not parsed:
        print("%s: not a disc id of a known scheme" % args.disc_id)
        return 1
    print("scheme:    %s" % parsed["scheme"])
    print("set:       %s" % parsed["set"])
    print("sequence:  %d" % parsed["sequence"])
    print("coverage:  %s" % parsed["coverage"])
    if parsed["check"]:
        print("check:     %s (%s)" % (parsed["check"], "correct" if parsed["valid"] else "WRONG: probably a typo"))
    disc = cat.disc(args.disc_id.strip().upper()) or cat.disc(args.disc_id.strip())
    if disc:
        print("disc:      %s [%s]" % (disc.get("Title"), disc.get("Location", "location not recorded")))
        again = discid.regenerate(disc)
        if again is not None:
            print("fields:    %s" % ("regenerate this id" if again == disc.get("Id") else
                                     "regenerate %s (the record and the id disagree)" % again))
    else:
        close = discid.suggest(args.disc_id, [d.get("Id") for d in cat.discs])
        print("disc:      not in the catalogue" + (" - did you mean %s?" % ", ".join(close) if close else ""))
    return 0 if parsed["valid"] else 1


def _edit_disc(args, fn):
    home = catalog.Home(args.home)
    cat = home.load()
    disc = cat.disc(args.disc_id)
    if not disc:
        raise SystemExit("Error: no disc %s in %s" % (args.disc_id, home.rec_path))
    fn(disc)
    home.save(cat)
    return 0


def cmd_note(args):
    return _edit_disc(args, lambda d: d.add("Note", args.text))


def cmd_locate(args):
    return _edit_disc(args, lambda d: d.set("Location", args.location))


def _resolve_disc(cat, disc_id, source):
    if not disc_id:
        disc_id = image.read_volume_id(source)
        if not disc_id:
            raise SystemExit("Error: no ISO9660 volume id on %s; pass the disc id explicitly" % source)
    disc = cat.disc(disc_id)
    if not disc:
        raise SystemExit("Error: disc %s is not in the catalogue" % disc_id)
    return disc_id, disc


def cmd_check(args):
    """Fixity check of a burned disc (drive) or an image, logged as a PREMIS 'fixity check' event."""
    image.require("dvdisaster")
    home = catalog.Home(args.home)
    cat = home.load()
    source = args.device or args.image
    disc_id, _ = _resolve_disc(cat, args.disc_id, source)
    log("Checking %s (%s) ..." % (disc_id, source))
    if args.device:
        ok, output = image.scan_device(args.device)
        what = "disc scan with dvdisaster -s on " + args.device
    else:
        ok, output = image.verify_ecc(args.image)
        what = "image test with dvdisaster -t"
    note = what + "\n" + image.summary(output)
    if args.note:
        note += "\n" + args.note
    cat.events.append(catalog.new_event(disc_id, "fixity check", "success" if ok else "failure", "dvdisaster", note))
    home.save(cat)
    print(output if not ok or args.verbose else image.summary(output))
    print("%s: %s" % (disc_id, "OK" if ok else "FAILED - see output above"))
    return 0 if ok else 1


def cmd_burned(args):
    """Record that copies of a disc image were burned."""
    home = catalog.Home(args.home)
    cat = home.load()
    disc = cat.disc(args.disc_id)
    if not disc:
        raise SystemExit("Error: no disc %s in %s" % (args.disc_id, home.rec_path))
    disc.set("Copies", str(int(disc.get("Copies", "0")) + args.copies))
    if args.media_id:
        disc.add("MediaId", args.media_id)
    note = "burned %d cop%s" % (args.copies, "y" if args.copies == 1 else "ies")
    if args.note:
        note += "; " + args.note
    cat.events.append(catalog.new_event(args.disc_id, "replication", "success", "manual", note))
    home.save(cat)
    print("%s: %s copies recorded" % (args.disc_id, disc.get("Copies")))
    return 0


def cmd_rebuild(args):
    """Merge the catalogue carried by a disc (mounted or extracted) into the home catalogue."""
    home = catalog.Home(args.home)
    cat = home.load()
    root = args.disc_root
    snap_dir = os.path.join(root, "catalog")
    sources = [os.path.join(snap_dir, "archive.rec"), os.path.join(root, "catalog.rec")]
    found = [p for p in sources if os.path.exists(p)]
    if not found:
        raise SystemExit("Error: %s has neither catalog/archive.rec nor catalog.rec" % root)
    added, updated, events = [], [], 0
    for path in found:
        a, u, e = catalog.merge(cat, catalog.Catalog(recfile.read(path)), prefer_other=args.prefer_disc)
        added += a
        updated += u
        events += e
    copied = 0
    for d in cat.discs:
        disc_id = d.get("Id")
        for kind, ext in catalog.DISC_FILE_KINDS.items():
            src = os.path.join(snap_dir, kind, disc_id + ext)
            if kind == "manifests" and not os.path.exists(src) and disc_id == disc_root_id(root):
                src = os.path.join(root, "manifest-sha256.txt")  # disc without catalog/manifests
            dest = home.disc_file(kind, disc_id)
            if os.path.exists(src) and not os.path.exists(dest):
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                shutil.copyfile(src, dest)
                copied += 1
    home.save(cat)
    if os.path.exists(home.sqlite_path):
        index.build(home, cat)
    print("Added %d disc(s)%s, updated %d, %d new event(s), %d file list(s) copied into %s"
          % (len(added), (" (" + ", ".join(added) + ")") if added else "", len(updated), events, copied, home.path))
    return 0


def disc_root_id(root):
    """Disc id of a mounted or extracted disc, from its bag-info.txt."""
    try:
        with open(os.path.join(root, "bag-info.txt"), encoding="utf-8") as f:
            for line in f:
                if line.startswith("External-Identifier:"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return None


def cmd_index(args):
    home = catalog.Home(args.home)
    cat = home.load()
    index.build(home, cat)
    print("Built %s" % home.sqlite_path)
    return 0


def add_llm_options(parser):
    parser.add_argument("--llm-url", help="OpenAI-compatible server (default: $ARCHIVE_LLM_URL or %s, Ollama)"
                        % llm.DEFAULT_URL)
    parser.add_argument("--llm-model", help="model name (default: $ARCHIVE_LLM_MODEL or the server's first model)")
    parser.add_argument("--llm-allow-remote", action="store_true",
                        help="allow a non-local LLM server (the inventory describes your private files)")
    parser.add_argument("--vision", action="store_true",
                        help="also show sample images (and video frames, with ffmpeg) to a local vision model; "
                             "local servers only")
    parser.add_argument("--vision-model", help="vision-capable model (default: the --llm-model)")
    parser.add_argument("--vision-url", help="server for the vision model (default: the --llm-url; must be local)")
    parser.add_argument("--vision-per-folder", type=int, default=3, help="images sampled per folder (default: 3)")
    parser.add_argument("--vision-max", type=int, default=40, help="images sampled in total (default: 40)")


def cmd_tag(args):
    return tagger.run(args)


def cmd_models(args):
    return tagger.models_command(args)


def cmd_describe(args):
    return describe.run(args)


def cmd_gui(args):
    from . import gui
    return gui.main(args)


def build_parser():
    p = argparse.ArgumentParser(prog="archive", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--home", help="catalogue directory (default: $BLURAY_ARCHIVE_HOME or ~/.local/share/bluray-archive)")
    sub = p.add_subparsers(dest="command", required=True)

    m = sub.add_parser("make", help="build a disc image from a folder")
    m.add_argument("source", help="folder to archive (left unmodified)")
    m.add_argument("-o", "--output", help="image path for a single disc (default: <disc-id>.iso)")
    m.add_argument("--output-dir", help="directory for the images (default: current directory)")
    m.add_argument("--id", help="disc id for a single disc (default: <coverage>_<SET>_<nn>)")
    m.add_argument("--set", help="set code, 2-8 letters or digits, e.g. PHOTOS")
    m.add_argument("--coverage",
                   help="dates the contents span, in EDTF: 2019, 2015/2024, 2019-07/2019-08, 199X, 1995~ "
                        "(default: from file modification times)")
    m.add_argument("--title")
    m.add_argument("--description")
    m.add_argument("--creator")
    m.add_argument("--subject", action="append", help="repeatable")
    m.add_argument("--note", action="append", help="repeatable")
    m.add_argument("--location", help="where the disc will be stored")
    m.add_argument("--rights")
    m.add_argument("--medium", choices=["auto"] + list(media.MEDIA), default="bd25",
                   help="target disc: RS03 fills it with error correction (default: bd25). "
                        "auto lets dvdisaster pick the smallest standard size")
    m.add_argument("--medium-sectors", type=int,
                   help="custom medium capacity in 2048-byte sectors (overrides --medium; mainly for testing)")
    m.add_argument("--no-defect-management", action="store_true",
                   help="discs are burned without BD-R defect management (slightly more space)")
    m.add_argument("--min-redundancy", type=float, default=20,
                   help="minimum RS03 redundancy in %% that each disc must keep (default: 20)")
    m.add_argument("--split", action="store_true",
                   help="spread the folder over as many discs as needed")
    m.add_argument("--media", help="media description (default: M-DISC <medium>)")
    m.add_argument("--formats", choices=["auto", "yes", "no"], default="auto",
                   help="identify file formats (PRONOM) with Siegfried: auto = when sf is installed")
    m.add_argument("--sf-home", help="Siegfried signature directory (sf -home)")
    m.add_argument("--llm", action="store_true",
                   help="ask a local LLM to suggest title, description, subjects and folder tags, "
                        "and to ask you questions about the folder (optional)")
    m.add_argument("--llm-rounds", type=int, default=2, help="question rounds with --llm (default: 2)")
    m.add_argument("--draft", help="metadata draft JSON from 'archive describe --save'")
    add_llm_options(m)
    m.add_argument("--ro-crate", action="store_true",
                   help="add RO-Crate 1.2 metadata (data/ro-crate-metadata.json + preview) for research-data tools")
    m.add_argument("--snapshot", choices=["full", "set", "disc"], default="full",
                   help="catalogue to include: full (every disc, default), set (this set only, for "
                        "discs given to other people), disc (this disc only)")
    m.add_argument("--extra-tools", help="folder copied to tools/extra/ (e.g. dvdisaster binaries)")
    m.add_argument("--tools-history", action="store_true",
                   help="also put this tool's full git history on the disc (git bundle); default: snapshot only")
    m.add_argument("--no-ecc", action="store_true", help="skip dvdisaster (testing only)")
    m.add_argument("--no-verify", action="store_true", help="skip dvdisaster -t after adding ECC")
    m.add_argument("--keep-stage", action="store_true")
    m.add_argument("-y", "--yes", action="store_true", help="no prompts; use defaults")
    m.set_defaults(func=cmd_make)

    f = sub.add_parser("find", help="search disc descriptions and file lists")
    f.add_argument("pattern", help="substring, or glob if it contains * ? [")
    f.add_argument("--limit", type=int, default=200)
    f.set_defaults(func=cmd_find)

    ls = sub.add_parser("list", help="list discs")
    ls.add_argument("--covers", metavar="DATE",
                    help="only discs whose coverage overlaps this date or range: 2019, 2019-07, 2019-07-15, 2018/2019")
    ls.set_defaults(func=cmd_list)

    st = sub.add_parser("sets", help="show the set vocabulary (Dewey-like classes) and discs per set")
    st.set_defaults(func=cmd_sets)

    di = sub.add_parser("id", help="explain and check a disc id (catches typos)")
    di.add_argument("disc_id")
    di.set_defaults(func=cmd_id)

    n = sub.add_parser("note", help="add a note to a disc")
    n.add_argument("disc_id")
    n.add_argument("text")
    n.set_defaults(func=cmd_note)

    loc = sub.add_parser("locate", help="set a disc's physical location")
    loc.add_argument("disc_id")
    loc.add_argument("location")
    loc.set_defaults(func=cmd_locate)
    c = sub.add_parser("check", help="verify a burned disc or image with dvdisaster and log the result")
    src = c.add_mutually_exclusive_group(required=True)
    src.add_argument("--device", help="optical drive, e.g. /dev/sr0 (scans the whole disc)")
    src.add_argument("--image", help="image file")
    c.add_argument("disc_id", nargs="?", help="default: read from the volume label")
    c.add_argument("--note")
    c.add_argument("-v", "--verbose", action="store_true")
    c.set_defaults(func=cmd_check)

    b = sub.add_parser("burned", help="record that copies of a disc were burned")
    b.add_argument("disc_id")
    b.add_argument("--copies", type=int, default=1)
    b.add_argument("--media-id", help="media id reported by the drive, e.g. from dvd+rw-mediainfo")
    b.add_argument("--note")
    b.set_defaults(func=cmd_burned)

    r = sub.add_parser("rebuild", help="merge the catalogue on a mounted disc into the home catalogue")
    r.add_argument("disc_root", help="mount point (or extracted copy) of a disc")
    r.add_argument("--prefer-disc", action="store_true",
                   help="overwrite existing home Disc records with the disc's versions")
    r.set_defaults(func=cmd_rebuild)

    i = sub.add_parser("index", help="(re)build the SQLite search index used by find")
    i.set_defaults(func=cmd_index)

    ds = sub.add_parser("describe", help="improve metadata with a local LLM (a folder, or a disc in the catalogue)")
    ds.add_argument("target", help="folder to be archived, or a disc id")
    ds.add_argument("--rounds", type=int, default=2, help="question rounds (default: 2)")
    ds.add_argument("--questions", type=int, default=5, help="questions per round (default: 5)")
    ds.add_argument("--save", help="write the reviewed result as a draft JSON for 'archive make --draft'")
    ds.add_argument("--disc-root", help="mounted disc, so README-style files on it can be read")
    ds.add_argument("--show-inventory", action="store_true", help="print exactly what would be sent, and stop")
    ds.add_argument("--apply", metavar="DRAFT", help="apply a saved draft to the disc (no LLM needed)")
    add_llm_options(ds)
    ds.set_defaults(func=cmd_describe)

    tg = sub.add_parser("tag", help="suggest folder tags from your tag vocabulary (small built-in model)")
    tg.add_argument("target", help="folder to be archived, or a disc id")
    tg.add_argument("--top", type=int, default=3, help="at most this many tags per folder (default: 3)")
    tg.add_argument("--vocab", help="tag vocabulary recfile (default: <home>/tags.rec)")
    tg.add_argument("--save", help="write (or merge into) a draft JSON for 'archive make --draft'")
    tg.add_argument("--apply", action="store_true", help="for a disc: write the tags without prompting")
    tg.add_argument("--disc-root", help="mounted disc, so README files on it can be read")
    tg.add_argument("--show-summaries", action="store_true", help="print what the model compares, and stop")
    tg.add_argument("--llama-embedding", help="path to llama.cpp's llama-embedding")
    tg.add_argument("--model", default=models.DEFAULT_EMBEDDING, help="built-in model (default: %(default)s)")
    tg.add_argument("--embed-url", help="use an OpenAI-compatible /v1/embeddings server instead of the built-in model")
    tg.add_argument("--embed-model", help="embedding model name on that server (default: its first model)")
    tg.add_argument("--llm-allow-remote", action="store_true", help="allow a non-local embeddings server")
    tg.set_defaults(func=cmd_tag)

    mo = sub.add_parser("models", help="built-in model for 'archive tag': fetch, status, build-runtime")
    mo.add_argument("action", choices=["fetch", "status", "build-runtime"])
    mo.add_argument("--model", default=models.DEFAULT_EMBEDDING, choices=list(models.MODELS))
    mo.add_argument("--from", dest="from_file", help="install from a local file (checksum-verified) instead of downloading")
    mo.add_argument("--llama-embedding", help="path to llama.cpp's llama-embedding")
    mo.set_defaults(func=cmd_models)

    g = sub.add_parser("gui", help="open the graphical interface in your web browser")
    g.add_argument("--port", type=int, default=0, help="port on 127.0.0.1 (default: any free port)")
    g.add_argument("--no-browser", action="store_true", help="print the URL instead of opening a browser")
    add_llm_options(g)
    g.set_defaults(func=cmd_gui)
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except BrokenPipeError:  # output piped into e.g. `head`, which stopped reading
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        return 0
