"""Tests for arv-gui (src/arv-gui), the web API that runs arv (and arv-assist) for every action.

Run: python3 -m unittest discover -s tests (make check runs it after the C checks, which build what
it needs: src/arv/build/arv, src/arv-assist/build/arv-assist and its fake model server).
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "src", "arv-gui"))

from arvgui import gui  # noqa: E402

os.environ["ARV_APE"] = "none"   # the small test media have no room for tools/arv.com
ARV = os.path.join(REPO, "src", "arv", "build", "arv")
FAKE_LLM = os.path.join(REPO, "src", "arv-assist", "build", "fake-llm")
HAVE_ARV = os.path.exists(ARV) and shutil.which("7z") is not None


def write(path, text, year=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    if year:
        ts = __import__("datetime").datetime(year, 6, 1).timestamp()
        os.utime(path, (ts, ts))



class IconTest(unittest.TestCase):
    def test_each_extension_has_one_family(self):
        import re
        with open(os.path.join(REPO, "src", "arv-gui", "arvgui", "gui.html"), encoding="utf-8") as f:
            page = f.read()
        table = re.search(r"var FAMILIES = \[(.*?)\n  \];", page, re.S).group(1)
        rows = re.findall(r'\["([^"]+)", "([^"]+)", "([^"]+)"\]', table)
        self.assertGreaterEqual(len(rows), 8)
        seen = {}
        for _, family, exts in rows:
            for x in exts.split():
                self.assertEqual(x, x.lower(), x)
                self.assertNotIn(x, seen, "%s is both %s and %s" % (x, seen.get(x), family))
                seen[x] = family


class GuiTest(unittest.TestCase):
    """The server only runs arv (and arv-assist) for the page: these drive it as the page does."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.home = os.path.join(self.tmp, "home")
        self.fake = None
        self.server = None

    def start(self, llm_options=None):
        self.server, self.url = gui.serve(self.home, 0, open_browser=False, llm_options=llm_options)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.base = self.url.split("?")[0].rstrip("/")
        self.token = self.server.app.token

    def tearDown(self):
        if self.server:
            self.server.shutdown()
            self.server.server_close()
        if self.fake:
            self.fake.kill()
            self.fake.wait()
        shutil.rmtree(self.tmp)

    def request(self, path, body=None, token=True, host=None):
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

    def run_arv(self, *argv, stdin=None, program="arv", home=True):
        """/api/run, as the page calls it: {returncode, out, err}"""
        body = {"argv": (["--home", self.home] if home else []) + list(argv), "program": program}
        if stdin is not None:
            body["stdin"] = stdin
        status, text = self.request("/api/run", body)
        self.assertEqual(status, 200, text)
        return json.loads(text)

    def ok(self, *argv, stdin=None):
        """arv run through the server, which must succeed: what it printed (its JSON when it is JSON)"""
        r = self.run_arv(*argv, stdin=stdin)
        self.assertEqual(r["returncode"], 0, r["out"] + r["err"])
        return json.loads(r["out"]) if "--json" in argv else r["out"] + r["err"]

    def job(self, *argv, stdin=None):
        """a job (make, check, burned ...), followed to its end"""
        body = {"argv": ["--home", self.home] + list(argv), "job": True}
        if stdin is not None:
            body["stdin"] = stdin
        status, text = self.request("/api/run", body)
        self.assertEqual(status, 200, text)
        job = json.loads(text)
        for _ in range(600):
            j = json.loads(self.request("/api/job?id=%s" % job["id"])[1])
            if j["done"]:
                self.assertEqual(j["returncode"], 0, "\n".join(j["lines"]))
                return j
            time.sleep(0.2)
        self.fail("job did not finish")

    def make_disc(self, name="Trip", **extra):
        src, out = os.path.join(self.tmp, name), os.path.join(self.tmp, "out-" + name)
        write(os.path.join(src, "a.txt"), "a", 2019)
        self.job("make", "-y", src, "--set", "PHOTOS", "--medium", "auto", "--no-ecc", "--output-dir", out)
        disc = self.ok("list", "--json")["discs"][-1]["Id"]
        return disc, os.path.join(out, [f for f in os.listdir(out) if f.endswith(".iso")][0])

    def test_access_control(self):
        self.start()
        self.assertEqual(self.request("/api/setup", token=False)[0], 403)
        self.assertEqual(self.request("/?t=wrong", token=False)[0], 403)
        self.assertEqual(self.request("/api/setup", host="evil.example:80")[0], 403)
        status, page = self.request("/?t=" + self.token, token=False)
        self.assertEqual(status, 200)
        self.assertIn(self.token, page)

    def test_bad_requests(self):
        """Only a command word comes first, never an option or arv gui; and only arv and arv-assist run."""
        self.start()
        for body in ({"argv": ["--yes"]}, {"argv": ["--home", self.home, "-v"]}, {"argv": ["gui"]},
                     {"argv": []}, {"argv": "list"}, {"argv": ["list", 3]}, {"argv": ["list"], "program": "sh"},
                     {"argv": ["list"], "stdin": 5}, ["list"]):
            self.assertEqual(self.request("/api/run", body)[0], 400, body)
        self.assertEqual(self.request("/api/job?id=999")[0], 404)
        self.assertEqual(self.request("/api/discs")[0], 404)       # the server has no features of its own

    @unittest.skipUnless(HAVE_ARV, "src/arv/build/arv (make) and 7z required")
    def test_start_does_not_touch_legacy_files(self):
        os.makedirs(self.home)
        legacy = os.path.join(self.home, "archive.sqlite")
        write(legacy, "preserve this file")
        self.start()
        with open(legacy, encoding="utf-8") as f:
            self.assertEqual(f.read(), "preserve this file")

    @unittest.skipUnless(HAVE_ARV, "src/arv/build/arv (make) and 7z required")
    def test_first_run_makes_an_archive(self):
        """No archive here or above and no default: arv where finds none, and the page makes one with arv init."""
        where = os.path.join(self.tmp, "nas")
        os.makedirs(where)
        old = os.getcwd(), {k: os.environ.get(k) for k in ("HOME", "XDG_CONFIG_HOME", "ARV_HOME")}
        os.environ["HOME"] = os.environ["XDG_CONFIG_HOME"] = self.tmp
        os.environ.pop("ARV_HOME", None)
        os.chdir(where)
        try:
            self.home = None
            self.start()
            self.assertEqual(json.loads(self.request("/api/setup")[1]), {"home": None, "cwd": where})
            self.assertNotEqual(self.run_arv("where", "--json", home=False)["returncode"], 0)
            made = self.run_arv("init", "~/nas", "--name", "family", "--default", home=False)   # ~ as in a shell
            self.assertEqual(made["returncode"], 0, made["out"] + made["err"])
            self.assertTrue(os.path.isdir(os.path.join(where, ".arv")))
            found = self.run_arv("where", "--json", home=False)
            self.assertEqual(json.loads(found["out"])["path"], os.path.join(where, ".arv"))
        finally:
            os.chdir(old[0])
            for k, v in old[1].items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v

    @unittest.skipUnless(HAVE_ARV, "src/arv/build/arv (make) and 7z required")
    def test_collections(self):
        self.start()
        fam, out = os.path.join(self.tmp, "fam"), os.path.join(self.tmp, "out")
        write(os.path.join(fam, "a.txt"), "a", 2019)
        self.ok("collection", "init", fam, "--code", "FAM", "--title", "Family", "--set", "PHOTO")
        for n in (1, 2):
            if n == 2:
                write(os.path.join(fam, "b.txt"), "b", 2019)
            self.job("make", fam, "-y", "--no-ecc", "--no-formats", "--output-dir", os.path.join(out, str(n)))
            image = [f for f in os.listdir(os.path.join(out, str(n))) if f.endswith(".iso")][0]
            self.job("burned", "--device", os.path.join(out, str(n), image))
        c = self.ok("collection", "list", "--json")["collections"][0]
        self.assertEqual((c["code"], [(e["edition"], e["safe"], e["replaced"], e["retired"]) for e in c["editions"]]),
                         ("FAM", [(1, True, True, False), (2, True, False, False)]))
        self.assertIn("Nothing recorded", self.ok("retire", "FAM"))       # the preview
        self.assertTrue(self.ok("collection", "list", "--json")["collections"][0]["editions"][0]["replaced"])
        self.ok("retire", "FAM", "--yes")
        c = self.ok("collection", "list", "--json")["collections"][0]
        self.assertEqual([(e["edition"], e["retired"]) for e in c["editions"]], [(1, True), (2, False)])
        self.ok("collection", "keep", "FAM", "2")
        self.assertTrue(self.ok("collection", "list", "--json")["collections"][0]["editions"][1]["kept"])

    @unittest.skipUnless(HAVE_ARV, "src/arv/build/arv (make) and 7z required")
    def test_make_note_find_describe(self):
        """A disc made with a draft on standard input, a note, found again, and described from a draft."""
        self.start()
        src = os.path.join(self.tmp, "Photos")
        write(os.path.join(src, "IMG_0001.JPG"), "jpeg", 2023)
        draft = json.dumps({"tags": {"IMG_0001.JPG": ["best"]}, "authorship": "human", "agent": "owner"})
        self.job("make", "-y", src, "--set", "PHOTOS", "--medium", "auto", "--no-ecc",
                 "--output-dir", os.path.join(self.tmp, "out"), "--note", "hello", "--draft", "-", stdin=draft)
        disc = self.ok("list", "--json")["discs"][0]
        self.assertEqual(disc["Note"], ["hello"])
        self.job("note", disc["Id"], "second")
        self.assertIn("No copy yet", self.ok("todo"))                  # made, not burned: Verify says so
        self.assertEqual(self.ok("find", "--json", "--limit", "500", "img_0001")["total"], 1)
        self.assertEqual(self.ok("list", "--json")["discs"][0]["Note"], ["hello", "second"])
        self.assertIn("best", self.ok("tags"))                         # the draft's tags reached the disc
        self.job("describe", disc["Id"], "--apply", "-",
                 stdin=json.dumps({"title": "Spring photos", "authorship": "human", "agent": "owner"}))
        self.assertEqual(self.ok("list", "--json")["discs"][0]["Title"], "Spring photos")

    @unittest.skipUnless(HAVE_ARV, "src/arv/build/arv (make) and 7z required")
    def test_copies(self):
        """A disc's copies by letter: burned with a drive (read back), burned without, stored, then one checked."""
        self.start()
        disc, image = self.make_disc()
        self.job("burned", disc, "--device", image, "--location", "Shelf")
        self.job("burned", disc, "--copies", "1", "--location", "Attic")
        self.job("stored", disc, image, "--location", "NAS")
        self.job("check", "--copy", "B", image, disc)
        copies = self.ok("objects", disc, "--json")["copies"]
        self.assertEqual([(c["copy"], c["form"], c["at"], c["readBack"]) for c in copies],
                         [("A", "disc", "Shelf", True), ("B", "disc", "Attic", True), ("C", "iso", "NAS", True)])
        self.assertTrue(copies[1]["checked"])        # B: read back only by the check that named it
        self.assertFalse(copies[0]["checked"])       # A: read back when burned, not checked since
        self.assertNotEqual(self.run_arv("objects", "NOPE", "--json")["returncode"], 0)

    @unittest.skipUnless(HAVE_ARV, "src/arv/build/arv (make) and 7z required")
    def test_identify_inventory_audit(self):
        """A copy known by a scanned barcode, found by it, and checked against a shelf; then a letter two
        catalogues gave out twice, relabelled, and a field they changed apart, which Audit shows."""
        self.start()
        disc, image = self.make_disc()
        arv = lambda home, *a: subprocess.run([ARV, "--home", home] + list(a), capture_output=True, text=True)
        self.ok("location", "add", "SHELF", "Shelf")
        self.job("burned", disc, "--copies", "1", "--location", "SHELF")
        self.job("identify", disc, "A", "--barcode", " CASE-77 ")
        found = self.ok("find", "--json", "--limit", "500", "case-77")["copies"]
        self.assertEqual([(c["disc"], c["copy"], c["identifier"], c["at"]) for c in found], [(disc, "A", "barcode:CASE-77", "Shelf")])
        self.assertIn({"code": "SHELF", "name": "Shelf", "parent": "", "temperature": "", "description": ""},
                      self.ok("location", "list", "--json")["locations"])
        inv = self.run_arv("inventory", "SHELF", stdin="CASE-77\nNOPE9\n")
        self.assertEqual(inv["returncode"], 0, inv["out"])         # only a copy missing is exit 1
        self.assertIn("UNKNOWN    NOPE9", inv["out"])
        self.assertNotIn("MISSING", inv["out"])
        self.job("check", "--copy", "A", image, disc)
        # a second catalogue of the archive: each gives out letter B, and each sets the disc's access
        other = os.path.join(self.tmp, "other")
        shutil.copytree(self.home, other)
        self.assertEqual(arv(other, "where", "--here").returncode, 0)
        for home, place, access in ((self.home, "SHELF", "public"), (other, "ATTIC", "sealed")):
            self.assertEqual(arv(home, "burned", disc, "--copies", "1", "--location", place).returncode, 0)
            self.assertEqual(arv(home, "access", disc, access).returncode, 0)
        merged = self.job("rebuild", other)
        self.assertIn("arv relabel %s B NEW --at ATTIC" % disc, "\n".join(merged["lines"]))
        audit = self.run_arv("audit")
        self.assertEqual(audit["returncode"], 1)
        self.assertIn("CONFLICT  %s Access" % disc, audit["out"])
        self.job("relabel", disc, "B", "C", "--at", "ATTIC")
        copies = self.ok("objects", disc, "--json")["copies"]
        self.assertEqual(sorted((c["copy"], c["at"]) for c in copies), [("A", "Shelf"), ("B", "Shelf"), ("C", "ATTIC")])
        jobs = json.loads(self.request("/api/jobs")[1])["jobs"]
        self.assertEqual(jobs[-1]["argv"], ["--home", self.home, "relabel", disc, "B", "C", "--at", "ATTIC"])

    @unittest.skipUnless(os.path.exists(FAKE_LLM), "make -C src/arv-assist check builds the fake model server")
    def test_llm_suggestions_through_arv_assist(self):
        replies = os.path.join(self.tmp, "text.reply")
        with open(replies, "w") as f:
            json.dump({"title": "Holiday photos", "description": "Photos.", "subjects": ["Travel"], "questions": ["Where?"],
                       "tags": {}}, f)
        port = os.path.join(self.tmp, "port")
        self.fake = subprocess.Popen([FAKE_LLM, port, os.path.join(self.tmp, "log"), replies, replies])
        for _ in range(50):
            if os.path.exists(port) and open(port).read().strip():
                break
            time.sleep(0.1)
        url = "http://127.0.0.1:%s/v1" % open(port).read().strip()
        self.start({"url": url})
        status = json.loads(self.run_arv("llm-status", program="arv-assist")["out"])
        self.assertEqual((status["available"], status["model"]), (True, "fake-model"))
        src = os.path.join(self.tmp, "Trip")
        write(os.path.join(src, "a.txt"), "x", 2019)
        got = json.loads(self.run_arv("suggest", program="arv-assist",
                                      stdin=json.dumps({"source": src, "answers": [["Where?", "Kyoto"]]}))["out"])
        self.assertEqual((got["title"], got["subjects"], got["agent"]), ("Holiday photos", ["travel"], "llm:fake-model"))
        with open(os.path.join(self.tmp, "log")) as f:
            self.assertIn("Kyoto", f.read())

    @unittest.skipUnless(HAVE_ARV, "src/arv/build/arv (make) and 7z required")
    def test_mastering_plan(self):
        self.start()
        video, photos = os.path.join(self.tmp, "pc", "wedding.mkv"), os.path.join(self.tmp, "nas", "photos")
        write(video, "film", 2025)
        write(os.path.join(photos, "a.jpg"), "jpeg", 2025)
        listing = json.loads(self.request("/api/browse?files=1&path=" + os.path.join(self.tmp, "pc"))[1])
        self.assertEqual(listing["files"], [{"name": "wedding.mkv", "bytes": 4}])
        self.ok("plan", "new", "trip", "--title", "Trip", "--set", "TRIP", "--medium", "bd25")
        self.assertEqual(self.ok("plan", "list", "--all", "--json"), {"plans": [{"name": "trip", "discs": 1, "items": 0, "made": None}]})
        self.ok("plan", "add", "trip", video, photos, "--disc", "auto")
        self.ok("plan", "move", "trip", "photos", "--from", "1", "--disc", "new")
        shown = self.ok("plan", "show", "trip", "--json")
        self.assertEqual([[i["path"] for i in d["items"]] for d in shown["discs"]], [["wedding.mkv"], ["photos"]])
        self.assertEqual(shown["discs"][1]["items"][0]["files"], 1)
        refused = self.run_arv("plan", "add", "trip", video, "--disc", "1")
        self.assertNotEqual(refused["returncode"], 0)
        self.assertIn("in the plan already", refused["out"] + refused["err"])
        self.job("plan", "make", "trip", "-y", "--output-dir", os.path.join(self.tmp, "out"), "--no-ecc")
        self.assertEqual(len(self.ok("plan", "show", "trip", "--json")["volumes"]), 2)
        self.assertIsNotNone(self.ok("plan", "list", "--all", "--json")["plans"][0]["made"])
        self.ok("plan", "again", "trip", "trip2")
        copy = self.ok("plan", "show", "trip2", "--json")
        self.assertEqual((copy["made"], copy["from"], [[i["path"] for i in d["items"]] for d in copy["discs"]]),
                         (None, "trip", [["wedding.mkv"], ["photos"]]))
        self.assertEqual([i["archived"] for d in copy["discs"] for i in d["items"]], [None, None])   # only when asked
        checked = self.ok("plan", "show", "trip2", "--json", "--archived")
        self.assertEqual([(i["archived"]["object"], i["archived"]["version"], i["archived"]["onDiscs"] == i["archived"]["files"])
                          for d in checked["discs"] for i in d["items"]], [("wedding.mkv", 1, True), ("photos", 1, True)])
        card = os.path.join(self.tmp, "card", "IMG_0001.JPG")
        write(card, "raw", 2025)
        self.ok("plan", "add", "trip2", card, "--disc", "1")
        item = [i for d in self.ok("plan", "show", "trip2", "--json")["discs"] for i in d["items"] if i["path"] == "IMG_0001.JPG"][0]
        self.assertEqual(item["source"], card)
        self.assertEqual(len(self.ok("list", "--json")["discs"]), 2)
        kept = self.ok("objects", "--json")
        self.assertEqual(sorted((o["name"], o["kind"], o["versions"]) for o in kept["objects"]),
                         [("photos", "folder", 1), ("wedding.mkv", "file", 1)])
        self.assertEqual({o["name"]: [s["there"] for s in o["sources"]] for o in kept["objects"]},
                         {"photos": [True], "wedding.mkv": [True]})


if __name__ == "__main__":
    unittest.main()
