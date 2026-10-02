"""arv (Archive, Record, Verify): build self-describing, error-corrected archival disc images.

Commands:
  init   start an archive: a .arv folder here (or a pointer to one elsewhere)
  where  which home catalogue is used here, and why
  make   bag a folder, write the catalogue + viewer, build the image, add RS03 ECC
  names  check a folder's file names against the disc filesystems' limits
  find   search every disc's catalogue and file list (no discs needed)
  list   list discs in the home catalogue
  note   add a note to a disc
  locate set where a disc's copies are kept
  location  places (site, room, shelf, box) as a tree
  collection  virtual folders of discs, folders and files across discs
  access set what other discs' catalogues may show of a disc
  burned record that copies were burned
  check  verify a disc or image with dvdisaster and log a fixity-check event
  rebuild merge the catalogue carried on a disc into the home catalogue
  index  build the SQLite search index
  describe  improve titles, descriptions and tags with a local LLM (optional)
  tag    suggest folder tags from your tag vocabulary (match rules, small built-in model)
  tags   every folder tag in use, grouped by namespace
  keywords  a disc's set paths and tags as hierarchical (XMP) keywords
  sets   the set vocabulary tree
  id     explain and check a disc id
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
import textwrap

from . import NAME, bag, catalog, homes, describe, discid, image, index, llm, make, media, models, names, recfile, sets, tagger

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # src/arv/ -> repository
REPO_NAME = NAME


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
            # untracked files never reach the disc (it carries `git archive HEAD`), so ignore them
            dirty = subprocess.run(["git", "-C", REPO_ROOT, "status", "--porcelain", "--untracked-files=no"],
                                   capture_output=True, text=True, check=True).stdout.strip()
            return "%s@%s%s" % (REPO_NAME, commit, "+uncommitted" if dirty else ""), True
        except subprocess.CalledProcessError:
            pass
    stamp = os.path.join(REPO_ROOT, "VERSION")  # written by `make install`
    if os.path.exists(stamp):
        with open(stamp, encoding="utf-8") as f:
            return f.read().strip(), False
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
    shutil.copyfile(os.path.join(os.path.dirname(os.path.abspath(__file__)), "vendor", "bagit.py"),
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

{plain}

The rest of this page is for checking the disc and, if it is ever damaged,
repairing it. Anyone comfortable with a command line can follow it.

This disc is a BagIt bag (RFC 8493) with dvdisaster RS03 error correction
data stored after the filesystem.

BROWSE
  Open index.html in any web browser. It lists every file on this disc
  and{other_discs}.

SEARCH
  Catalogue software that reads this format can search every disc (the
  spec: tools/{repo}/docs/smart-archive-format.md).
  With only Python 3, from the root of the mounted disc:
    python3 tools/{repo}/arv --home catalog find PATTERN
    python3 tools/{repo}/arv --home catalog list
  Or plain text tools: grep -ri PATTERN catalog/volumes/*/listing.tsv

VERIFY (detect damage)
  From the root of the mounted disc, either of:
    sha256sum -c manifest-sha256.txt
    python3 tools/bagit.py --validate .

REPAIR (fix damage)
  Use dvdisaster: https://github.com/teaching-droid/dvdisaster-light or
  https://github.com/speed47/dvdisaster (a copy may be in tools/extra/, but
  keep one off-disc too).
  1. Read the disc into an image, even if parts are unreadable:
       dvdisaster -d /dev/sr0 -r -i disc.iso
     (dvdisaster Light can read, repair and re-read in one go: add --rescue.)
{size_check}  2. Repair, then check:
       dvdisaster -i disc.iso -f
       dvdisaster -i disc.iso -t
  3. Still damaged? Every copy of this disc is identical. Put in another copy
     and read it into the same image; only the missing sectors are read:
       dvdisaster -d /dev/sr0 -r -j 1 -i disc.iso
     then repair as in step 2.
  Then burn or mount disc.iso and verify as above.

CATALOGUE
  catalog.rec             this disc's record (GNU recutils format, plain text)
{catalog_lines}
TOOLS
  tools/{repo}/           the program that made this disc
{bundle_line}  tools/bagit.py          BagIt validator (public domain)
"""


def write_readme(path, disc, snapshot_scope, history=False, image_sectors=None):
    if snapshot_scope == "disc":
        other = ""
        cat_lines = "  catalog/volumes/<id>/   this disc's file list (listing.tsv) and checksums\n"
    else:
        other = " the discs made before it (%s catalogue)" % snapshot_scope
        cat_lines = ("  catalog/archive.rec     all discs in the archive as of the burn date\n"
                     "  catalog/volumes/<id>/   per disc: manifest.sha256, listing.tsv, formats.csv\n")
    title = disc.get("Title")
    who = disc.get("Creator")
    plain = ("This is an archive disc%s, made on %s: %s. Its files are ordinary files in the\n"
             "data/ folder, and any computer can open them."
             % (" by " + who if who else "", disc.get("Date"), title))
    if image_sectors:
        size_check = ("The image must be %d sectors (%d bytes). If it comes out smaller, the error "
                      "correction was not found; read again with --ignore-iso-size."
                      % (image_sectors, image_sectors * 2048))
    else:
        size_check = ("The image is larger than the filesystem. If dvdisaster does not mention RS03 "
                      "error correction while reading, read again with --ignore-iso-size.")
    size_check = textwrap.fill(size_check, 76, initial_indent=" " * 5, subsequent_indent=" " * 5,
                               break_on_hyphens=False) + "\n"
    text = README_TEMPLATE.format(
        plain=textwrap.fill(plain.replace("\n", " "), 76), size_check=size_check,
        title=title, underline="=" * len(title), id=disc.get("Id"), set=disc.get("Set"),
        part=("  (part %s)" % disc.get("Part")) if disc.get("Part") else "",
        date=disc.get("Date"), files=disc.get("Files"), bytes=disc.get("Bytes"),
        software=disc.get("Software"), other_discs=(" lists" + other) if other else " its notes",
        catalog_lines=cat_lines, repo=REPO_NAME,
        bundle_line=("  tools/%s.bundle     the same with full history: git clone <bundle>\n" % REPO_NAME)
        if history else "",
    )
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


