"""`archive make`: plan, stage, build and protect one or more disc images.

A folder that does not fit on one disc (at the requested minimum RS03
redundancy) can be split with --split: files are assigned in path order, each
disc becomes its own complete bag, and every disc of the batch carries the
catalogue of the whole batch. Sizes are checked exactly with
`genisoimage -print-size` before any image is written.
"""

import os
import shutil
import sys
import tempfile
import uuid
from dataclasses import dataclass, field

from . import bag, catalog, discid, formats, html, image, index, media, recfile, rocrate, web

FILESYSTEM = "ISO9660 level 3 + Rock Ridge + Joliet, UDF 1.02 bridge"


def log(msg):
    print(msg, file=sys.stderr)


@dataclass
class Plan:
    entries: list
    disc_id: str = ""
    record: recfile.Record = None
    part: int = 1
    parts: int = 1
    sequence: int = 1
    out: str = ""
    stage: str = ""
    sectors: int = 0
    events: list = field(default_factory=list)
    extras: list = field(default_factory=list)  # [(Entry, source path)] added to data/, e.g. RO-Crate files

    @property
    def payload_entries(self):
        return self.entries + [e for e, _ in self.extras]


def estimate_sectors(entry):
    """Rough image cost of one file: its data, a UDF file entry, and directory records."""
    return -(-entry.size // media.SECTOR) + 1 + (3 * (len(entry.path) + 64)) // media.SECTOR + 1


def greedy_split(entries, limit):
    bins, current, used = [], [], 0
    for e in entries:
        cost = estimate_sectors(e)
        if cost > limit:
            raise SystemExit("Error: %s (%d bytes) is larger than one disc can hold" % (e.path, e.size))
        if current and used + cost > limit:
            bins.append(current)
            current, used = [], 0
        current.append(e)
        used += cost
    if current or not bins:
        bins.append(current)
    return bins


def dir_sectors(path):
    total = 0
    for root, _, names in os.walk(path):
        for n in names:
            total += -(-os.path.getsize(os.path.join(root, n)) // media.SECTOR) + 1
    return total


class Maker:
    def __init__(self, args, meta, entries, src, home, cat, version, is_git, stage_tools, write_readme):
        self.args, self.meta, self.entries, self.src = args, meta, entries, src
        self.home, self.cat, self.version, self.is_git = home, cat, version, is_git
        self.stage_tools, self.write_readme = stage_tools, write_readme
        if args.medium_sectors:
            self.capacity = args.medium_sectors
        elif args.medium == "auto":
            self.capacity = None
        else:
            self.capacity = media.capacity(args.medium, not args.no_defect_management)
        self.budget = None if self.capacity is None else media.data_budget(self.capacity, args.min_redundancy)
        self.workdir = None
        self.plans = []
        self.formats = None  # (header, {path: row}) from Siegfried

    # ------------------------------------------------------------ planning

    def initial_bins(self):
        if not self.args.split or self.budget is None:
            if self.args.split:
                raise SystemExit("Error: --split needs a target --medium (not auto)")
            return [self.entries]
        tools = os.path.join(self.workdir, "tools-probe")
        self.stage_tools(tools, self.is_git, self.args.extra_tools, self.args.tools_history)
        reserve = dir_sectors(tools) + self.snapshot_estimate() + self.budget // 200 + 1024
        shutil.rmtree(tools)
        return greedy_split(self.entries, self.budget - reserve)

    def snapshot_estimate(self):
        """Sectors for catalog/ (prior discs' files + this batch's lists, twice: listings + web)."""
        total = 0
        for d in self.prior_discs():
            for p in self.home.disc_files(d.get("Id")).values():
                total += 2 * os.path.getsize(p)
        total += sum(4 * (len(e.path) + 200) for e in self.entries)  # manifests, listings, web, formats
        return total // media.SECTOR + 256

    def prior_discs(self):
        if self.args.snapshot == "full":
            return list(self.cat.discs)
        if self.args.snapshot == "set":
            return [d for d in self.cat.discs if d.get("Set") == self.meta["set"]]
        return []

    def assign(self, bins):
        meta, n = self.meta, len(bins)
        if self.args.id and n > 1:
            raise SystemExit("Error: --id cannot be used when the folder is split across %d discs" % n)
        first = self.cat.next_number(meta["set"])
        self.plans = []
        for i, entries in enumerate(bins):
            try:
                disc_id = self.args.id or discid.compose(meta["set"], first + i, meta["coverage"])
            except discid.IdError as err:
                raise SystemExit("Error: %s" % err)
            if not catalog.ID_RE.match(disc_id) or len(disc_id) > image.MAX_VOLID_LEN:
                raise SystemExit("Error: invalid disc id %r (letters, digits, _ . -; at most %d characters)"
                                 % (disc_id, image.MAX_VOLID_LEN))
            if self.cat.disc(disc_id):
                raise SystemExit("Error: disc id %s already exists in %s" % (disc_id, self.home.rec_path))
            if self.args.output and n == 1:
                out = os.path.abspath(self.args.output)
            else:
                out = os.path.join(os.path.abspath(self.args.output_dir or "."), disc_id + ".iso")
            if os.path.exists(out):
                raise SystemExit("Error: %s already exists" % out)
            plan = Plan(entries=entries, disc_id=disc_id, part=i + 1, parts=n, out=out, sequence=first + i)
            plan.record = self.disc_record(plan)
            plan.events = [catalog.new_event(disc_id, "message digest calculation", "success", self.version,
                                             "sha256 and sha512 manifests of %d files" % len(entries))]
            if self.meta.get("draft_agent"):
                plan.events.append(catalog.new_event(
                    disc_id, "metadata modification", "success", "%s + owner review" % self.meta["draft_agent"],
                    "title, description, subjects and folder tags drafted with a local LLM and reviewed by the owner"))
            if self.formats:
                header, rows = self.formats
                unknown = sum(1 for e in entries if (rows.get(e.path) or {}).get("puid", "UNKNOWN") == "UNKNOWN")
                plan.events.append(catalog.new_event(
                    disc_id, "format identification", "success" if not unknown else "warning",
                    header.splitlines()[0].lstrip("# ") if header else "siegfried",
                    "PRONOM ids for %d files, %d unidentified" % (len(entries), unknown)))
            self.plans.append(plan)

    @property
    def medium_label(self):
        if self.args.medium_sectors:
            return "custom medium"
        return media.label(self.args.medium) if self.args.medium != "auto" else "auto"

    @property
    def group_id(self):
        if len(self.plans) < 2:
            return None
        return "%s-%02d-%02d" % (self.meta["set"], self.plans[0].sequence, self.plans[-1].sequence)

    def disc_record(self, plan):
        m, a = self.meta, self.args
        # Uuid: machine identity of this image (copies burned from it share it); Id is for humans
        # Id is derived from IdScheme + Set + Sequence + Coverage, so it can be regenerated and checked
        r = recfile.Record("Disc", [("Id", plan.disc_id), ("Uuid", str(uuid.uuid4()))])
        if not a.id:
            r.add("IdScheme", discid.SCHEME)
        r.fields += [("Title", m["title"]), ("Set", m["set"])]
        if m.get("set_class") is not None:
            r.add("SetClass", "%03d" % m["set_class"])  # place in the set vocabulary at burn time
        r.fields += [("Sequence", str(plan.sequence)), ("Coverage", m["coverage"]), ("Date", catalog.today())]
        if plan.parts > 1:
            r.add("Part", "%d of %d" % (plan.part, plan.parts))
        for key in ("creator", "description"):
            if m.get(key):
                r.add(key.capitalize(), m[key])
        for s in m["subjects"]:
            r.add("Subject", s)
        for n in m["notes"]:
            r.add("Note", n)
        if m.get("location"):
            r.add("Location", m["location"])
        if a.rights:
            r.add("Rights", a.rights)
        if a.no_ecc:
            ecc = "none"
        elif self.capacity:
            ecc = "dvdisaster RS03 augmented image, %s (%d sectors), minimum %s%% redundancy" % (
                self.medium_label, self.capacity, a.min_redundancy)
        else:
            ecc = "dvdisaster RS03 augmented image"
        for k, v in [("Media", a.media or ("M-DISC " + (self.medium_label if self.capacity else "BD-R"))),
                     ("Files", len(plan.entries)), ("Bytes", sum(e.size for e in plan.entries)),
                     ("Filesystem", FILESYSTEM), ("Ecc", ecc), ("Software", self.version)]:
            r.add(k, str(v))
        return r

    # ------------------------------------------------------------ staging

    def batch_files(self):
        """Write every batch disc's manifest, listing and formats once; the stages copy from here.

        Returns {disc_id: {kind: path}}. Also creates each disc's RO-Crate files (--ro-crate).
        """
        batch = os.path.join(self.workdir, "batch")
        os.makedirs(batch, exist_ok=True)
        out = {}
        for p in self.plans:
            p.extras = self.rocrate_files(p, batch) if self.args.ro_crate else []
            files = {"manifests": os.path.join(batch, p.disc_id + ".sha256"),
                     "listings": os.path.join(batch, p.disc_id + ".tsv")}
            bag.write_manifest(files["manifests"], [(e.hashes["sha256"], "data/" + e.path) for e in p.payload_entries])
            web.write_listing(files["listings"], p.payload_entries)
            tags, captions = self.plan_tags(p)
            if tags or captions:
                files["tags"] = os.path.join(batch, p.disc_id + ".tags")
                catalog.write_tags(files["tags"], tags, captions)
            if self.formats:
                files["formats"] = os.path.join(batch, p.disc_id + ".csv")
                formats.write(files["formats"], self.formats[0], self.formats[1], p.entries)
            out[p.disc_id] = files
        return out

    def plan_tags(self, plan):
        """Folder tags and image captions for the folders that are on this disc."""
        tags = self.meta.get("folder_tags") or {}
        captions = self.meta.get("folder_captions") or {}
        if not tags and not captions:
            return {}, {}
        folders = {"."}
        for e in plan.entries:
            parts = e.path.split("/")[:-1]
            folders.update("/".join(parts[:i]) for i in range(1, len(parts) + 1))
        return ({f: t for f, t in tags.items() if f in folders},
                {f: c for f, c in captions.items() if f in folders})

    def rocrate_files(self, plan, batch):
        folder = os.path.join(batch, plan.disc_id + "-rocrate")
        os.makedirs(folder, exist_ok=True)
        doc = rocrate.build(plan.record, plan.entries, self.formats[1] if self.formats else None)
        extras = []
        for name, text in ((rocrate.METADATA, rocrate.dumps(doc)), (rocrate.PREVIEW, rocrate.preview(plan.record, plan.entries))):
            path = os.path.join(folder, name)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            st = os.stat(path)
            extras.append((bag.Entry(name, st.st_size, st.st_mtime, bag.hash_file(path)), path))
        return extras

    def stage(self, plan, batch):
        a = self.args
        stage = tempfile.mkdtemp(prefix="stage-%s-" % plan.disc_id, dir=self.workdir)
        plan.stage = stage
        info = [("Bagging-Date", catalog.today()),
                ("External-Identifier", plan.disc_id),
                ("External-Description", self.meta["title"] + (" - " + self.meta["description"] if self.meta.get("description") else "")),
                ("Bag-Group-Identifier", self.group_id or self.meta["set"])]
        if plan.parts > 1:
            info.append(("Bag-Count", "%d of %d" % (plan.part, plan.parts)))
        info += [("Payload-Oxum", bag.payload_oxum(plan.payload_entries)), ("Bag-Software-Agent", self.version)]
        bag.write_bag_tags(stage, plan.payload_entries, info)

        if a.snapshot == "disc":
            batch_plans = [plan]
        else:
            batch_plans = self.plans
        prior = self.prior_discs()
        snapshot = self.cat.subset({d.get("Id") for d in prior})
        snapshot.discs += [p.record for p in batch_plans]
        snapshot.events += [e for p in batch_plans for e in p.events]
        files = {d.get("Id"): self.home.disc_files(d.get("Id")) for d in prior}
        files.update({p.disc_id: batch[p.disc_id] for p in batch_plans})
        catalog_dir = os.path.join(stage, "catalog")
        catalog.write_snapshot(catalog_dir, snapshot, files, a.snapshot)
        listings = os.path.join(catalog_dir, "listings")
        tags_dir = os.path.join(catalog_dir, "tags")
        folder_tags = {n[:-5]: catalog.read_tag_info(os.path.join(tags_dir, n))
                       for n in (os.listdir(tags_dir) if os.path.isdir(tags_dir) else [])}
        web.write_web_data(os.path.join(catalog_dir, "web"), plan.disc_id, snapshot.discs,
                           {n[:-4]: os.path.join(listings, n) for n in os.listdir(listings)}, folder_tags)

        self.stage_tools(os.path.join(stage, "tools"), self.is_git, a.extra_tools, a.tools_history)
        self.write_readme(os.path.join(stage, "README.txt"), plan.record, a.snapshot, a.tools_history)
        with open(os.path.join(stage, "index.html"), "w", encoding="utf-8") as f:
            f.write(html.render_index(plan.record, plan.payload_entries, snapshot))
        with open(os.path.join(stage, "search.html"), "w", encoding="utf-8") as f:
            f.write(web.render_search())

        # catalog.rec: the disc's entry point (Archive record, see docs/smart-archive-format.md),
        # then this disc's own Disc and Event records. Written last so it can point to every file.
        own = catalog.Catalog()
        own.discs, own.events = [plan.record], list(plan.events)
        recfile.write(os.path.join(stage, "catalog.rec"),
                      catalog.archive_records(plan.record, stage) + own.records())
        bag.write_tagmanifests(stage)

    def payload(self, plan):
        extras = [(e.path, src) for e, src in plan.extras]
        if plan.parts == 1 and len(plan.entries) == len(self.entries):
            return {"payload_dir": self.src, "payload_files": extras or None}
        return {"payload_files": [(e.path, os.path.join(self.src, e.path)) for e in plan.entries] + extras}

    def fit(self):
        """Stage every disc and measure it; move files forward until every disc fits."""
        bins = self.initial_bins()
        attempts = len(self.entries) + 10
        for _ in range(attempts):
            self.assign(bins)
            batch = self.batch_files()
            over = None
            for i, plan in enumerate(self.plans):
                self.stage(plan, batch)
                plan.sectors = image.print_size(plan.stage, plan.disc_id, **self.payload(plan))
                if self.budget is not None and plan.sectors > self.budget and over is None:
                    over = i
            if over is None:
                return
            plan = self.plans[over]
            if len(plan.entries) == 1:
                raise SystemExit("Error: %s does not fit on one disc together with the catalogue and tools"
                                 % plan.entries[0].path)
            if not self.args.split:
                need = -(-plan.sectors // self.budget)
                raise SystemExit(
                    "Error: this folder needs %s but a %s holds %s at %s%% minimum redundancy.\n"
                    "Use --split (about %d discs), a larger --medium, or a lower --min-redundancy."
                    % (html.human_size(plan.sectors * media.SECTOR), self.medium_label,
                       html.human_size(self.budget * media.SECTOR), self.args.min_redundancy, need))
            excess = plan.sectors - self.budget + 64
            moved, cost = [], 0
            while len(plan.entries) > 1 and cost < excess:
                e = plan.entries.pop()
                moved.insert(0, e)
                cost += -(-e.size // media.SECTOR) + 1  # real size, not the estimate that was wrong
            bins = [p.entries for p in self.plans]
            if over + 1 < len(bins):
                bins[over + 1] = moved + bins[over + 1]
            else:
                bins.append(moved)
            bins = [b for b in bins if b]
            for p in self.plans:
                shutil.rmtree(p.stage, ignore_errors=True)
            shutil.rmtree(os.path.join(self.workdir, "batch"), ignore_errors=True)
            log("Rebalancing: disc %d was %d sectors over budget" % (over + 1, plan.sectors - self.budget))
        raise SystemExit("Error: could not fit the files onto discs after %d attempts" % attempts)

    # ------------------------------------------------------------ building

    def build(self, plan):
        a = self.args
        log("Building %s (%d of %d, %s) ..." % (plan.out, plan.part, plan.parts, html.human_size(plan.sectors * media.SECTOR)))
        image.build_iso(plan.stage, plan.out, plan.disc_id, **self.payload(plan))
        note = "image %s, %d sectors" % (os.path.basename(plan.out), plan.sectors)
        plan.events.append(catalog.new_event(plan.disc_id, "creation", "success", self.version, note))
        if a.no_ecc:
            return
        log("Adding dvdisaster RS03 error correction ...")
        output = image.add_ecc(plan.out, medium_sectors=self.capacity)
        for line in output.splitlines():
            if "redundancy" in line:
                plan.events[-1].set("Note", note + "; RS03: " + line.strip())
        if not a.no_verify:
            log("Verifying with dvdisaster -t ...")
            ok, output = image.verify_ecc(plan.out)
            plan.events.append(catalog.new_event(plan.disc_id, "fixity check", "success" if ok else "failure",
                                                 "dvdisaster", "image test after creation"))
            if not ok:
                log(output)
                log("Error: dvdisaster verification failed for %s" % plan.disc_id)

    def run(self):
        a = self.args
        out_dir = os.path.dirname(os.path.abspath(a.output)) if a.output else os.path.abspath(a.output_dir or ".")
        os.makedirs(out_dir, exist_ok=True)
        self.workdir = tempfile.mkdtemp(prefix=".archive-make-", dir=out_dir)
        try:
            if a.ro_crate:
                clash = [e.path for e in self.entries if e.path in (rocrate.METADATA, rocrate.PREVIEW)]
                if clash:
                    raise SystemExit("Error: --ro-crate would overwrite %s in the source folder" % ", ".join(clash))
            if a.formats == "yes" or (a.formats == "auto" and formats.available()):
                if not formats.available():
                    raise SystemExit("Error: --formats yes needs Siegfried (sf) on PATH")
                log("Identifying file formats with Siegfried ...")
                try:
                    self.formats = formats.identify(self.src, a.sf_home)
                except formats.FormatsError as err:
                    if a.formats == "yes":
                        raise SystemExit("Error: Siegfried failed: %s" % err)
                    log("Warning: skipping format identification, Siegfried failed: %s" % err)
            self.fit()
            if self.capacity and not a.no_ecc and not image.dvdisaster_sets_medium_size():
                log("Warning: this dvdisaster build ignores the target medium size for RS03 and picks the "
                    "smallest standard medium that fits, so small images get less error correction than "
                    "the disc could hold. Use the speed47 fork (https://github.com/speed47/dvdisaster).")
            for plan in self.plans:
                self.build(plan)
            for plan in self.plans:
                self.cat.discs.append(plan.record)
                self.cat.events.extend(plan.events)
                self.home.store_disc_files(plan.disc_id, {
                    kind: os.path.join(plan.stage, "catalog", kind, plan.disc_id + ext)
                    for kind, ext in catalog.DISC_FILE_KINDS.items()})
            self.home.save(self.cat)
            if os.path.exists(self.home.sqlite_path):
                index.build(self.home, self.cat)
        finally:
            if a.keep_stage:
                log("Kept staging directory %s" % self.workdir)
            else:
                shutil.rmtree(self.workdir, ignore_errors=True)
        for plan in self.plans:
            print("%s\t%s\t%s" % (plan.disc_id, plan.out, self.meta["title"]))
        ok = not any(e.get("Outcome") == "failure" for p in self.plans for e in p.events)
        return 0 if ok else 1


def sanitize_set(name):
    """Set code for ids: 2-8 capital letters or digits (longer names are shortened)."""
    try:
        return discid.normalise_set(name)
    except discid.IdError:
        return "ARCHIVE"
