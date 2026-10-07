"""Tests for arv-gui (src/arv-gui), the one part of arv in Python: its catalogue reader and its web
API, which runs arv (and arv-assist) for every action.

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

from arvgui import catalog, gui, recfile  # noqa: E402

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


class GuiTest(unittest.TestCase):
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

    def wait(self, job):
        for _ in range(600):
            j = json.loads(self.request("/api/job?id=%s" % job["id"])[1])
            if j["done"]:
                return j
            time.sleep(0.2)
        self.fail("job did not finish")

    def test_access_control(self):
        self.start()
        self.assertEqual(self.request("/api/discs", token=False)[0], 403)
        self.assertEqual(self.request("/?t=wrong", token=False)[0], 403)
        self.assertEqual(self.request("/api/discs", host="evil.example:80")[0], 403)
        status, page = self.request("/?t=" + self.token, token=False)
        self.assertEqual(status, 200)
        self.assertIn(self.token, page)

    @unittest.skipUnless(HAVE_ARV, "src/arv/build/arv (make) and 7z required")
    def test_make_note_find(self):
        self.start()
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
        owed = json.loads(self.request("/api/todo")[1])["text"]
        self.assertIn("No copy yet", owed)          # made, not burned: Verify says so
        found = json.loads(self.request("/api/find?q=img_0001")[1])
        self.assertEqual(found["total"], 1)
        self.assertEqual(json.loads(self.request("/api/discs")[1])["discs"][0]["Note"], ["hello", "second"])

    @unittest.skipUnless(os.path.exists(FAKE_LLM), "make -C src/arv-assist check builds the fake model server")
    def test_llm_suggestions_through_arv_assist(self):
        replies = os.path.join(self.tmp, "text.reply")
        with open(replies, "w") as f:
            json.dump({"title": "Holiday photos", "description": "Photos.", "subjects": ["Travel"], "questions": ["Where?"],
                       "folder_tags": {}}, f)
        port = os.path.join(self.tmp, "port")
        self.fake = subprocess.Popen([FAKE_LLM, port, os.path.join(self.tmp, "log"), replies, replies])
        for _ in range(50):
            if os.path.exists(port) and open(port).read().strip():
                break
            time.sleep(0.1)
        url = "http://127.0.0.1:%s/v1" % open(port).read().strip()
        self.start({"url": url})
        status = json.loads(self.request("/api/llm/status")[1])
        self.assertEqual((status["available"], status["model"]), (True, "fake-model"))
        src = os.path.join(self.tmp, "Trip")
        write(os.path.join(src, "a.txt"), "x", 2019)
        got = json.loads(self.request("/api/llm/suggest", {"source": src, "answers": [["Where?", "Kyoto"]]})[1])
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
        post = lambda body: json.loads(self.request("/api/plan", body)[1])
        self.assertEqual(post({"action": "new", "name": "trip", "title": "Trip", "set": "TRIP", "medium": "bd25"})["returncode"], 0)
        self.assertEqual(json.loads(self.request("/api/plans")[1]), {"plans": [{"name": "trip", "made": None}]})
        self.assertEqual(post({"action": "add", "name": "trip", "sources": [video, photos], "disc": "auto"})["returncode"], 0)
        self.assertEqual(post({"action": "move", "name": "trip", "paths": ["photos"], "from": 1, "disc": "new"})["returncode"], 0)
        shown = json.loads(self.request("/api/plan?name=trip")[1])
        self.assertEqual([[i["path"] for i in d["items"]] for d in shown["discs"]], [["wedding.mkv"], ["photos"]])
        self.assertEqual(shown["discs"][1]["items"][0]["files"], 1)
        refused = post({"action": "add", "name": "trip", "sources": [video], "disc": "1"})
        self.assertNotEqual(refused["returncode"], 0)
        self.assertIn("in the plan already", refused["output"])
        job = post({"action": "make", "name": "trip", "no_ecc": True, "output_dir": os.path.join(self.tmp, "out")})
        result = self.wait(job)
        self.assertEqual(result["returncode"], 0, "\n".join(result["lines"]))
        self.assertEqual(len(json.loads(self.request("/api/plan?name=trip")[1])["volumes"]), 2)
        self.assertIsNotNone(json.loads(self.request("/api/plans")[1])["plans"][0]["made"])
        again = post({"action": "again", "name": "trip", "new": "trip2"})
        self.assertEqual(again["returncode"], 0, again["output"])
        copy = json.loads(self.request("/api/plan?name=trip2")[1])
        self.assertEqual((copy["made"], copy["from"], [[i["path"] for i in d["items"]] for d in copy["discs"]]),
                         (None, "trip", [["wedding.mkv"], ["photos"]]))
        card = os.path.join(self.tmp, "card", "IMG_0001.JPG")
        write(card, "raw", 2025)
        added = post({"action": "add", "name": "trip2", "sources": [card], "disc": "1", "copy": True})
        self.assertEqual(added["returncode"], 0, added["output"])
        item = [i for d in json.loads(self.request("/api/plan?name=trip2")[1])["discs"] for i in d["items"]
                if i["path"] == "IMG_0001.JPG"][0]
        self.assertEqual(item["origin"], card)
        self.assertTrue(item["source"].startswith(os.path.join(self.home, "drafts", "plans", "trip2")))
        self.assertEqual(len(json.loads(self.request("/api/discs")[1])["discs"]), 2)
        kept = json.loads(self.request("/api/objects")[1])
        self.assertEqual(sorted((o["name"], o["kind"], o["versions"]) for o in kept["objects"]),
                         [("photos", "folder", 1), ("wedding.mkv", "file", 1)])
        self.assertEqual({o["name"]: [s["there"] for s in o["sources"]] for o in kept["objects"]},
                         {"photos": [True], "wedding.mkv": [True]})
        self.assertEqual(self.request("/api/plan", {"action": "new", "name": "../x"})[0], 400)
        self.assertEqual(self.request("/api/plan?name=nope")[0], 404)

    def test_bad_requests(self):
        self.start()
        self.assertEqual(self.request("/api/command", {"command": "rm -rf"})[0], 400)
        self.assertEqual(self.request("/api/job?id=999")[0], 404)


if __name__ == "__main__":
    unittest.main()