# ---------------------------------------------------------------- commands

def place(cat, text):
    """A Location code when ``text`` names one (any case), else the text as given."""
    if not text:
        return text
    loc = cat.location(text)
    return loc.get("Code") if loc else text.strip()


def pick_code(vocab, text):
    """A vocabulary code for a typed code or alias ("holidays" -> TRIP); otherwise the text as a code."""
    code = vocab.resolve(text)
    if code and sets.word(text) != code:
        log("%s -> %s" % (text.strip(), code))
    return code or make.sanitize_set(text)


def rule_suggestions(vocab, entries, share=0.1):
    """Codes whose Match rules claim at least ``share`` of the files, most files first."""
    counts = vocab.match([e.path for e in entries])
    least = max(1, share * len(entries))
    ranked = sorted((c for c, n in counts.items() if n >= least), key=lambda c: (-counts[c], c))
    # keep the most specific: drop a code when one of its descendants is also suggested
    return [c for c in ranked if not any(c in vocab.ancestors(o) for o in ranked)]


def cmd_make(args):
    src = os.path.abspath(args.source)
    if not os.path.isdir(src):
        raise SystemExit("Error: %s is not a directory" % src)
    image.require(*(["genisoimage"] if args.filesystem == "hybrid" else []),
                  *(["dvdisaster"] if not args.no_ecc else []))
    if args.filesystem == "udf250" and not image.find_udfmake(args.udfmake):
        raise SystemExit("Error: --filesystem udf250 needs udfmake: build it with 'make -C %s', "
                         "put it on PATH, or pass --udfmake PATH"
                         % os.path.join(REPO_ROOT, "src", "udfmake"))
    if args.output and args.output_dir:
        raise SystemExit("Error: use either --output or --output-dir")
    interactive = sys.stdin.isatty() and not args.yes
    home = catalog.Home(args.home)
    cat = home.load()

    log("Scanning and hashing %s ..." % src)
    entries = bag.scan_payload(src)
    check_names([e.path for e in entries], args.filesystem, args.ignore_names)

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

    if draft.get("folder_tags"):  # aliases in LLM or saved drafts become the vocabulary's tag names
        try:
            tag_vocab = tagger.load_tag_vocab(home)
            draft["folder_tags"] = {f: tag_vocab.canonical_list(t) for f, t in draft["folder_tags"].items()}
        except tagger.TagError:
            pass
    default_title, default_set = folder_defaults(src)
    try:
        coverage = discid.to_edtf(args.coverage) if args.coverage else catalog.coverage_years(entries)
    except discid.IdError as err:
        raise SystemExit("Error: --coverage: %s (examples: 2019, 2015/2024, 2019-07/2019-08, 199X, 1995~)" % err)
    try:
        vocab = sets.load(home)
    except sets.VocabError as err:
        raise SystemExit("Error: %s" % err)
    rule_codes = [] if args.no_rules else rule_suggestions(vocab, entries)
    default_set = vocab.guess(default_set) or (rule_codes[0] if rule_codes else default_set)
    set_code = pick_code(vocab, args.set or ask("Set code (see 'arv sets')", default_set, interactive))
    rule_categories = [c for c in rule_codes if c != set_code and c not in vocab.ancestors(set_code)][:3]
    if args.category:
        categories = args.category
    else:
        answer = ask("Extra categories, comma separated (optional)", ", ".join(rule_categories) or None, interactive)
        categories = [c for c in (answer or "").split(",") if c.strip()]
        if categories and not interactive:
            log("Categories from Match rules in %s: %s (--category to choose, --no-rules to skip)"
                % (vocab.path, ", ".join(categories)))
    categories = [pick_code(vocab, c) for c in categories]
    categories = [c for i, c in enumerate(categories) if c != set_code and c not in categories[:i]]
    paths = []
    for code in [set_code] + categories:
        found = vocab.paths(code)
        if found:
            paths += found
        else:
            hint = vocab.near(code)
            log("Warning: %s is not in %s%s. It is used anyway, without a place in the vocabulary."
                % (code, vocab.path, " (did you mean %s?)" % ", ".join(hint) if hint else ""))
    if paths:
        log("Classified as: %s" % ", ".join(paths))
    meta = {
        "set": set_code,
        "categories": categories,
        "paths": paths,
        "coverage": coverage,
        "title": args.title or ask("Title", default_title, interactive),
        "description": args.description or ask("Description (optional)", None, interactive),
        "creator": args.creator or ask("Creator", os.environ.get("USER"), interactive),
        "location": place(cat, args.location or ask("Physical location (optional; see 'arv location list')",
                                                    None, interactive)),
        "access": args.access,
        "subjects": args.subject or [s.strip() for s in (ask("Subjects, comma separated (optional)", None, interactive) or "").split(",") if s.strip()],
        "notes": args.note or [n for n in [ask("Note (optional)", None, interactive)] if n],
        "folder_tags": draft.get("folder_tags") or {},
        "folder_captions": draft.get("folder_captions") or {},
        "draft_agent": draft.get("agent"),
    }
    version, is_git = software_version()
    maker = make.Maker(args, meta, entries, src, home, cat, version, is_git, stage_tools, write_readme)
    return maker.run()


