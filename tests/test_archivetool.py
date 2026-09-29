"""Tests for the archive CLI. Run: python3 -m unittest discover -s tests

Image tests need genisoimage and 7z; the ECC test additionally needs dvdisaster
and only runs with ARCHIVE_TEST_ECC=1 (it takes several minutes).
"""

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, REPO)

from archivetool import bag, catalog, cli, image, make, media, recfile  # noqa: E402

HAVE_IMAGE_TOOLS = all(shutil.which(t) for t in ("genisoimage", "7z"))

from archivetool import discid  # noqa: E402

PROJECTS_01 = discid.compose("PROJECTS", 1, "2020/2025")
PHOTOS_01 = discid.compose("PHOTOS", 1, "2023")
PHOTOS_02 = discid.compose("PHOTOS", 2, "2023")


def run_cli(*argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
        try:
            code = cli.main(list(argv))
        except SystemExit as e:
            code = e.code
    return code, out.getvalue()


def write(path, text, year=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    if year:
        ts = __import__("datetime").datetime(year, 6, 1).timestamp()
        os.utime(path, (ts, ts))


class RecfileTest(unittest.TestCase):
    def test_roundtrip_multiline_and_repeated_fields(self):
        r = recfile.Record("Disc", [("Id", "X"), ("Note", "line one\nline two\n\nafter blank"), ("Note", "second")])
        parsed = recfile.parse(recfile.dumps(catalog.Catalog().records()[:1] + [r]))
        disc = [p for p in parsed if not p.is_descriptor][0]
        self.assertEqual(disc.type, "Disc")
        self.assertEqual(disc.get_all("Note"), ["line one\nline two\n\nafter blank", "second"])

    def test_records_follow_their_descriptor(self):
        c = catalog.Catalog()
        c.discs.append(recfile.Record("Disc", [("Id", "A"), ("Title", "t"), ("Date", "2026-01-01")]))
        c.events.append(catalog.new_event("A", "creation", "success", "test"))
        again = catalog.Catalog(recfile.parse(recfile.dumps(c.records())))
        self.assertEqual([d.get("Id") for d in again.discs], ["A"])
        self.assertEqual([e.get("Type") for e in again.events], ["creation"])

    @unittest.skipUnless(shutil.which("recfix"), "recutils not installed")
    def test_recfix_accepts_catalogue(self):
        c = catalog.Catalog()
        c.discs.append(recfile.Record("Disc", [("Id", "A"), ("Title", "t"), ("Date", "2026-01-01"), ("Files", "3")]))
        c.events.append(catalog.new_event("A", "fixity check", "success", "test"))
        with tempfile.NamedTemporaryFile("w", suffix=".rec", delete=False) as f:
            f.write(recfile.dumps(c.records()))
        try:
            subprocess.run(["recfix", f.name], check=True)
        finally:
            os.remove(f.name)


class BagTest(unittest.TestCase):
    def test_rejects_ambiguous_names(self):
        with tempfile.TemporaryDirectory() as d:
            write(os.path.join(d, "bad%25name.txt"), "x")
            with self.assertRaises(ValueError):
                bag.scan_payload(d, progress=False)

    def test_coverage(self):
        with tempfile.TemporaryDirectory() as d:
            write(os.path.join(d, "a"), "x", 2019)
            write(os.path.join(d, "b"), "x", 2021)
            self.assertEqual(catalog.coverage_years(bag.scan_payload(d, progress=False)), "2019/2021")


class DiscIdTest(unittest.TestCase):
    def test_compose_and_parse(self):
        disc_id = discid.compose("photos", 7, "2015/2024")
        self.assertTrue(disc_id.startswith("PHOTOS-07_2015-2024_"))
        self.assertEqual(disc_id[:16], "PHOTOS-07_2015-2")  # the Joliet label still identifies the disc
        p = discid.parse(disc_id)
        self.assertEqual((p["set"], p["sequence"], p["coverage"], p["valid"]), ("PHOTOS", 7, "2015-2024", True))

    def test_every_single_typo_and_neighbour_swap_is_caught(self):
        disc_id = discid.compose("PHOTOS", 7, "2015/2024")
        body = disc_id[:-2]
        for i, ch in enumerate(body):
            if ch in "-_":
                continue
            for r in discid.ALPHABET:
                if r != ch:
                    typo = disc_id[:i] + r + disc_id[i + 1:]
                    self.assertFalse((discid.parse(typo) or {"valid": False})["valid"], typo)
            if i + 1 < len(body) and body[i + 1] not in "-_" and body[i + 1] != ch:
                swap = disc_id[:i] + body[i + 1] + ch + disc_id[i + 2:]
                self.assertFalse((discid.parse(swap) or {"valid": False})["valid"], swap)

    def test_edtf_coverage(self):
        cases = {"2019": (2019, 2019), "2015/2024": (2015, 2024), "2019-07/2019-08": (2019, 2019),
                 "199X": (1990, 1999), "1995~": (1995, 1995), "[1998,1999]": (1998, 1999),
                 "2020-2025": (2020, 2025)}  # the last is a legacy year range
        for text, span in cases.items():
            self.assertEqual(discid.coverage_range(text), span, text)
        self.assertEqual(discid.compact("2019-07/2019-08"), "201907-201908")
        self.assertEqual(discid.to_edtf("2015-2024"), "2015/2024")
        with self.assertRaises(discid.IdError):
            discid.to_edtf("sometime")

    def test_legacy_ids_still_parse(self):
        p = discid.parse("2020-2025_PROJECTS_01")
        self.assertEqual((p["scheme"], p["set"], p["sequence"]), (discid.LEGACY_SCHEME, "PROJECTS", 1))

    def test_regenerate_from_fields_and_suggest(self):
        disc_id = discid.compose("TAXES", 3, "2019")
        record = recfile.Record("Disc", [("Id", disc_id), ("IdScheme", discid.SCHEME), ("Set", "TAXES"),
                                         ("Sequence", "3"), ("Coverage", "2019")])
        self.assertEqual(discid.regenerate(record), disc_id)
        typo = disc_id[:-1] + ("0" if disc_id[-1] != "0" else "1")
        self.assertEqual(discid.suggest(typo, [disc_id, discid.compose("TAXES", 4, "2020")]), [disc_id])


class SetsTest(unittest.TestCase):
    def test_vocabulary_is_a_dag_of_words(self):
        from archivetool import sets
        with tempfile.TemporaryDirectory() as d:
            vocab = sets.load(catalog.Home(d))
            self.assertTrue(os.path.exists(os.path.join(d, "sets.rec")))  # copied for editing
            self.assertEqual(vocab.paths("TRIP"), ["MEMORIES/PHOTO/TRIP"])
            self.assertEqual(vocab.paths("SCAN"), ["MEMORIES/PHOTO/SCAN", "RECORDS/SCAN"])  # two parents
            self.assertEqual(vocab.ancestors("SCAN"), {"MEMORIES", "PHOTO", "RECORDS"})
            for word, code in {"Photos": "PHOTO", "Taxes_2019": "TAXES", "scans": "SCAN", "Holiday": "TRIP",
                               "Emails": "EMAIL", "zzz": None}.items():
                self.assertEqual(vocab.guess(word), code, word)
            for code in vocab.entries:
                self.assertTrue(discid.SET_RE.match(code), code)  # every code fits in an id

    def test_cycles_and_unknown_parents_are_rejected(self):
        from archivetool import sets
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "v.rec")
            for body, message in (("Code: AA\nName: a\nParent: BB\n\nCode: BB\nName: b\nParent: AA\n", "cycle"),
                                  ("Code: AA\nName: a\nParent: ZZ\n", "unknown parent")):
                with open(path, "w", encoding="utf-8") as f:
                    f.write("%rec: Set\n\n" + body)
                with self.assertRaisesRegex(sets.VocabError, message):
                    sets.load(None, path)

    def test_date_ranges_to_the_day(self):
        self.assertTrue(discid.covers("2019-07-14/2019-07-20", "2019-07-15"))
        self.assertFalse(discid.covers("2019-07-14/2019-07-20", "2019-08"))
        self.assertTrue(discid.covers("2019/..", "2030"))
        self.assertEqual(discid.compose("TRIP", 1, "2019-12-24/2020-01-02")[:22], "TRIP-01_201912-202001_")


class MediaTest(unittest.TestCase):
    def test_budget_is_the_exact_redundancy_boundary(self):
        for name in media.MEDIA:
            for dm in (True, False):
                cap = media.capacity(name, dm)
                for pct in (10, 20, 33.3, 50):
                    budget = media.data_budget(cap, pct)
                    self.assertGreaterEqual(media.rs03_layout(budget, cap)[1], pct)
                    self.assertLess(media.rs03_layout(budget + cap // 255, cap)[1], pct)

    def test_bd25_budget(self):
        # 20% minimum redundancy leaves ~20 GB of data on a 25 GB BD-R
        self.assertAlmostEqual(media.data_budget(media.capacity("bd25"), 20) * 2048 / 1e9, 20.04, places=2)


@unittest.skipUnless(HAVE_IMAGE_TOOLS, "genisoimage and 7z required")
class SplitTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.home = os.path.join(self.tmp, "home")
        self.src = os.path.join(self.tmp, "Big")
        for d in "abc":
            for i in range(8):
                path = os.path.join(self.src, d, "f%d.bin" % i)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as f:
                    f.write(os.urandom(700_000))

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def make(self, *extra):
        return run_cli("--home", self.home, "make", "-y", "--no-ecc", "--set", "BIG", "--medium-sectors", "5000",
                       "--output-dir", os.path.join(self.tmp, "out"), self.src, *extra)

    def check_split(self, out):
        lines = [l.split("\t") for l in out.strip().splitlines()]
        self.assertGreater(len(lines), 1)
        budget = media.data_budget(5000, 20)
        seen = []
        for n, (disc_id, iso, _) in enumerate(lines, 1):
            self.assertLessEqual(os.path.getsize(iso) // 2048, budget)
            dest = os.path.join(self.tmp, "x", disc_id)
            subprocess.run(["7z", "x", "-o" + dest, iso], check=True, stdout=subprocess.DEVNULL)
            proc = subprocess.run([sys.executable, "-I", os.path.join(dest, "tools", "bagit.py"), "--validate", dest],
                                  capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            with open(os.path.join(dest, "bag-info.txt"), encoding="utf-8") as f:
                self.assertIn("Bag-Count: %d of %d" % (n, len(lines)), f.read())
            self.assertEqual(len(os.listdir(os.path.join(dest, "catalog", "manifests"))), len(lines))
            for root, _, names in os.walk(os.path.join(dest, "data")):
                seen += [os.path.relpath(os.path.join(root, x), os.path.join(dest, "data")) for x in names]
        self.assertEqual(sorted(seen), sorted(e.path for e in bag.scan_payload(self.src, progress=False)))

    def test_too_big_without_split(self):
        code, out = self.make()
        self.assertIn("Use --split", str(code))

    def test_split(self):
        code, out = self.make("--split")
        self.assertEqual(code, 0, out)
        self.check_split(out)

    def test_rebalances_when_estimate_is_low(self):
        original = make.estimate_sectors
        make.estimate_sectors = lambda e: 1  # pack everything onto one disc, then let fit() correct it
        try:
            code, out = self.make("--split")
        finally:
            make.estimate_sectors = original
        self.assertEqual(code, 0, out)
        self.check_split(out)


@unittest.skipUnless(HAVE_IMAGE_TOOLS, "genisoimage and 7z required")
class MakeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.home = os.path.join(self.tmp, "home")
        self.projects = os.path.join(self.tmp, "2025-01-13_Projects_2020_-_2025")
        write(os.path.join(self.projects, "readme.md"), "hello", 2020)
        write(os.path.join(self.projects, "sub dir", "100% ünïcode.txt"), "unicode", 2025)
        self.photos = os.path.join(self.tmp, "Photos")
        write(os.path.join(self.photos, "IMG_0001.JPG"), "jpeg", 2023)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def make(self, src, *extra):
        self.count = getattr(self, "count", 0) + 1
        iso = os.path.join(self.tmp, "disc%d.iso" % self.count)
        code, out = run_cli("--home", self.home, "make", "-y", "--no-ecc", "-o", iso, src, *extra)
        self.assertEqual(code, 0, out)
        disc_id, iso, _ = out.strip().split("\t")
        dest = os.path.join(self.tmp, "x-" + disc_id)
        subprocess.run(["7z", "x", "-o" + dest, iso], check=True, stdout=subprocess.DEVNULL)
        return disc_id, dest

    def validate(self, bag_dir):
        proc = subprocess.run([sys.executable, "-I", os.path.join(bag_dir, "tools", "bagit.py"),
                               "--validate", bag_dir], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_full_disc(self):
        disc_id, disc = self.make(self.projects, "--location", "Shelf A", "--note", "first")
        self.assertEqual(disc_id, PROJECTS_01)
        self.validate(disc)
        subprocess.run(["sha256sum", "-c", "--quiet", "manifest-sha256.txt"], cwd=disc, check=True)
        for name in ("index.html", "README.txt", "catalog.rec", "catalog/archive.rec",
                     "tools/bagit.py", "tools/bluray-archival-workflow/archivetool/cli.py"):
            self.assertTrue(os.path.exists(os.path.join(disc, name)), name)
        with open(os.path.join(disc, "index.html"), encoding="utf-8") as f:
            page = f.read()
        self.assertIn("100% ünïcode.txt", page)
        self.assertIn('href="data/sub%20dir/100%25%20%C3%BCn%C3%AFcode.txt"', page)
        self.assertNotIn("<script", page)
        records = recfile.read(os.path.join(disc, "catalog.rec"))
        on_disc = catalog.Catalog(records)
        self.assertEqual(on_disc.disc(disc_id).get("Location"), "Shelf A")
        self.assertEqual([e.get("Type") for e in on_disc.events], ["message digest calculation"])
        # Entry point for other tools: first real record says what this is and where things are
        archive = [r for r in records if r.type == "Archive" and not r.is_descriptor][0]
        self.assertEqual((archive.get("Format"), archive.get("Version")), ("smart-archive", "0.1"))
        self.assertEqual(archive.get("Uuid"), on_disc.disc(disc_id).get("Uuid"))
        self.assertEqual(len(archive.get("Uuid")), 36)
        for field in ("Manifest", "Listing", "Snapshot", "Viewer"):
            self.assertTrue(os.path.exists(os.path.join(disc, archive.get(field))), field)
        # Uuid is also in the home catalogue, so a re-read disc maps back to the same record
        self.assertEqual(catalog.Home(self.home).load().disc(disc_id).get("Uuid"), archive.get("Uuid"))

    def test_snapshot_scopes_and_find(self):
        self.make(self.projects)
        _, photos_set = self.make(self.photos, "--set", "PHOTOS", "--snapshot", "set")
        _, photos_full = self.make(self.photos, "--set", "PHOTOS")
        self.assertEqual(os.listdir(os.path.join(photos_set, "catalog", "manifests")), [PHOTOS_01 + ".sha256"])
        self.assertEqual(sorted(os.listdir(os.path.join(photos_full, "catalog", "manifests"))),
                         sorted([PROJECTS_01 + ".sha256", PHOTOS_01 + ".sha256", PHOTOS_02 + ".sha256"]))
        self.validate(photos_full)
        with open(os.path.join(photos_full, "index.html"), encoding="utf-8") as f:
            self.assertIn(PROJECTS_01, f.read())

        code, out = run_cli("--home", self.home, "find", "ünï")
        self.assertEqual(code, 0)
        self.assertIn(PROJECTS_01, out)
        code, out = run_cli("--home", self.home, "find", "*.jpg")
        self.assertEqual(out.count("IMG_0001.JPG"), 2)
        self.assertEqual(run_cli("--home", self.home, "find", "nothing-matches")[0], 1)

    def test_note_and_locate(self):
        disc_id, _ = self.make(self.photos, "--set", "PHOTOS")
        run_cli("--home", self.home, "note", disc_id, "multi\nline")
        run_cli("--home", self.home, "locate", disc_id, "Offsite")
        disc = catalog.Home(self.home).load().disc(disc_id)
        self.assertEqual(disc.get("Location"), "Offsite")
        self.assertEqual(disc.get_all("Note"), ["multi\nline"])

    def test_source_folder_untouched(self):
        before = sorted(os.listdir(self.photos))
        self.make(self.photos)
        self.assertEqual(sorted(os.listdir(self.photos)), before)

    def test_id_fields_commands_and_covers(self):
        disc_id, _ = self.make(self.projects, "--coverage", "2015-2024")  # legacy range input is accepted
        disc = catalog.Home(self.home).load().disc(disc_id)
        self.assertEqual((disc.get("IdScheme"), disc.get("Set"), disc.get("Sequence"), disc.get("Coverage")),
                         (discid.SCHEME, "PROJECTS", "1", "2015/2024"))
        self.assertEqual(discid.regenerate(disc), disc_id)
        code, out = run_cli("--home", self.home, "id", disc_id)
        self.assertEqual(code, 0)
        self.assertIn("regenerate this id", out)
        typo = disc_id[:-1] + ("0" if disc_id[-1] != "0" else "1")
        code, out = run_cli("--home", self.home, "id", typo)
        self.assertEqual(code, 1)
        self.assertIn("did you mean %s" % disc_id, out)
        self.assertIn(disc_id, run_cli("--home", self.home, "list", "--covers", "2019")[1])
        self.assertNotIn(disc_id, run_cli("--home", self.home, "list", "--covers", "2030")[1])
        photos_id, _ = self.make(self.photos, "--set", "PHOTOS")
        self.assertEqual(photos_id, PHOTOS_01)  # sequences are per set
        self.assertEqual(catalog.Home(self.home).load().next_number("PROJECTS"), 2)
        # one Set plus extra categories; every vocabulary path is recorded on the disc
        trip_id, disc = self.make(self.photos, "--set", "trip", "--category", "scan",
                                  "--coverage", "2023-06-01/2023-06-14")
        trip = catalog.Catalog(recfile.read(os.path.join(disc, "catalog.rec"))).disc(trip_id)
        self.assertEqual((trip.get("Set"), trip.get_all("Category")), ("TRIP", ["SCAN"]))
        self.assertEqual(trip.get_all("Path"), ["MEMORIES/PHOTO/TRIP", "MEMORIES/PHOTO/SCAN", "RECORDS/SCAN"])
        self.assertTrue(trip_id.startswith("TRIP-01_202306_"))
        out = run_cli("--home", self.home, "sets")[1]
        self.assertRegex(out, r"TRIP\s+Trips and holidays\s+1 disc")
        self.assertRegex(out, r"MEMORIES\s+Memories\s+0 discs \(1 including below\)")
        self.assertIn("also under RECORDS, PHOTO", out)
        self.assertIn("PROJECTS", out)
        self.assertIn(trip_id, run_cli("--home", self.home, "list", "--in", "records")[1])  # via its category
        self.assertNotIn(disc_id, run_cli("--home", self.home, "list", "--in", "memories")[1])
        self.assertIn(trip_id, run_cli("--home", self.home, "list", "--covers", "2023-06-10")[1])
        self.assertNotIn(trip_id, run_cli("--home", self.home, "list", "--covers", "2023-07")[1])

    def test_tools_snapshot_without_history_by_default(self):
        _, disc = self.make(self.photos, "--set", "PHOTOS")
        tools = os.listdir(os.path.join(disc, "tools"))
        self.assertIn("bluray-archival-workflow", tools)
        self.assertNotIn("bluray-archival-workflow.bundle", tools)
        with open(os.path.join(disc, "README.txt"), encoding="utf-8") as f:
            self.assertNotIn(".bundle", f.read())
        if os.path.isdir(os.path.join(REPO, ".git")):
            _, disc = self.make(self.photos, "--set", "PHOTOS", "--tools-history")
            self.assertIn("bluray-archival-workflow.bundle", os.listdir(os.path.join(disc, "tools")))

    def test_search_page_data(self):
        self.make(self.projects)
        disc_id, disc = self.make(self.photos, "--set", "PHOTOS")
        for name in ("search.html", "catalog/web/discs.js", "catalog/web/files/%s.js" % disc_id,
                     "catalog/web/files/%s.js" % PROJECTS_01, "catalog/listings/%s.tsv" % disc_id):
            self.assertTrue(os.path.exists(os.path.join(disc, name)), name)
        with open(os.path.join(disc, "catalog/web/files/%s.js" % PROJECTS_01), encoding="utf-8") as f:
            self.assertIn("100% ünïcode.txt", f.read())
        with open(os.path.join(disc, "search.html"), encoding="utf-8") as f:
            self.assertIn('<script src="catalog/web/discs.js">', f.read())

    def test_disc_scope_has_only_this_disc(self):
        self.make(self.projects)
        disc_id, disc = self.make(self.photos, "--set", "PHOTOS", "--snapshot", "disc")
        self.assertEqual(os.listdir(os.path.join(disc, "catalog", "manifests")), [disc_id + ".sha256"])
        self.validate(disc)

    def test_index_matches_scan(self):
        self.make(self.projects)
        self.make(self.photos, "--set", "PHOTOS")
        results = {}
        for pattern in ("ünï", "*.jpg", "readme", "photos"):
            results[pattern] = run_cli("--home", self.home, "find", pattern)
        self.assertEqual(run_cli("--home", self.home, "index")[0], 0)
        for pattern, expected in results.items():
            self.assertEqual(run_cli("--home", self.home, "find", pattern), expected, pattern)
        # index is refreshed automatically by make once it exists
        self.make(self.photos, "--set", "PHOTOS")
        self.assertEqual(run_cli("--home", self.home, "find", "*.jpg")[1].count("IMG_0001.JPG"), 2)

    def test_burned(self):
        disc_id, _ = self.make(self.photos, "--set", "PHOTOS")
        run_cli("--home", self.home, "burned", disc_id, "--copies", "2")
        run_cli("--home", self.home, "burned", disc_id, "--media-id", "VERBAT-IMk")
        cat = catalog.Home(self.home).load()
        self.assertEqual(cat.disc(disc_id).get("Copies"), "3")
        self.assertEqual(cat.disc(disc_id).get("MediaId"), "VERBAT-IMk")
        self.assertEqual([e.get("Type") for e in cat.events_for(disc_id)].count("replication"), 2)

    def test_rebuild_from_newest_disc(self):
        self.make(self.projects, "--location", "Shelf A")
        _, newest = self.make(self.photos, "--set", "PHOTOS")
        original = catalog.Home(self.home).load()
        fresh = os.path.join(self.tmp, "fresh-home")
        code, out = run_cli("--home", fresh, "rebuild", newest)
        self.assertEqual(code, 0, out)
        rebuilt = catalog.Home(fresh).load()
        self.assertEqual([d.get("Id") for d in rebuilt.discs], [d.get("Id") for d in original.discs])
        self.assertEqual(rebuilt.disc(PROJECTS_01).get("Location"), "Shelf A")
        self.assertEqual(run_cli("--home", fresh, "find", "ünï"), run_cli("--home", self.home, "find", "ünï"))
        # merging the same disc again changes nothing
        self.assertIn("Added 0 disc(s), updated 0, 0 new event(s), 0 file list(s)",
                      run_cli("--home", fresh, "rebuild", newest)[1])

    def test_ro_crate(self):
        disc_id, disc = self.make(self.photos, "--set", "PHOTOS", "--ro-crate", "--formats", "no",
                                  "--rights", "https://creativecommons.org/licenses/by/4.0/")
        self.validate(disc)
        with open(os.path.join(disc, "data", "ro-crate-metadata.json"), encoding="utf-8") as f:
            doc = json.load(f)
        graph = {e["@id"]: e for e in doc["@graph"]}
        self.assertEqual(graph["ro-crate-metadata.json"]["conformsTo"]["@id"], "https://w3id.org/ro/crate/1.2")
        self.assertEqual(graph["./"]["hasPart"], [{"@id": "IMG_0001.JPG"}])
        self.assertEqual(graph["./"]["temporalCoverage"], "2023")
        self.assertEqual(graph["#disc-id"]["value"], disc_id)
        self.assertEqual(os.listdir(self.photos), ["IMG_0001.JPG"])  # source untouched

    def test_ro_crate_refuses_to_overwrite(self):
        write(os.path.join(self.photos, "ro-crate-metadata.json"), "{}")
        code, _ = run_cli("--home", self.home, "make", "-y", "--no-ecc", "--ro-crate", "-o",
                          os.path.join(self.tmp, "x.iso"), self.photos)
        self.assertIn("overwrite", str(code))

    @unittest.skipUnless(shutil.which("sf"), "Siegfried (sf) not installed")
    def test_formats(self):
        write(os.path.join(self.photos, "doc.pdf"), "%PDF-1.4\n%%EOF\n")
        extra = ["--sf-home", os.environ["SF_HOME"]] if os.environ.get("SF_HOME") else []
        disc_id, disc = self.make(self.photos, "--set", "PHOTOS", "--formats", "yes", *extra)
        from archivetool import formats
        rows = formats.read(os.path.join(disc, "catalog", "formats", disc_id + ".csv"))
        self.assertEqual(rows["doc.pdf"]["puid"], "fmt/18")
        events = catalog.Home(self.home).load().events_for(disc_id)
        self.assertIn("format identification", [e.get("Type") for e in events])

    @unittest.skipUnless(os.environ.get("ARCHIVE_TEST_ECC") and shutil.which("dvdisaster"), "set ARCHIVE_TEST_ECC=1")
    def test_ecc(self):
        iso = os.path.join(self.tmp, "ecc.iso")
        code, out = run_cli("--home", self.home, "make", "-y", "--medium-sectors", "20000", "-o", iso, self.photos)
        self.assertEqual(code, 0, out)
        if image.dvdisaster_sets_medium_size():  # speed47: RS03 fills exactly the requested medium
            self.assertEqual(os.path.getsize(iso), 20000 // 255 * 255 * 2048)
        events = catalog.Home(self.home).load().events
        self.assertIn(("fixity check", "success"), [(e.get("Type"), e.get("Outcome")) for e in events])
        code, out = run_cli("--home", self.home, "check", "--image", iso)  # disc id read from the volume label
        self.assertEqual(code, 0, out)
        self.assertEqual(len([e for e in catalog.Home(self.home).load().events if e.get("Type") == "fixity check"]), 2)


class FakeLLM:
    """Minimal OpenAI-compatible server returning a canned reply; records the requests."""

    def __init__(self, reply):
        import threading
        from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
        fake = self
        self.reply, self.requests = reply, []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _json(self, obj):
                data = json.dumps(obj).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                self._json({"data": [{"id": "fake-model"}]})

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                fake.requests.append(body)
                if self.path.endswith("/embeddings"):
                    return self._json({"data": [{"index": i, "embedding": fake.embed(t)}
                                                for i, t in enumerate(body["input"])]})
                reply = fake.reply(body) if callable(fake.reply) else fake.reply
                content = reply if isinstance(reply, str) else json.dumps(reply)
                self._json({"choices": [{"message": {"role": "assistant", "content": content}}]})

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = "http://127.0.0.1:%d/v1" % self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    @staticmethod
    def embed(text):
        text = text.lower()
        return [3.0 if "cat" in text else 0.0, 3.0 if "code" in text or "python" in text else 0.0, 0.3]

    def close(self):
        self.server.shutdown()
        self.server.server_close()


REPLY = {"title": "Family trip photos 2019", "description": "Photos from a 2019 trip, one folder per day.",
         "subjects": ["Travel", "family", "travel"], "questions": ["Where was the 2019 trip?", "Who took the photos?"],
         "folder_tags": {"photos/2019 trip": ["travel", "2019"], "no/such/folder": ["x"]}}


class LLMTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.src = os.path.join(self.tmp, "Trip")
        write(os.path.join(self.src, "photos", "2019 trip", "IMG_0001.JPG"), "jpeg", 2019)
        write(os.path.join(self.src, "README.txt"), "Pictures from our holiday.", 2019)
        self.fake = FakeLLM(REPLY)

    def tearDown(self):
        self.fake.close()
        shutil.rmtree(self.tmp)

    def test_suggest_parses_and_filters(self):
        from archivetool import describe, llm
        entries = describe.folder_entries(self.src)
        inv = llm.inventory(entries, "Trip", text_root=self.src)
        self.assertIn("Pictures from our holiday.", inv)  # README text is included
        self.assertIn("photos/2019 trip/", inv)
        result = llm.suggest(llm.Client(self.fake.url), inv, folders=llm.folders_of(entries))
        self.assertEqual(result["subjects"], ["travel", "family"])  # lowercased, de-duplicated
        self.assertEqual(result["folder_tags"], {"photos/2019 trip": ["travel", "2019"]})  # unknown folder dropped
        self.assertEqual(self.fake.requests[0]["model"], "fake-model")  # picked from /models

    def test_fenced_json_and_bad_json(self):
        from archivetool import llm
        self.fake.reply = "Sure!\n```json\n" + json.dumps(REPLY) + "\n```"
        self.assertEqual(llm.suggest(llm.Client(self.fake.url), "inv")["title"], "Family trip photos 2019")
        self.fake.reply = "I cannot help with that."
        with self.assertRaises(llm.LLMError):
            llm.suggest(llm.Client(self.fake.url), "inv")

    def test_refuses_remote_server(self):
        from archivetool import llm
        with self.assertRaises(llm.LLMError):
            llm.Client("http://203.0.113.5:11434/v1")
        llm.Client("http://203.0.113.5:11434/v1", allow_remote=True)  # explicit opt-in

    def test_describe_save_then_make_with_draft(self):
        home = os.path.join(self.tmp, "home")
        draft = os.path.join(self.tmp, "draft.json")
        code, _ = run_cli("--home", home, "describe", self.src, "--llm-url", self.fake.url, "--save", draft)
        self.assertEqual(code, 0)
        with open(draft, encoding="utf-8") as f:
            self.assertEqual(json.load(f)["agent"], "llm:fake-model")
        if not HAVE_IMAGE_TOOLS:
            return
        code, out = run_cli("--home", home, "make", "-y", "--no-ecc", "--draft", draft,
                            "-o", os.path.join(self.tmp, "t.iso"), self.src)
        self.assertEqual(code, 0, out)
        cat = catalog.Home(home).load()
        disc = cat.discs[0]
        self.assertEqual(disc.get("Title"), "Family trip photos 2019")
        self.assertEqual(disc.get_all("Subject"), ["travel", "family"])
        self.assertIn("metadata modification", [e.get("Type") for e in cat.events])
        code, out = run_cli("--home", home, "find", "travel")
        self.assertIn("TAG   %s" % disc.get("Id"), out)
        self.assertIn("data/photos/2019 trip/", out)

    def test_interactive_questions_refine_and_review(self):
        from archivetool import describe, llm
        client = llm.Client(self.fake.url)
        entries = describe.folder_entries(self.src)
        replies = iter(["Kyoto, Japan", "",   # round 1: answer the first question, skip the second
                        "", "", "", "n"])      # review: accept title, description, subjects; drop tags
        original = describe.ask
        describe.ask = lambda prompt: next(replies)
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                suggestion, answers = describe.conversation(client, "inv", llm.folders_of(entries), rounds=1)
                draft = describe.review(suggestion, answers)
        finally:
            describe.ask = original
        self.assertEqual(answers, [("Where was the 2019 trip?", "Kyoto, Japan")])
        # the refine request carried the owner's answer
        self.assertIn("Kyoto, Japan", self.fake.requests[-1]["messages"][-1]["content"])
        self.assertEqual(draft["notes"], ["Q: Where was the 2019 trip?\nA: Kyoto, Japan"])
        self.assertEqual(draft["folder_tags"], {})
        self.assertEqual(draft["title"], "Family trip photos 2019")

    @unittest.skipUnless(HAVE_IMAGE_TOOLS, "genisoimage and 7z required")
    def test_apply_draft_to_existing_disc(self):
        home = os.path.join(self.tmp, "home")
        code, out = run_cli("--home", home, "make", "-y", "--no-ecc", "--title", "Old title",
                            "-o", os.path.join(self.tmp, "t.iso"), self.src)
        self.assertEqual(code, 0, out)
        disc_id = out.split("\t")[0]
        draft = os.path.join(self.tmp, "d.json")
        with open(draft, "w", encoding="utf-8") as f:
            json.dump({"title": "New title", "subjects": ["travel"], "notes": ["Q: Who?\nA: Us"],
                       "folder_tags": {"photos": ["travel"]}, "agent": "llm:test"}, f)
        code, out = run_cli("--home", home, "describe", disc_id, "--apply", draft)
        self.assertEqual(code, 0)
        cat = catalog.Home(home).load()
        disc = cat.disc(disc_id)
        self.assertEqual(disc.get("Title"), "New title")
        self.assertEqual(disc.get_all("Subject"), ["travel"])
        self.assertEqual(disc.get_all("Note"), ["Q: Who?\nA: Us"])
        event = [e for e in cat.events if e.get("Type") == "metadata modification"][-1]
        self.assertEqual(event.get("Agent"), "llm:test + owner review")
        self.assertIn("TAG", run_cli("--home", home, "find", "travel")[1])

    def test_make_llm_requires_terminal(self):
        code, _ = run_cli("--home", os.path.join(self.tmp, "home"), "make", "-y", "--no-ecc", "--llm",
                          "--llm-url", self.fake.url, self.src)
        self.assertIn("interactive", str(code))


# 1x1 PNG, padded past the 1 KiB "too small to be a photo" cut-off with a text chunk
PNG = (b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x02\x00\x00\x00\x90wS\xde"
       b"\x00\x00\x04\x00tEXt" + b"c" * 1024 + b"\x00\x00\x00\x00"
       b"\x00\x00\x00\x0cIDATx\x9cc\xf8\xff\xff?\x00\x05\xfe\x02\xfe\xa7\x9a\x8a\x10"
       b"\x00\x00\x00\x00IEND\xaeB`\x82")


class VisionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.src = os.path.join(self.tmp, "Trip")
        for i in range(5):
            path = os.path.join(self.src, "photos", "beach day", "IMG_%d.png" % i)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(PNG)

        def reply(body):
            content = body["messages"][-1]["content"]
            if isinstance(content, list):  # an image request
                self.images.append(content[1]["image_url"]["url"][:22])
                return "Caption: People on a sandy beach.\nTags: beach, sea, Beach"
            self.texts.append(content)
            return dict(REPLY, folder_tags={})
        self.images, self.texts = [], []
        self.fake = FakeLLM(reply)

    def tearDown(self):
        self.fake.close()
        shutil.rmtree(self.tmp)

    def test_sampling(self):
        from archivetool import describe, vision
        picked = vision.sample(describe.folder_entries(self.src), per_folder=3)
        self.assertEqual([e.path.rsplit("/", 1)[1] for e in picked["photos/beach day"]],
                         ["IMG_0.png", "IMG_1.png", "IMG_3.png"])  # evenly spaced

    def test_vision_is_local_only(self):
        from archivetool import llm, vision
        with self.assertRaises(llm.LLMError):
            vision.VisionClient("http://203.0.113.5:11434/v1")

    def test_parse_image_replies(self):
        from archivetool import vision
        self.assertEqual(vision.parse_image_reply("Caption: A dog on grass.\nTags: Dog, grass, dog, park."),
                         {"caption": "A dog on grass.", "tags": ["dog", "grass", "park"]})
        self.assertEqual(vision.parse_image_reply('{"caption": "A cat.", "tags": "cat; pet"}'),
                         {"caption": "A cat.", "tags": ["cat", "pet"]})
        self.assertEqual(vision.parse_image_reply("A dog on grass."), {"caption": "A dog on grass.", "tags": []})
        empty = {"caption": "", "tags": []}
        self.assertEqual(vision.parse_image_reply("{}"), empty)
        self.assertEqual(vision.parse_image_reply("Caption: one short sentence\nTags: 3 to 6 short lowercase tags"), empty)

    def test_describe_with_vision(self):
        draft = os.path.join(self.tmp, "d.json")
        code, _ = run_cli("describe", self.src, "--llm-url", self.fake.url, "--vision", "--vision-per-folder", "2",
                          "--save", draft)
        self.assertEqual(code, 0)
        self.assertEqual(self.images, ["data:image/png;base64,"] * 2)
        self.assertIn("People on a sandy beach.", self.texts[-1])  # the text model saw the captions
        with open(draft, encoding="utf-8") as f:
            d = json.load(f)
        self.assertEqual(d["folder_tags"]["photos/beach day"], ["beach", "sea"])
        self.assertIn("sandy beach", d["folder_captions"]["photos/beach day"])

    @unittest.skipUnless(HAVE_IMAGE_TOOLS, "genisoimage and 7z required")
    def test_captions_on_disc_and_searchable(self):
        home = os.path.join(self.tmp, "home")
        draft = os.path.join(self.tmp, "d.json")
        run_cli("describe", self.src, "--llm-url", self.fake.url, "--vision", "--save", draft)
        code, out = run_cli("--home", home, "make", "-y", "--no-ecc", "--draft", draft, "-o",
                            os.path.join(self.tmp, "t.iso"), self.src)
        self.assertEqual(code, 0, out)
        disc_id = out.split("\t")[0]
        info = catalog.read_tag_info(catalog.Home(home).disc_file("tags", disc_id))
        self.assertEqual(info["photos/beach day"][0], ["beach", "sea"])
        self.assertIn("sand", run_cli("--home", home, "find", "sandy")[1])  # caption search


FAKE_EMBEDDER = r"""#!/usr/bin/env python3
# Stand-in for llama.cpp's llama-embedding: bag-of-words vectors, same CLI and output format.
import hashlib, json, math, re, sys
args = sys.argv[1:]
text = open(args[args.index("-f") + 1], encoding="utf-8").read()
sep = args[args.index("--embd-separator") + 1]
out = []
for chunk in text.split(sep):
    v = [0.0] * 64
    for w in re.findall(r"[a-z]+", chunk.lower().replace("represent this sentence for searching relevant passages", "")):
        v[int(hashlib.md5(w.encode()).hexdigest(), 16) % 64] += 1.0
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    out.append([x / n for x in v])
print("log noise before the vectors")
print(json.dumps(out))
"""


class TagTest(unittest.TestCase):
    def setUp(self):
        from archivetool import models
        self.tmp = tempfile.mkdtemp()
        self.home = os.path.join(self.tmp, "home")
        self.binary = os.path.join(self.tmp, "llama-embedding")
        with open(self.binary, "w") as f:
            f.write(FAKE_EMBEDDER.replace("#!/usr/bin/env python3", "#!" + sys.executable))
        os.chmod(self.binary, 0o755)
        # a stand-in model file registered under a test name, so no download is needed
        self.model_file = os.path.join(self.tmp, "fake.gguf")
        with open(self.model_file, "wb") as f:
            f.write(b"fake model")
        models.MODELS["test-model"] = dict(models.MODELS[models.DEFAULT_EMBEDDING], file="fake.gguf",
                                           sha256=models.sha256_of(self.model_file), size=10)
        self.vocab = os.path.join(self.tmp, "tags.rec")
        with open(self.vocab, "w", encoding="utf-8") as f:
            f.write("%rec: Tag\n\nName: pets\nDescription: cats dogs kittens puppies\n\n"
                    "Name: code\nDescription: source code python programming scripts\n\n"
                    "Name: travel\nDescription: trip holiday sightseeing beach\n")
        self.src = os.path.join(self.tmp, "Stuff")
        write(os.path.join(self.src, "cats and kittens", "IMG_0001.jpg"), "x", 2020)
        write(os.path.join(self.src, "scripts", "python code.py"), "x", 2020)
        write(os.path.join(self.src, "beach trip", "DSC_0001.JPG"), "x", 2020)

    def tearDown(self):
        from archivetool import models
        models.MODELS.pop("test-model", None)
        shutil.rmtree(self.tmp)

    def tagger(self):
        from archivetool import catalog, models, tagger
        home = catalog.Home(self.home)
        models.install_model_file(home, "test-model", self.model_file)
        return tagger.Tagger(home, self.binary, model_name="test-model", vocab_file=self.vocab)

    def test_suggest_and_camera_names_ignored(self):
        from archivetool import describe, tagger
        texts = tagger.summaries(describe.folder_entries(self.src), self.src)
        self.assertNotIn("DSC", texts["beach trip"])
        got = self.tagger().suggest(texts, top=1)
        self.assertEqual({f: t[0][0] for f, t in got.items()},
                         {"cats and kittens": "pets", "scripts": "code", "beach trip": "travel"})

    def test_learns_from_reviews(self):
        from archivetool import bag, tagger
        t = self.tagger()
        seen = tagger.summaries([bag.Entry("kyoto temples garden/a.jpg", 1, 0)])
        t.remember(seen, {"kyoto temples garden": ["japan"]})
        again = tagger.summaries([bag.Entry("kyoto temples garden/b.jpg", 1, 0),
                                  bag.Entry("scripts/x.py", 1, 0)])
        got = t.suggest(again, top=2)
        self.assertIn("japan", [x for x, _ in got["kyoto temples garden"]])
        self.assertNotIn("japan", [x for x, _ in got["scripts"]])

    def test_model_checksum_is_enforced(self):
        from archivetool import catalog, models
        bad = os.path.join(self.tmp, "bad.gguf")
        with open(bad, "wb") as f:
            f.write(b"something else")
        with self.assertRaises(models.ModelError):
            models.install_model_file(catalog.Home(self.home), "test-model", bad)

    def test_cli_save_draft_and_apply_to_disc(self):
        from archivetool import catalog, models
        models.install_model_file(catalog.Home(self.home), "test-model", self.model_file)
        opts = ["--llama-embedding", self.binary, "--model", "test-model", "--vocab", self.vocab]
        draft = os.path.join(self.tmp, "d.json")
        code, _ = run_cli("--home", self.home, "tag", self.src, "--save", draft, *opts)
        self.assertEqual(code, 0)
        with open(draft, encoding="utf-8") as f:
            d = json.load(f)
        self.assertEqual(d["folder_tags"]["scripts"][0], "code")
        self.assertIn("(unreviewed)", d["agent"])
        if not HAVE_IMAGE_TOOLS:
            return
        code, out = run_cli("--home", self.home, "make", "-y", "--no-ecc", "--draft", draft,
                            "-o", os.path.join(self.tmp, "t.iso"), self.src)
        self.assertEqual(code, 0, out)
        disc_id = out.split("\t")[0]
        self.assertIn("TAG", run_cli("--home", self.home, "find", "pets")[1])
        # re-tag the existing disc from its catalogue listing and apply
        code, _ = run_cli("--home", self.home, "tag", disc_id, "--apply", *opts)
        self.assertEqual(code, 0)
        events = catalog.Home(self.home).load().events_for(disc_id)
        self.assertIn("embeddings:test-model (unreviewed)", [e.get("Agent") for e in events])

    def test_embeddings_api_engine(self):
        from archivetool import catalog, tagger
        import math
        fake = FakeLLM({})
        try:
            t = tagger.Tagger(catalog.Home(self.home), vocab_file=self.vocab, embed_url=fake.url)
            self.assertEqual(t.agent, "embeddings:fake-model")  # examples are kept per model
            got = t.suggest({"a": "cats kittens", "b": "python code"}, top=1)
            self.assertEqual((got["a"][0][0], got["b"][0][0]), ("pets", "code"))
            self.assertAlmostEqual(math.sqrt(sum(x * x for x in t.embed(["cat"])[0])), 1.0)  # normalised
        finally:
            fake.close()


class GuiTest(unittest.TestCase):
    def setUp(self):
        import threading
        from archivetool import gui
        self.tmp = tempfile.mkdtemp()
        self.home = os.path.join(self.tmp, "home")
        self.server, self.url = gui.serve(self.home, 0, open_browser=False)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.base = self.url.split("?")[0].rstrip("/")
        self.token = self.server.app.token

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        shutil.rmtree(self.tmp)

    def request(self, path, body=None, token=True, host=None):
        import urllib.error
        import urllib.request
        headers = {"X-Archive-Token": self.token} if token else {}
        if host:
            headers["Host"] = host
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        try:
            with urllib.request.urlopen(urllib.request.Request(self.base + path, data=data, headers=headers)) as r:
                return r.status, r.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()

    def wait(self, job):
        import time
        for _ in range(600):
            status, body = self.request("/api/job?id=%s" % job["id"])
            j = json.loads(body)
            if j["done"]:
                return j
            time.sleep(0.2)
        self.fail("job did not finish")

    def test_access_control(self):
        self.assertEqual(self.request("/api/discs", token=False)[0], 403)
        self.assertEqual(self.request("/?t=wrong", token=False)[0], 403)
        self.assertEqual(self.request("/api/discs", host="evil.example:80")[0], 403)
        status, page = self.request("/?t=" + self.token, token=False)
        self.assertEqual(status, 200)
        self.assertIn(self.token, page)

    @unittest.skipUnless(HAVE_IMAGE_TOOLS, "genisoimage and 7z required")
    def test_make_note_find(self):
        src = os.path.join(self.tmp, "Photos")
        write(os.path.join(src, "IMG_0001.JPG"), "jpeg", 2023)
        status, body = self.request("/api/make", {"source": src, "set": "PHOTOS", "medium": "auto", "no_ecc": True,
                                                  "output_dir": os.path.join(self.tmp, "out"), "note": ["hello"]})
        self.assertEqual(status, 200, body)
        result = self.wait(json.loads(body))
        self.assertEqual(result["returncode"], 0, "\n".join(result["lines"]))
        disc = json.loads(self.request("/api/discs")[1])["discs"][0]
        self.assertEqual(disc["Note"], ["hello"])
        job = json.loads(self.request("/api/command", {"command": "note", "disc_id": disc["Id"], "text": "second"})[1])
        self.assertEqual(self.wait(job)["returncode"], 0)
        found = json.loads(self.request("/api/find?q=img_0001")[1])
        self.assertEqual(found["total"], 1)
        self.assertEqual(json.loads(self.request("/api/discs")[1])["discs"][0]["Note"], ["hello", "second"])

    def test_bad_requests(self):
        self.assertEqual(self.request("/api/command", {"command": "rm -rf"})[0], 400)
        self.assertEqual(self.request("/api/job?id=999")[0], 404)


if __name__ == "__main__":
    unittest.main()
