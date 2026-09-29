"""archive: build self-describing, error-corrected archival disc images.

Commands:
  make   bag a folder, write the catalogue + viewer, build the image, add RS03 ECC
  find   search every disc's catalogue and file list (no discs needed)
  list   list discs in the home catalogue
  note   add a note to a disc
  locate set where a disc is physically stored
"""

import argparse
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

from . import bag, catalog, html, image, recfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_NAME = "bluray-archival-workflow"
DEFAULT_MEDIA = "M-DISC BD-R"
FILESYSTEM = "ISO9660 level 3 + Rock Ridge + Joliet, UDF 1.02 bridge"
ECC = "dvdisaster RS03 augmented image"


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


def stage_tools(tools_dir, is_git, extra_tools):
    """Copy this tool (plain tree + git bundle), bagit.py and any extra tools onto the disc."""
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
        bundle = os.path.join(tools_dir, REPO_NAME + ".bundle")
        proc = subprocess.run(["git", "-C", REPO_ROOT, "bundle", "create", bundle, "--all"],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            log("Warning: git bundle failed, disc gets the plain tree only:\n" + proc.stderr.strip())
            if os.path.exists(bundle):
                os.remove(bundle)
    else:
        shutil.copytree(REPO_ROOT, tree, ignore=shutil.ignore_patterns("__pycache__", "*.pyc", ".git", "*.iso"))
    shutil.copyfile(os.path.join(REPO_ROOT, "archivetool", "vendor", "bagit.py"),
                    os.path.join(tools_dir, "bagit.py"))
    if extra_tools:
        shutil.copytree(extra_tools, os.path.join(tools_dir, "extra"))


README_TEMPLATE = """\
{title}
{underline}

Disc id:  {id}
Set:      {set}
Burned:   {date}
Contents: {files} files, {bytes} bytes (in data/)
Made by:  {software}

This disc is a BagIt bag (RFC 8493) with dvdisaster RS03 error correction
data stored after the filesystem.

BROWSE
  Open index.html in any web browser. It lists every file on this disc
  and{other_discs}.

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
  tools/{repo}.bundle     the same with full history: git clone <bundle>
  tools/bagit.py          BagIt validator (public domain)
"""


def write_readme(path, disc, snapshot_scope):
    if snapshot_scope == "none":
        other, cat_lines = "", ""
    else:
        other = " the discs made before it (%s catalogue)" % snapshot_scope
        cat_lines = ("  catalog/archive.rec     all discs in the archive as of the burn date\n"
                     "  catalog/manifests/      file lists (sha256) of those discs\n")
    title = disc.get("Title")
    text = README_TEMPLATE.format(
        title=title, underline="=" * len(title), id=disc.get("Id"), set=disc.get("Set"),
        date=disc.get("Date"), files=disc.get("Files"), bytes=disc.get("Bytes"),
        software=disc.get("Software"), other_discs=(" lists" + other) if other else " its notes",
        catalog_lines=cat_lines, repo=REPO_NAME,
    )
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


# ---------------------------------------------------------------- commands

def cmd_make(args):
    src = os.path.abspath(args.source)
    if not os.path.isdir(src):
        raise SystemExit("Error: %s is not a directory" % src)
    image.require("genisoimage", *(["dvdisaster"] if not args.no_ecc else []))
    interactive = sys.stdin.isatty() and not args.yes
    home = catalog.Home(args.home)
    cat = home.load()

    log("Scanning and hashing %s ..." % src)
    entries = bag.scan_payload(src)
    total_bytes = sum(e.size for e in entries)

    default_title, default_set = folder_defaults(src)
    set_name = args.set or ask("Set name", default_set, interactive)
    set_name = re.sub(r"[^A-Za-z0-9-]", "", set_name).upper() or "ARCHIVE"
    coverage = args.coverage or catalog.coverage_years(entries)
    disc_id = args.id or catalog.make_disc_id(coverage, set_name, cat.next_number(set_name))
    if not catalog.ID_RE.match(disc_id):
        raise SystemExit("Error: invalid disc id %r" % disc_id)
    if cat.disc(disc_id):
        raise SystemExit("Error: disc id %s already exists in %s" % (disc_id, home.rec_path))

    title = args.title or ask("Title", default_title, interactive)
    description = args.description or ask("Description (optional)", None, interactive)
    creator = args.creator or ask("Creator", os.environ.get("USER"), interactive)
    location = args.location or ask("Physical location (optional)", None, interactive)
    subjects = args.subject or [s.strip() for s in (ask("Subjects, comma separated (optional)", None, interactive) or "").split(",") if s.strip()]
    notes = args.note or ([n] if (n := ask("Note (optional)", None, interactive)) else [])
    version, is_git = software_version()

    disc = recfile.Record("Disc", [("Id", disc_id), ("Title", title), ("Set", set_name),
                                   ("Coverage", coverage), ("Date", catalog.today())])
    if creator:
        disc.add("Creator", creator)
    if description:
        disc.add("Description", description)
    for s in subjects:
        disc.add("Subject", s)
    for n in notes:
        disc.add("Note", n)
    if location:
        disc.add("Location", location)
    if args.rights:
        disc.add("Rights", args.rights)
    for k, v in [("Media", args.media), ("Files", len(entries)), ("Bytes", total_bytes),
                 ("Filesystem", FILESYSTEM), ("Ecc", "none" if args.no_ecc else ECC), ("Software", version)]:
        disc.add(k, str(v))

    out = os.path.abspath(args.output or disc_id + ".iso")
    if os.path.exists(out):
        raise SystemExit("Error: %s already exists" % out)
    stage = tempfile.mkdtemp(prefix=".stage-%s-" % disc_id, dir=os.path.dirname(out))
    log("Disc id: %s\nStaging tag files in %s" % (disc_id, stage))
    try:
        bag.write_bag_tags(stage, entries, [
            ("Bagging-Date", catalog.today()),
            ("External-Identifier", disc_id),
            ("External-Description", title + (" - " + description if description else "")),
            ("Bag-Group-Identifier", set_name),
            ("Payload-Oxum", bag.payload_oxum(entries)),
            ("Bag-Software-Agent", version),
        ])

        digest_event = catalog.new_event(disc_id, "message digest calculation", "success", version,
                                         "sha256 and sha512 manifests of %d files" % len(entries))
        on_disc = catalog.Catalog()
        on_disc.discs, on_disc.events = [disc], [digest_event]
        recfile.write(os.path.join(stage, "catalog.rec"), on_disc.records())

        snapshot = None
        if args.snapshot != "none":
            prior = cat.discs if args.snapshot == "full" else [d for d in cat.discs if d.get("Set") == set_name]
            ids = {d.get("Id") for d in prior}
            snapshot = cat.subset(ids)
            snapshot.discs.append(disc)
            snapshot.events.append(digest_event)
            sources = {i: home.manifest_path(i) for i in ids if os.path.exists(home.manifest_path(i))}
            sources[disc_id] = os.path.join(stage, "manifest-sha256.txt")
            catalog.write_snapshot(os.path.join(stage, "catalog"), snapshot, sources, args.snapshot)

        stage_tools(os.path.join(stage, "tools"), is_git, args.extra_tools)
        write_readme(os.path.join(stage, "README.txt"), disc, args.snapshot)
        with open(os.path.join(stage, "index.html"), "w", encoding="utf-8") as f:
            f.write(html.render_index(disc, entries, snapshot))
        bag.write_tagmanifests(stage)

        log("Building image %s ..." % out)
        image.build_iso(stage, src, out, disc_id)

        events = [digest_event, catalog.new_event(disc_id, "creation", "success", version, "image " + os.path.basename(out))]
        if not args.no_ecc:
            log("Adding dvdisaster RS03 error correction ...")
            image.add_ecc(out)
            if not args.no_verify:
                log("Verifying with dvdisaster -t ...")
                ok, output = image.verify_ecc(out)
                events.append(catalog.new_event(disc_id, "fixity check", "success" if ok else "failure",
                                                "dvdisaster", "image test after creation"))
                if not ok:
                    log(output)
                    log("Error: dvdisaster verification failed")

        cat.discs.append(disc)
        cat.events.extend(events)
        home.store_manifest(disc_id, os.path.join(stage, "manifest-sha256.txt"))
        home.save(cat)
    finally:
        if args.keep_stage:
            log("Kept staging directory %s" % stage)
        else:
            shutil.rmtree(stage, ignore_errors=True)

    print("%s\t%s\t%s" % (disc_id, out, title))
    return 0 if all(e.get("Outcome") == "success" for e in events) else 1


def cmd_find(args):
    home = catalog.Home(args.home)
    cat = home.load()
    disc_hits, file_hits = catalog.find(home, cat, args.pattern)
    for d in disc_hits:
        print("DISC  %s  %s  [%s]" % (d.get("Id"), d.get("Title"), d.get("Location", "location unknown")))
    for d, path in file_hits[: args.limit] if args.limit else file_hits:
        print("%s  [%s]  %s" % (d.get("Id"), d.get("Location", "?"), path))
    if args.limit and len(file_hits) > args.limit:
        print("... %d more file matches (use --limit 0 for all)" % (len(file_hits) - args.limit))
    return 0 if disc_hits or file_hits else 1


def cmd_list(args):
    cat = catalog.Home(args.home).load()
    for d in cat.discs:
        print("%s\t%s\t%s\t%s files\t%s" % (d.get("Id"), d.get("Date"), d.get("Title"),
                                           d.get("Files"), d.get("Location", "")))
    return 0


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


def build_parser():
    p = argparse.ArgumentParser(prog="archive", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--home", help="catalogue directory (default: $BLURAY_ARCHIVE_HOME or ~/.local/share/bluray-archive)")
    sub = p.add_subparsers(dest="command", required=True)

    m = sub.add_parser("make", help="build a disc image from a folder")
    m.add_argument("source", help="folder to archive (left unmodified)")
    m.add_argument("-o", "--output", help="image path (default: <disc-id>.iso)")
    m.add_argument("--id", help="disc id (default: <coverage>_<SET>_<nn>)")
    m.add_argument("--set", help="set name, e.g. PHOTOS")
    m.add_argument("--coverage", help="year range (default: from file modification times)")
    m.add_argument("--title")
    m.add_argument("--description")
    m.add_argument("--creator")
    m.add_argument("--subject", action="append", help="repeatable")
    m.add_argument("--note", action="append", help="repeatable")
    m.add_argument("--location", help="where the disc will be stored")
    m.add_argument("--rights")
    m.add_argument("--media", default=DEFAULT_MEDIA)
    m.add_argument("--snapshot", choices=["full", "set", "none"], default="full",
                   help="catalogue of other discs to include: full (default), set (this set only, "
                        "for discs given to other people), none")
    m.add_argument("--extra-tools", help="folder copied to tools/extra/ (e.g. dvdisaster binaries)")
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
    ls.set_defaults(func=cmd_list)

    n = sub.add_parser("note", help="add a note to a disc")
    n.add_argument("disc_id")
    n.add_argument("text")
    n.set_defaults(func=cmd_note)

    loc = sub.add_parser("locate", help="set a disc's physical location")
    loc.add_argument("disc_id")
    loc.add_argument("location")
    loc.set_defaults(func=cmd_locate)
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    return args.func(args)