def check_names(paths, filesystem, ignore_warnings=False):
    """Stop on names the image cannot hold; show names some systems will see differently."""
    issues = names.check(paths, filesystem)
    errors = [i for i in issues if i[1] == "error"]
    if errors:
        raise SystemExit("Error: %d file name(s) cannot be stored in a %s image (rename them, or use the "
                         "default hybrid image, which keeps them exactly for Linux):\n%s"
                         % (len(errors), filesystem, "\n".join(names.report(errors))))
    if issues and not ignore_warnings:
        log("Note: %d file name(s) will look different on Windows/macOS (the manifests and Linux keep "
            "them exactly; --ignore-names hides this):\n%s" % (len(issues), "\n".join(names.report(issues))))


def cmd_names(args):
    """`arv names FOLDER`: which names a disc image would change, without making one."""
    src = os.path.abspath(args.source)
    if not os.path.isdir(src):
        raise SystemExit("Error: %s is not a directory" % src)
    paths = []
    for root, dirs, files in os.walk(src):
        dirs.sort()
        rel = os.path.relpath(root, src)
        for n in sorted(files):
            paths.append(n if rel == "." else "%s/%s" % (rel.replace(os.sep, "/"), n))
    code = 0
    for fs in ([args.filesystem] if args.filesystem else list(image.FILESYSTEMS)):
        issues = names.check(paths, fs)
        print("%s: %s" % (fs, "all %d names kept exactly" % len(paths) if not issues else
                          "%d issue(s)" % len(issues)))
        for line in names.report(issues, limit=args.limit or len(issues)):
            print(line)
        code = code or any(i[1] == "error" for i in issues)
    return 1 if code else 0


def cmd_find(args):
    home = catalog.Home(args.home)
    cat = home.load()
    if index.is_fresh(home):
        disc_hits, file_hits = index.find(home, cat, args.pattern)
    else:
        if os.path.exists(home.sqlite_path):
            log("Note: archive.sqlite is out of date; scanning manifests (run 'arv index' to refresh)")
        disc_hits, file_hits = catalog.find(home, cat, args.pattern)
    for d in disc_hits:
        print("DISC  %s  %s  [%s]" % (d.get("Id"), d.get("Title"), cat.where(d) or "location unknown"))
    tag_hits = catalog.find_tags(home, cat, args.pattern)
    for d, folder, tags in tag_hits:
        print("TAG   %s  [%s]  %s  (%s)" % (d.get("Id"), cat.where(d) or "?",
                                          "data/" if folder == "." else "data/%s/" % folder, ", ".join(tags)))
    for d, path in file_hits[: args.limit] if args.limit else file_hits:
        print("%s  [%s]  %s" % (d.get("Id"), cat.where(d) or "?", path))
    if args.limit and len(file_hits) > args.limit:
        print("... %d more file matches (use --limit 0 for all)" % (len(file_hits) - args.limit))
    return 0 if disc_hits or file_hits or tag_hits else 1


def cmd_list(args):
    cat = catalog.Home(args.home).load()
    places = cat.locations_under(args.at) if args.at else None
    for d in cat.discs:
        if args.within and args.within.upper() not in sets.disc_codes(d):
            continue
        if places is not None and not any(l.strip().upper() in places for l in d.get_all("Location")):
            continue
        if args.access and catalog.access(d) != args.access:
            continue
        if args.made and not (d.get("Date") or "").startswith(args.made):
            continue
        if args.covers:
            try:
                if not discid.covers(d.get("Coverage"), args.covers):
                    continue
            except discid.IdError as err:
                raise SystemExit("Error: --covers: %s" % err)
        print("%s\t%s\t%s\t%s files\t%s\t%s" % (d.get("Id"), d.get("Date"), d.get("Title"),
                                               d.get("Files") or "?", catalog.access(d), cat.where(d)))
    return 0


def cmd_sets(args):
    """The vocabulary as a tree (entries with several parents appear under each), with disc counts."""
    home = catalog.Home(args.home)
    try:
        vocab = sets.load(home)
    except sets.VocabError as err:
        raise SystemExit("Error: %s" % err)
    discs = home.load().discs
    member = [sets.disc_codes(d) for d in discs]
    direct = {}
    for d in discs:
        for code in {d.get("Set")} | set(d.get_all("Category")):
            direct[code] = direct.get(code, 0) + 1

    def show(e, depth):
        within = sum(1 for m in member if e.code in m)
        count = ""
        if within:
            count = "%d disc%s" % (direct.get(e.code, 0), "" if direct.get(e.code, 0) == 1 else "s")
            if within != direct.get(e.code, 0):
                count += " (%d including below)" % within
        print("%s%-8s %-28s %s%s" % ("  " * depth, e.code, e.name, count,
                                      "   [also under %s]" % ", ".join(e.parents) if len(e.parents) > 1 else ""))
        if args.verbose:
            pad = "  " * depth + " " * 9
            for label, values in (("scope", [e.scope_note]), ("aliases", [", ".join(e.aliases)]),
                                  ("matches", [" ".join(e.matches)])):
                if values[0]:
                    print("%s%s: %s" % (pad, label, values[0]))
        for child in vocab.children(e.code):
            show(child, depth + 1)

    for root in vocab.roots():
        show(root, 0)
    unknown = sorted({c for c in direct if c and not vocab.get(c)})
    for code in unknown:
        print("%-8s (not in %s) %d disc%s" % (code, vocab.path, direct[code], "" if direct[code] == 1 else "s"))
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
        print("disc:      %s [%s]" % (disc.get("Title"), cat.where(disc) or "location not recorded"))
        again = discid.regenerate(disc)
        if again is not None:
            print("fields:    %s" % ("regenerate this id" if again == disc.get("Id") else
                                     "regenerate %s (the record and the id disagree)" % again))
    else:
        close = discid.suggest(args.disc_id, [d.get("Id") for d in cat.discs])
        print("disc:      not in the catalogue" + (" - did you mean %s?" % ", ".join(close) if close else ""))
    return 0 if parsed["valid"] else 1


