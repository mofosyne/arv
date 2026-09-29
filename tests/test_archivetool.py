"""Tests for the archive CLI. Run: python3 -m unittest discover -s tests

Image tests need genisoimage and 7z; the ECC test additionally needs dvdisaster
and only runs with ARCHIVE_TEST_ECC=1 (it takes several minutes).
"""

import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, REPO)

from archivetool import bag, catalog, cli, recfile  # noqa: E402

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
        self.assertEqual(recfile.read(os.path.join(disc, "catalog.rec"))[2].get("Location"), "Shelf A")

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

    @unittest.skipUnless(os.environ.get("ARCHIVE_TEST_ECC") and shutil.which("dvdisaster"), "set ARCHIVE_TEST_ECC=1")
    def test_ecc(self):
        code, out = run_cli("--home", self.home, "make", "-y", "-o", os.path.join(self.tmp, "ecc.iso"), self.photos)
        self.assertEqual(code, 0, out)
        events = catalog.Home(self.home).load().events
        self.assertIn(("fixity check", "success"), [(e.get("Type"), e.get("Outcome")) for e in events])


if __name__ == "__main__":
    unittest.main()
