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
            self.assertEqual(catalog.coverage_years(bag.scan_payload(d, progress=False)), "2019-2021")


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
        self.assertEqual(disc_id, "2020-2025_PROJECTS_01")
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
        on_disc = catalog.Catalog(recfile.read(os.path.join(disc, "catalog.rec")))
        self.assertEqual(on_disc.disc(disc_id).get("Location"), "Shelf A")
        self.assertEqual([e.get("Type") for e in on_disc.events], ["message digest calculation"])

    def test_snapshot_scopes_and_find(self):
        self.make(self.projects)
        _, photos_set = self.make(self.photos, "--set", "PHOTOS", "--snapshot", "set")
        _, photos_full = self.make(self.photos, "--set", "PHOTOS")
        self.assertEqual(os.listdir(os.path.join(photos_set, "catalog", "manifests")), ["2023_PHOTOS_01.sha256"])
        self.assertEqual(sorted(os.listdir(os.path.join(photos_full, "catalog", "manifests"))),
                         ["2020-2025_PROJECTS_01.sha256", "2023_PHOTOS_01.sha256", "2023_PHOTOS_02.sha256"])
        self.validate(photos_full)
        with open(os.path.join(photos_full, "index.html"), encoding="utf-8") as f:
            self.assertIn("2020-2025_PROJECTS_01", f.read())

        code, out = run_cli("--home", self.home, "find", "ünï")
        self.assertEqual(code, 0)
        self.assertIn("2020-2025_PROJECTS_01", out)
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

    def test_search_page_data(self):
        self.make(self.projects)
        disc_id, disc = self.make(self.photos, "--set", "PHOTOS")
        for name in ("search.html", "catalog/web/discs.js", "catalog/web/files/%s.js" % disc_id,
                     "catalog/web/files/2020-2025_PROJECTS_01.js", "catalog/listings/%s.tsv" % disc_id):
            self.assertTrue(os.path.exists(os.path.join(disc, name)), name)
        with open(os.path.join(disc, "catalog/web/files/2020-2025_PROJECTS_01.js"), encoding="utf-8") as f:
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
        self.assertEqual(rebuilt.disc("2020-2025_PROJECTS_01").get("Location"), "Shelf A")
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


if __name__ == "__main__":
    unittest.main()