def _edit_disc(args, fn):
    """Apply ``fn(disc)``, which returns a description of the change for its event (None: no change)."""
    home = catalog.Home(args.home)
    cat = home.load()
    disc = cat.disc(args.disc_id)
    if not disc:
        raise SystemExit("Error: no disc %s in %s" % (args.disc_id, home.rec_path))
    change = fn(disc)
    if change:
        catalog.metadata_change(cat, change, disc_id=disc.get("Id"))
    home.save(cat)
    return 0


def _clip(text, width=60):
    text = " ".join(text.split())
    return text if len(text) <= width else text[:width - 3] + "..."


def cmd_note(args):
    def add(disc):
        disc.add("Note", args.text)
        return "Note added: %s" % _clip(args.text)
    return _edit_disc(args, add)


def cmd_locate(args):
    """Set (or with --add, extend) the places where a disc's copies are kept."""
    home = catalog.Home(args.home)
    cat = home.load()
    disc = cat.disc(args.disc_id)
    if not disc:
        raise SystemExit("Error: no disc %s in %s" % (args.disc_id, home.rec_path))
    old_all = disc.get_all("Location")
    new = [place(cat, l) for l in args.location]
    for l in new:
        if not cat.location(l):
            log("Note: %s is not a location code ('arv location add' to define it); stored as text" % l)
    old = [] if not args.add else disc.get_all("Location")
    keep = [l for l in old + new if l]
    # replace in place: the new Location fields go where the first old one was (or at the end)
    first = next((i for i, (k, _) in enumerate(disc.fields) if k == "Location"), len(disc.fields))
    before = [f for f in disc.fields[:first] if f[0] != "Location"]
    after = [f for f in disc.fields[first:] if f[0] != "Location"]
    disc.fields = before + [("Location", l) for i, l in enumerate(keep) if l not in keep[:i]] + after
    now = disc.get_all("Location")
    if now != old_all:
        catalog.metadata_change(cat, "Location: %s -> %s" % (", ".join(old_all) or "(none)", ", ".join(now)),
                                disc_id=disc.get("Id"))
    home.save(cat)
    print("%s: %s" % (disc.get("Id"), cat.where(disc)))
    return 0


def _listing_paths(home, disc_id):
    path = home.disc_file("listings", disc_id)
    if not os.path.exists(path):
        return None
    from .listing import read_listing
    return [rel for _, _, rel in read_listing(path)]


def cmd_collection(args):
    """Collections: virtual folders of whole discs, folders and files across discs."""
    home = catalog.Home(args.home)
    cat = home.load()
    if args.action == "list":
        def show(col, depth):
            items = col.get_all("Item")
            print("%s%-12s %-36s %d item%s" % ("  " * depth, col.get("Code"), col.get("Name"), len(items),
                                              "" if len(items) == 1 else "s"))
            for child in [c for c in cat.collections if (c.get("Parent") or "").upper() == col.get("Code")]:
                show(child, depth + 1)
        for top in [c for c in cat.collections if not cat.collection(c.get("Parent"))]:
            show(top, 0)
        return 0
    if not args.code:
        raise SystemExit("Error: arv collection %s needs a collection code" % args.action)
    code = args.code.strip().upper()
    col = cat.collection(code)
    if args.action == "show":
        if not col:
            raise SystemExit("Error: no collection %s" % code)
        print("%s  %s" % (code, cat.collection_path(code)))
        if col.get("Description"):
            print("  " + col.get("Description"))
        for item in col.get_all("Item"):
            disc_id, path, _ = catalog.parse_item(item)
            disc = cat.disc(disc_id)
            print("  %-50s %s  [%s]" % (path or "(whole disc)", disc_id,
                                         cat.where(disc) if disc else "not in the catalogue"))
        return 0
    if args.action == "add":
        if col:
            raise SystemExit("Error: collection %s already exists" % code)
        if not catalog.COLLECTION_RE.match(code):
            raise SystemExit("Error: collection code %r: use 1-32 capital letters, digits, - or _" % args.code)
        if args.within and not cat.collection(args.within):
            raise SystemExit("Error: no collection %s (add it first)" % args.within.upper())
        col = recfile.Record("Collection", [("Code", code), ("Name", args.name or code)])
        if args.within:
            col.add("Parent", args.within.strip().upper())
        if args.description:
            col.add("Description", args.description)
        cat.collections.append(col)
        changes = ["created" + (" in %s" % args.within.strip().upper() if args.within else "")]
    elif not col:
        raise SystemExit("Error: no collection %s" % code)
    else:
        changes = []
    items_before = len(col.get_all("Item"))
    parent_before, name_before = col.get("Parent"), col.get("Name")
    if args.action in ("add", "put"):
        have = set(col.get_all("Item"))
        for item in args.items:
            disc_id, path, is_folder = catalog.parse_item(item)
            if not cat.disc(disc_id):
                raise SystemExit("Error: no disc %s in the catalogue (items are DISC-ID, DISC-ID:folder/ or "
                                 "DISC-ID:folder/file)" % disc_id)
            paths = _listing_paths(home, disc_id) if path else None
            if path and paths is not None and not (
                    any(p.startswith(path) for p in paths) if is_folder else path in paths):
                if not is_folder and any(p.startswith(path + "/") for p in paths):
                    path += "/"            # a folder given without the trailing slash
                else:
                    raise SystemExit("Error: %s has no %s %s" % (disc_id, "folder" if is_folder else "file", path))
            item = disc_id + (":" + path if path else "")
            if item not in have:
                col.add("Item", item)
                have.add(item)
    elif args.action == "drop":
        gone = {catalog.parse_item(i)[0] + (":" + catalog.parse_item(i)[1] if catalog.parse_item(i)[1] else "")
                for i in args.items}
        col.fields = [(k, v) for k, v in col.fields if not (k == "Item" and v in gone)]
    elif args.action == "move":
        if args.within and code in [c.get("Code") for c in cat.collection_chain(args.within)]:
            raise SystemExit("Error: %s is inside %s; that would make a loop" % (args.within.upper(), code))
        col.fields = [(k, v) for k, v in col.fields if k != "Parent"]
        if args.within:
            col.fields.insert(2, ("Parent", args.within.strip().upper()))
        if args.name:
            col.set("Name", args.name)
    # counts only: item paths could name files on sealed discs
    delta = len(col.get_all("Item")) - items_before
    if delta > 0:
        changes.append("%d item%s added" % (delta, "" if delta == 1 else "s"))
    elif delta < 0:
        changes.append("%d item%s removed" % (-delta, "" if delta == -1 else "s"))
    if args.action == "move" and col.get("Parent") != parent_before:
        changes.append("moved into %s" % (col.get("Parent") or "the top level"))
    if args.action == "move" and col.get("Name") != name_before:
        changes.append("renamed %s -> %s" % (name_before, col.get("Name")))
    if changes:
        catalog.metadata_change(cat, "; ".join(changes), obj="collection:" + code)
    home.save(cat)
    print("%s: %s, %d items" % (code, cat.collection_path(code), len(col.get_all("Item"))))
    return 0


def cmd_access(args):
    def set_access(disc):
        before = catalog.access(disc)
        disc.set("Access", args.level)
        return None if before == args.level else "Access: %s -> %s" % (before, args.level)
    return _edit_disc(args, set_access)


def cmd_location(args):
    """Location records: places (site, room, shelf, box) arranged in a tree."""
    home = catalog.Home(args.home)
    cat = home.load()
    if args.action == "list":
        def show(loc, depth):
            code = loc.get("Code")
            discs = [d for d in cat.discs if code in [l.strip().upper() for l in d.get_all("Location")]]
            inside = [d for d in cat.discs
                      if any(l.strip().upper() in cat.locations_under(code) for l in d.get_all("Location"))]
            count = "%d disc%s" % (len(discs), "" if len(discs) == 1 else "s")
            if len(inside) != len(discs):
                count += " (%d including inside)" % len(inside)
            print("%s%-10s %-30s %s" % ("  " * depth, code, loc.get("Name"), count))
            if args.verbose:
                for d in discs:
                    print("%s  %s  %s" % ("  " * depth + " " * 11, d.get("Id"), d.get("Title")))
            for child in [l for l in cat.locations if (l.get("Parent") or "").upper() == code]:
                show(child, depth + 1)

        for top in [l for l in cat.locations if not cat.location(l.get("Parent"))]:
            show(top, 0)
        loose = sorted({l for d in cat.discs for l in d.get_all("Location") if not cat.location(l)})
        for text in loose:
            print("%-10s (free text, not a location code)" % text)
        return 0
    if not args.code:
        raise SystemExit("Error: arv location %s needs a location code" % args.action)
    code = args.code.strip().upper()
    if not catalog.LOCATION_RE.match(code):
        raise SystemExit("Error: location code %r: use 1-24 capital letters, digits, - or _" % args.code)
    parent = args.within.strip().upper() if args.within else None
    if parent and not cat.location(parent):
        raise SystemExit("Error: no location %s (add it first)" % parent)
    loc = cat.location(code)
    if args.action == "add":
        if loc:
            raise SystemExit("Error: location %s already exists (use 'arv location move' or edit %s)"
                             % (code, home.rec_path))
        loc = recfile.Record("Location", [("Code", code), ("Name", args.name or code)])
        if parent:
            loc.add("Parent", parent)
        if args.description:
            loc.add("Description", args.description)
        cat.locations.append(loc)
        catalog.metadata_change(cat, "created" + (" in %s" % parent if parent else ""), obj="location:" + code)
    elif args.action == "move":
        if not loc:
            raise SystemExit("Error: no location %s" % code)
        if parent and code in [l.get("Code") for l in cat.location_chain(parent)]:
            raise SystemExit("Error: %s is inside %s; that would make a loop" % (parent, code))
        parent_before, name_before = loc.get("Parent"), loc.get("Name")
        if not parent and args.name:
            parent = parent_before  # a rename alone keeps the place where it is
        loc.fields = [(k, v) for k, v in loc.fields if k != "Parent"]
        if parent:
            loc.fields.insert(2, ("Parent", parent))
        if args.name:
            loc.set("Name", args.name)
        changes = []
        if loc.get("Parent") != parent_before:
            changes.append("moved into %s" % (loc.get("Parent") or "the top level"))
        if loc.get("Name") != name_before:
            changes.append("renamed %s -> %s" % (name_before, loc.get("Name")))
        if changes:
            catalog.metadata_change(cat, "; ".join(changes), obj="location:" + code)
    home.save(cat)
    print("%s: %s" % (code, cat.location_path(code)))
    return 0


def _resolve_disc(cat, disc_id, source):
    if not disc_id:
        disc_id = image.read_volume_id(source)
        if not disc_id:
            raise SystemExit("Error: no volume label on %s; pass the disc id explicitly" % source)
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
    if not os.path.exists(source):
        raise SystemExit("Error: %s does not exist" % source)
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
    if args.location:
        where = place(cat, args.location)
        if where not in disc.get_all("Location"):
            disc.add("Location", where)
    note = "burned %d cop%s" % (args.copies, "y" if args.copies == 1 else "ies")
    if args.location:
        note += ", kept at " + cat.location_path(place(cat, args.location))
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
        for kind in catalog.DISC_FILE_KINDS:
            src = catalog.volume_file(snap_dir, kind, disc_id)
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


def cmd_init(args):
    """Create a .arv home in a folder, or a .arv pointer file to an existing home."""
    folder = os.path.abspath(args.folder)
    target = os.path.join(folder, homes.FOLDER)
    if os.path.exists(target):
        raise SystemExit("Error: %s already exists" % target)
    if os.path.isdir(os.path.join(folder, ".git")):
        log("Note: %s is a git repository; a .arv in the folder above it can cover several "
            "repositories and stays out of git" % folder)
    if args.pointer:
        home = os.path.abspath(args.pointer)
        if not os.path.isdir(home):
            raise SystemExit("Error: %s is not a folder" % home)
        print("Wrote %s -> %s" % (homes.write_pointer(folder, home), home))
        return 0
    home = catalog.Home(target)
    for d in (home.config_dir, home.catalog_dir, home.drafts_dir, home.cache_dir):
        home.ensure(d)
    print("Created %s" % target)
    if args.name:
        homes.register(args.name, target, default=args.default)
        print("Registered as %s in %s%s" % (args.name, homes.config_path(), " (default)" if args.default else ""))
    return 0


def cmd_where(args):
    print(args.home)
    print("  found by %s" % args.home_found)
    homes_list = homes.configured()
    if homes_list:
        print("homes on this machine (%s):" % homes.config_path())
        for h in homes_list:
            print("  %-12s %s%s" % (h.get("Name"), h.get("Path"), "  (default)" if h.get("Default") == "yes" else ""))
    return 0


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


def cmd_tags(args):
    """Every folder tag in the catalogue, grouped by namespace, with how often each is used."""
    home = catalog.Home(args.home)
    cat = home.load()
    try:
        vocab = tagger.load_tag_vocab(home, args.vocab)
    except tagger.TagError:
        vocab = None
    known = {n for n, _ in vocab.pairs} if vocab else set()
    usage = {}
    for d in cat.discs:
        path = home.disc_file("tags", d.get("Id"))
        if os.path.exists(path):
            for folder, tags in catalog.read_tags(path).items():
                for t in tags:
                    folders, discs = usage.setdefault(t, (set(), set()))
                    folders.add((d.get("Id"), folder))
                    discs.add(d.get("Id"))
    groups = {}
    for t in usage:
        groups.setdefault(catalog.split_tag(t)[0], []).append(t)
    if args.namespace is not None:
        groups = {args.namespace: groups.get(args.namespace, [])}
    for ns in sorted(groups):
        print(ns + ":" if ns else "(no namespace)")
        for t in sorted(groups[ns]):
            folders, discs = usage[t]
            flag = ""
            if vocab and not ns and t not in known:
                alias = vocab.canonical(t)
                flag = "   (alias of %s)" % alias if alias != t else "   (not in %s)" % vocab.path
            print("  %-28s %d folder%s on %d disc%s%s" % (catalog.split_tag(t)[1] if ns else t,
                  len(folders), "" if len(folders) == 1 else "s", len(discs), "" if len(discs) == 1 else "s", flag))
    return 0


def disc_keywords(home, disc):
    """{folder (relative to data/, '.' for the whole disc): [hierarchical keywords]}."""
    out = {".": [p.replace("/", "|") for p in disc.get_all("Path")] or [disc.get("Set")]}
    path = home.disc_file("tags", disc.get("Id"))
    if os.path.exists(path):
        for folder, tags in catalog.read_tags(path).items():
            out.setdefault(folder, []).extend(catalog.hierarchical(t) for t in tags)
    return out


def cmd_keywords(args):
    """Folder tags and set paths as hierarchical keywords (XMP lr:hierarchicalSubject, as used by
    Lightroom and digiKam), in a TSV or as an exiftool argument file for a restored copy."""
    home = catalog.Home(args.home)
    disc = home.load().disc(args.disc_id)
    if not disc:
        raise SystemExit("Error: no disc %s in %s" % (args.disc_id, home.rec_path))
    keywords = disc_keywords(home, disc)
    if args.format == "tsv":
        print("# folder (relative to data/)\thierarchical keywords (| between levels)")
        for folder, words in keywords.items():
            print("%s\t%s" % (folder, ", ".join(words)))
        return 0
    print("# exiftool argument file for disc %s: from the root of a restored copy, run" % disc.get("Id"))
    print("#   exiftool -@ this-file")
    print("# Each section adds the keywords to every file under one folder (-r). Removing each value")
    print("# before adding it keeps a second run from adding it twice.")
    for folder, words in keywords.items():
        print("-r")
        print("-overwrite_original")
        for w in words:
            leaf = w.rsplit("|", 1)[-1]
            for tag, value in (("XMP-lr:HierarchicalSubject", w), ("XMP-dc:Subject", leaf)):
                print("-%s-=%s" % (tag, value))
                print("-%s+=%s" % (tag, value))
        print("data" if folder == "." else "data/" + folder)
        print("-execute")
    return 0


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
    p = argparse.ArgumentParser(prog="arv", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--home", help="home catalogue folder (default: $ARV_HOME, else the nearest .arv folder "
                                  "or pointer above the current folder, else the machine config's default; "
                                  "see `arv where`)")
    p.add_argument("--archive", metavar="NAME", help="use the home registered under NAME in ~/.config/arv/homes.rec")
    sub = p.add_subparsers(dest="command", required=True)

    it = sub.add_parser("init", help="start an archive: a .arv folder here, or a pointer to one")
    it.add_argument("folder", nargs="?", default=".", help="root of the tree the archive describes (default: here)")
    it.add_argument("--pointer", metavar="HOME", help="write a .arv pointer file to this existing home instead")
    it.add_argument("--name", help="also register the home on this machine under this name (for --archive)")
    it.add_argument("--default", action="store_true", help="with --name: the home used outside any .arv tree")
    it.set_defaults(func=cmd_init)

    wh = sub.add_parser("where", help="which home catalogue is used here, and why")
    wh.set_defaults(func=cmd_where)

    m = sub.add_parser("make", help="build a disc image from a folder")
    m.add_argument("source", help="folder to archive (left unmodified)")
    m.add_argument("-o", "--output", help="image path for a single disc (default: <disc-id>.iso)")
    m.add_argument("--output-dir", help="directory for the images (default: current directory)")
    m.add_argument("--id", help="disc id for a single disc (default: <coverage>_<SET>_<nn>)")
    m.add_argument("--set", help="set code, 2-8 letters or digits, e.g. PHOTOS")
    m.add_argument("--category", action="append",
                   help="extra category code from the vocabulary (repeatable), e.g. --set PROJ --category CODE")
    m.add_argument("--no-rules", action="store_true",
                   help="don't suggest a set and categories from the vocabulary's Match rules")
    m.add_argument("--coverage",
                   help="dates the contents span, in EDTF: 2019, 2015/2024, 2019-07/2019-08, 199X, 1995~ "
                        "(default: from file modification times)")
    m.add_argument("--title")
    m.add_argument("--label", metavar="TEXT",
                   help="volume label text after the disc id (default: the title; '' for the id only). "
                        "Fits 32 characters in all on the hybrid image, 126 on UDF 2.50")
    m.add_argument("--description")
    m.add_argument("--creator")
    m.add_argument("--subject", action="append", help="repeatable")
    m.add_argument("--note", action="append", help="repeatable")
    m.add_argument("--location", help="where the disc will be stored: a code from 'arv location list', or text")
    m.add_argument("--access", choices=catalog.ACCESS_LEVELS, default=catalog.DEFAULT_ACCESS,
                   help="what other discs' catalogues may show of this one: public (also discs given to "
                        "others), private (your own discs; default), sealed (only its id and location)")
    m.add_argument("--rights")
    m.add_argument("--filesystem", choices=list(image.FILESYSTEMS), default="hybrid",
                   help="hybrid (default): ISO9660 + Joliet + UDF 1.02, readable almost anywhere; "
                        "udf250: UDF 2.50 with a metadata partition, as Blu-ray uses (needs src/udfmake)")
    m.add_argument("--udfmake", help="path to the udfmake program (default: PATH, then src/udfmake/build)")
    m.add_argument("--ignore-names", action="store_true",
                   help="don't list names that Windows/macOS will see shortened or changed (see 'arv names')")
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
    m.add_argument("--draft", help="metadata draft JSON from 'arv describe --save'")
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

    nm = sub.add_parser("names", help="check a folder's file names against each disc filesystem's limits")
    nm.add_argument("source")
    nm.add_argument("--filesystem", choices=list(image.FILESYSTEMS), help="only this one (default: all)")
    nm.add_argument("--limit", type=int, default=20, help="issues listed per kind (0: all)")
    nm.set_defaults(func=cmd_names)

    f = sub.add_parser("find", help="search disc descriptions and file lists")
    f.add_argument("pattern", help="substring, or glob if it contains * ? [")
    f.add_argument("--limit", type=int, default=200)
    f.set_defaults(func=cmd_find)

    ls = sub.add_parser("list", help="list discs")
    ls.add_argument("--covers", metavar="DATE",
                    help="only discs whose coverage overlaps this date or range: 2019, 2019-07, 2019-07-15, 2018/2019")
    ls.add_argument("--in", dest="within", metavar="CODE",
                    help="only discs whose set or categories are CODE or anywhere below it (e.g. --in MEMORIES)")
    ls.add_argument("--at", metavar="LOCATION", help="only discs kept at this location or anywhere inside it")
    ls.add_argument("--made", metavar="DATE", help="only discs made in this year or month (2024, 2024-05)")
    ls.add_argument("--access", choices=catalog.ACCESS_LEVELS, help="only discs with this access level")
    ls.set_defaults(func=cmd_list)

    st = sub.add_parser("sets", help="show the set vocabulary (a word hierarchy) and discs per set")
    st.add_argument("-v", "--verbose", action="store_true", help="also show scope notes, aliases and match rules")
    st.set_defaults(func=cmd_sets)

    di = sub.add_parser("id", help="explain and check a disc id (catches typos)")
    di.add_argument("disc_id")
    di.set_defaults(func=cmd_id)

    n = sub.add_parser("note", help="add a note to a disc")
    n.add_argument("disc_id")
    n.add_argument("text")
    n.set_defaults(func=cmd_note)

    loc = sub.add_parser("locate", help="set where a disc's copies are kept (one location per place)")
    loc.add_argument("disc_id")
    loc.add_argument("location", nargs="+", help="location codes (see 'arv location') or text")
    loc.add_argument("--add", action="store_true", help="add to the disc's locations instead of replacing them")
    loc.set_defaults(func=cmd_locate)

    lo = sub.add_parser("location", help="places where discs are kept (site, room, shelf, box), as a tree")
    lo.add_argument("action", choices=["list", "add", "move"])
    lo.add_argument("code", nargs="?", help="location code, e.g. BOX3")
    lo.add_argument("name", nargs="?", help="readable name (add), e.g. 'Box 3, blue lid'")
    lo.add_argument("--in", dest="within", metavar="PARENT", help="the location it is inside (add, move)")
    lo.add_argument("--description")
    lo.add_argument("-v", "--verbose", action="store_true", help="list: show the discs at each place")
    lo.set_defaults(func=cmd_location)

    co = sub.add_parser("collection", help="virtual folders of discs, folders and files across discs")
    co.add_argument("action", choices=["list", "show", "add", "put", "drop", "move"])
    co.add_argument("code", nargs="?", help="collection code, e.g. KYOTO-BEST")
    co.add_argument("items", nargs="*", help="add/put/drop: DISC-ID, DISC-ID:folder/ or DISC-ID:folder/file")
    co.add_argument("--name", help="readable name (add, move)")
    co.add_argument("--in", dest="within", metavar="PARENT", help="the collection it is inside (add, move)")
    co.add_argument("--description")
    co.set_defaults(func=cmd_collection)

    ac = sub.add_parser("access", help="set what other discs' catalogues may show of a disc")
    ac.add_argument("disc_id")
    ac.add_argument("level", choices=catalog.ACCESS_LEVELS)
    ac.set_defaults(func=cmd_access)
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
    b.add_argument("--location", help="where these copies are kept (added to the disc's locations)")
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
    ds.add_argument("--save", help="write the reviewed result as a draft JSON for 'arv make --draft'")
    ds.add_argument("--disc-root", help="mounted disc, so README-style files on it can be read")
    ds.add_argument("--show-inventory", action="store_true", help="print exactly what would be sent, and stop")
    ds.add_argument("--apply", metavar="DRAFT", help="apply a saved draft to the disc (no LLM needed)")
    add_llm_options(ds)
    ds.set_defaults(func=cmd_describe)

    tg = sub.add_parser("tag", help="suggest folder tags from your tag vocabulary (small built-in model)")
    tg.add_argument("target", help="folder to be archived, or a disc id")
    tg.add_argument("--top", type=int, default=3, help="at most this many tags per folder (default: 3)")
    tg.add_argument("--vocab", help="tag vocabulary recfile (default: <home>/config/tags.rec)")
    tg.add_argument("--save", help="write (or merge into) a draft JSON for 'arv make --draft'")
    tg.add_argument("--apply", action="store_true", help="for a disc: write the tags without prompting")
    tg.add_argument("--disc-root", help="mounted disc, so README files on it can be read")
    tg.add_argument("--show-summaries", action="store_true", help="print what the model compares, and stop")
    tg.add_argument("--rules-only", action="store_true",
                    help="only the vocabulary's Match rules (no model needed)")
    tg.add_argument("--llama-embedding", help="path to llama.cpp's llama-embedding")
    tg.add_argument("--model", default=models.DEFAULT_EMBEDDING, help="built-in model (default: %(default)s)")
    tg.add_argument("--embed-url", help="use an OpenAI-compatible /v1/embeddings server instead of the built-in model")
    tg.add_argument("--embed-model", help="embedding model name on that server (default: its first model)")
    tg.add_argument("--llm-allow-remote", action="store_true", help="allow a non-local embeddings server")
    tg.set_defaults(func=cmd_tag)

    tl = sub.add_parser("tags", help="every folder tag in use, grouped by namespace (person:, place:, ...)")
    tl.add_argument("--namespace", help="only this namespace ('' for tags without one)")
    tl.add_argument("--vocab", help="tag vocabulary recfile (default: <home>/config/tags.rec)")
    tl.set_defaults(func=cmd_tags)

    kw = sub.add_parser("keywords", help="a disc's set paths and folder tags as hierarchical keywords (XMP)")
    kw.add_argument("disc_id")
    kw.add_argument("--format", choices=["tsv", "exiftool"], default="tsv",
                    help="tsv (default) or an exiftool -@ argument file that writes them into a restored copy")
    kw.set_defaults(func=cmd_keywords)

    mo = sub.add_parser("models", help="built-in model for 'arv tag': fetch, status, build-runtime")
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
    parser = build_parser()
    args, extra = parser.parse_known_args(argv)
    if extra:
        # `collection add CODE --name X ITEM ...`: items after options are still items
        if args.command == "collection" and not any(e.startswith("-") for e in extra):
            args.items += extra
        else:
            parser.error("unrecognized arguments: %s" % " ".join(extra))
    if args.command != "init":
        source = getattr(args, "source", None) if args.command == "make" else None
        args.home, args.home_found = homes.find(args.home, args.archive, source=source)
    try:
        return args.func(args)
    except BrokenPipeError:  # output piped into e.g. `head`, which stopped reading
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        return 0
