"""Tests for arv's Python add-on (the local AI helpers and gui), with the C arv for everything else.

Run: python3 -m unittest discover -s tests. The C arv (src/arvc) is built here if it is missing; the
C arv's own checks are make -C src/arvc check.
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
sys.path.insert(0, os.path.join(REPO, "src"))

from arv import catalog, cli, discid, recfile  # noqa: E402

os.environ["ARV_APE"] = "none"   # the small test media have no room for tools/arv.com (check.sh tests it)
ARVC = os.path.join(REPO, "src", "arvc", "build", "arvc")


def arvc_available():
    """The C arv, built here if a compiler is present; 7z reads the images."""
    if not os.path.exists(ARVC) and shutil.which("make") and shutil.which("cc"):
        subprocess.run(["make", "-s", "-C", os.path.join(REPO, "src", "arvc")],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return os.path.exists(ARVC) and shutil.which("7z") is not None


HAVE_IMAGE_TOOLS = arvc_available()


TINY_JPEG = __import__("base64").b64decode(  # a valid 1x1 JPEG, so exiftool can write to it
    "/9j/4AAQSkZJRgABAQEASABIAAD/2wBDAP////////////////////////////////////////////////////////////"
    "//////////////////////////wgALCAABAAEBAREA/8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABPxA=")


def write(path, text, year=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb" if isinstance(text, bytes) else "w", **({} if isinstance(text, bytes) else {"encoding": "utf-8"})) as f:
        f.write(text)
    if year:
        ts = __import__("datetime").datetime(year, 6, 1).timestamp()
        os.utime(path, (ts, ts))


def disc_id_then(text):
    return lambda disc_id: disc_id + " " + text


def run_cli(*argv):
    """arv as the launcher runs it: the add-on's commands in-process, the rest in the C arv.
    Returns (exit status, or the error message; standard output)."""
    i = 0
    while i < len(argv) and argv[i].startswith("-"):
        i += 2 if argv[i] in ("--home", "--archive", "-C") else 1
    if i < len(argv) and argv[i] in cli.COMMANDS:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            try:
                code = cli.main(list(argv))
            except SystemExit as e:
                code = e.code
        return code, out.getvalue()
    proc = subprocess.run([ARVC] + list(argv), capture_output=True, text=True, stdin=subprocess.DEVNULL)
    if proc.returncode:
        return (proc.stderr.strip().splitlines() or [str(proc.returncode)])[-1], proc.stdout
    return 0, proc.stdout


@unittest.skipUnless(HAVE_IMAGE_TOOLS, "the C arv (src/arvc) or 7z not available")
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


class HomeDiscoveryTest(unittest.TestCase):
    def test_arv_folder_pointer_disc_root_and_machine_config(self):
        from arv import homes
        with tempfile.TemporaryDirectory() as d:
            env = {"XDG_CONFIG_HOME": os.path.join(d, "cfg"), "XDG_DATA_HOME": os.path.join(d, "data"),
                   "ARV_HOME": "", "BLURAY_ARCHIVE_HOME": ""}
            old = {k: os.environ.get(k) for k in env}
            os.environ.update(env)
            try:
                tree, other, outside = (os.path.join(d, n) for n in ("tree", "other", "outside"))
                deep = os.path.join(tree, "repo", "sub")
                os.makedirs(os.path.join(tree, "repo", ".git"))     # git does not stop the walk
                os.makedirs(deep)
                os.makedirs(other)
                os.makedirs(outside)
                self.assertEqual(run_cli("init", tree, "--name", "family", "--default")[0], 0)
                arv = os.path.join(tree, ".arv")
                self.assertTrue(os.path.isdir(os.path.join(arv, "catalog")))
                self.assertTrue(os.path.exists(os.path.join(arv, "cache", "CACHEDIR.TAG")))
                self.assertEqual(homes.find(start=deep)[0], arv)
                # a pointer file in another tree, written with Windows line endings
                with open(os.path.join(other, ".arv"), "w", newline="") as f:
                    f.write("Home: ../tree/.arv\r\n")
                self.assertEqual(homes.find(start=other)[0], arv)
                # outside any tree: the machine config's default; --archive by name; --home wins
                self.assertEqual(homes.find(start=outside)[0], arv)
                self.assertEqual(homes.find(archive="family", start=outside)[0], arv)
                self.assertEqual(homes.find(home="x", start=deep)[0], "x")
                # make: the walk starts at the folder being archived
                os.remove(homes.config_path())
                self.assertEqual(homes.find(start=outside, source=deep)[0], arv)
                self.assertEqual(homes.find(start=outside)[0], os.path.join(d, "data", "arv"))
                # the root of an archive disc: its catalog/ is read in place
                write(os.path.join(outside, "catalog.rec"), "%rec: Archive\n")
                write(os.path.join(outside, "catalog", "archive.rec"), "%rec: Disc\n")
                self.assertEqual(homes.find(start=outside)[0], os.path.join(outside, "catalog"))
            finally:
                for k, v in old.items():
                    if v is None:
                        os.environ.pop(k, None)
                    else:
                        os.environ[k] = v


class SetsTest(unittest.TestCase):
    def test_vocabulary_is_a_dag_of_words(self):
        from arv import sets
        with tempfile.TemporaryDirectory() as d:
            vocab = sets.load(catalog.Home(d))
            self.assertTrue(os.path.exists(os.path.join(d, "config", "sets.rec")))  # copied for editing
            self.assertEqual(vocab.paths("TRIP"), ["MEMORIES/PHOTO/TRIP"])
            self.assertEqual(vocab.paths("SCAN"), ["MEMORIES/PHOTO/SCAN", "RECORDS/SCAN"])  # two parents
            self.assertEqual(vocab.ancestors("SCAN"), {"MEMORIES", "PHOTO", "RECORDS"})
            for word, code in {"Photos": "PHOTO", "Taxes_2019": "TAXES", "scans": "SCAN", "Holiday": "TRIP",
                               "Emails": "EMAIL", "zzz": None}.items():
                self.assertEqual(vocab.guess(word), code, word)
            for code in vocab.entries:
                self.assertTrue(discid.SET_RE.match(code), code)  # every code fits in an id

    def test_cycles_and_unknown_parents_are_rejected(self):
        from arv import sets
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "v.rec")
            for body, message in (("Code: AA\nName: a\nParent: BB\n\nCode: BB\nName: b\nParent: AA\n", "cycle"),
                                  ("Code: AA\nName: a\nParent: ZZ\n", "unknown parent")):
                with open(path, "w", encoding="utf-8") as f:
                    f.write("%rec: Set\n\n" + body)
                with self.assertRaisesRegex(sets.VocabError, message):
                    sets.load(None, path)

    def test_aliases_scope_notes_and_match_rules(self):
        from arv import sets
        vocab = sets.load(None, sets.DEFAULT_SETS)
        self.assertEqual(vocab.resolve("holidays"), "TRIP")        # alias
        self.assertEqual(vocab.resolve("trips"), "TRIP")           # plural of a code
        self.assertEqual(vocab.resolve("PROJECTS"), "PROJECTS")    # a code beats an alias when typed
        self.assertEqual(vocab.guess("Projects"), "PROJ")          # but a folder name means one project set
        self.assertEqual(vocab.guess("2019_Vacation"), "TRIP")
        self.assertIsNone(vocab.resolve("zzz"))
        self.assertTrue(vocab.get("VIDEO").scope_note)
        got = vocab.match(["board/main.kicad_pcb", "board/fw/main.c", "tool/.git/HEAD", "tool/x.py", "a.txt"])
        self.assertEqual(got, {"ELEC": 1, "CODE": 3})
        self.assertTrue(sets.path_matches("*/Taxes/*", "2019/taxes/return.pdf"))
        self.assertFalse(sets.path_matches("*/Taxes/*", "2019/tax/return.pdf"))
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "v.rec")
            with open(path, "w", encoding="utf-8") as f:
                f.write("%rec: Set\n\nCode: AA\nName: a\nAlias: same\n\nCode: BB\nName: b\nAlias: Same\n")
            with self.assertRaisesRegex(sets.VocabError, "alias 'Same' of BB is already AA"):
                sets.load(None, path)

    def test_date_ranges_to_the_day(self):
        self.assertTrue(discid.covers("2019-07-14/2019-07-20", "2019-07-15"))
        self.assertFalse(discid.covers("2019-07-14/2019-07-20", "2019-08"))
        self.assertTrue(discid.covers("2019/..", "2030"))
        self.assertEqual(discid.compose("TRIP", 1, "2019-12-24/2020-01-02")[:22], "TRIP-01_201912-202001_")


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
        from arv import describe, llm
        entries = describe.folder_entries(self.src)
        inv = llm.inventory(entries, "Trip", text_root=self.src)
        self.assertIn("Pictures from our holiday.", inv)  # README text is included
        self.assertIn("photos/2019 trip/", inv)
        result = llm.suggest(llm.Client(self.fake.url), inv, folders=llm.folders_of(entries))
        self.assertEqual(result["subjects"], ["travel", "family"])  # lowercased, de-duplicated
        self.assertEqual(result["folder_tags"], {"photos/2019 trip": ["travel", "2019"]})  # unknown folder dropped
        self.assertEqual(self.fake.requests[0]["model"], "fake-model")  # picked from /models

    def test_fenced_json_and_bad_json(self):
        from arv import llm
        self.fake.reply = "Sure!\n```json\n" + json.dumps(REPLY) + "\n```"
        self.assertEqual(llm.suggest(llm.Client(self.fake.url), "inv")["title"], "Family trip photos 2019")
        self.fake.reply = "I cannot help with that."
        with self.assertRaises(llm.LLMError):
            llm.suggest(llm.Client(self.fake.url), "inv")

    def test_refuses_remote_server(self):
        from arv import llm
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
        from arv import describe, llm
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

    @unittest.skipUnless(HAVE_IMAGE_TOOLS, "the C arv and 7z required")
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
        self.assertEqual(event.get_all("Agent"), ["llm:test", catalog.person()])
        self.assertEqual(event.get("Authorship"), "accepted")   # applying a saved draft accepts it
        self.assertIn("TAG", run_cli("--home", home, "find", "travel")[1])

    def test_make_llm_points_to_describe(self):
        code, _ = run_cli("--home", os.path.join(self.tmp, "home"), "make", "-y", "--no-ecc", "--llm",
                          "--llm-url", self.fake.url, self.src)
        self.assertIn("arv describe FOLDER --save", str(code))


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
        from arv import describe, vision
        picked = vision.sample(describe.folder_entries(self.src), per_folder=3)
        self.assertEqual([e.path.rsplit("/", 1)[1] for e in picked["photos/beach day"]],
                         ["IMG_0.png", "IMG_1.png", "IMG_3.png"])  # evenly spaced

    def test_vision_is_local_only(self):
        from arv import llm, vision
        with self.assertRaises(llm.LLMError):
            vision.VisionClient("http://203.0.113.5:11434/v1")

    def test_parse_image_replies(self):
        from arv import vision
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

    @unittest.skipUnless(HAVE_IMAGE_TOOLS, "the C arv and 7z required")
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
        from arv import models
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
        from arv import models
        models.MODELS.pop("test-model", None)
        shutil.rmtree(self.tmp)

    def tagger(self):
        from arv import catalog, models, tagger
        home = catalog.Home(self.home)
        models.install_model_file(home, "test-model", self.model_file)
        return tagger.Tagger(home, self.binary, model_name="test-model", vocab_file=self.vocab)

    def test_suggest_and_camera_names_ignored(self):
        from arv import describe, tagger
        texts = tagger.summaries(describe.folder_entries(self.src), self.src)
        self.assertNotIn("DSC", texts["beach trip"])
        got = self.tagger().suggest(texts, top=1)
        self.assertEqual({f: t[0][0] for f, t in got.items()},
                         {"cats and kittens": "pets", "scripts": "code", "beach trip": "travel"})

    def test_learns_from_reviews(self):
        from arv import bag, tagger
        t = self.tagger()
        seen = tagger.summaries([bag.Entry("kyoto temples garden/a.jpg", 1, 0)])
        t.remember(seen, {"kyoto temples garden": ["japan"]})
        again = tagger.summaries([bag.Entry("kyoto temples garden/b.jpg", 1, 0),
                                  bag.Entry("scripts/x.py", 1, 0)])
        got = t.suggest(again, top=2)
        self.assertIn("japan", [x for x, _ in got["kyoto temples garden"]])
        self.assertNotIn("japan", [x for x, _ in got["scripts"]])

    def test_model_checksum_is_enforced(self):
        from arv import catalog, models
        bad = os.path.join(self.tmp, "bad.gguf")
        with open(bad, "wb") as f:
            f.write(b"something else")
        with self.assertRaises(models.ModelError):
            models.install_model_file(catalog.Home(self.home), "test-model", bad)

    def test_cli_save_draft_and_apply_to_disc(self):
        from arv import catalog, models
        models.install_model_file(catalog.Home(self.home), "test-model", self.model_file)
        opts = ["--llama-embedding", self.binary, "--model", "test-model", "--vocab", self.vocab]
        draft = os.path.join(self.tmp, "d.json")
        code, _ = run_cli("--home", self.home, "tag", self.src, "--save", draft, *opts)
        self.assertEqual(code, 0)
        with open(draft, encoding="utf-8") as f:
            d = json.load(f)
        self.assertEqual(d["folder_tags"]["scripts"][0], "code")
        self.assertEqual(d["authorship"], "suggested")              # no terminal: nobody reviewed it
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
        modified = [(e.get("Authorship"), e.get_all("Agent")) for e in events if e.get("Type") == "metadata modification"]
        self.assertEqual(modified, [
            ("accepted", ["embeddings:test-model", catalog.person()]),   # make --draft: a person chose the draft
            ("suggested", ["embeddings:test-model"]),                    # tag --apply without a terminal
        ])

    def test_rules_only_and_aliases_in_review(self):
        from arv import catalog as cat_mod, tagger
        with open(self.vocab, "a", encoding="utf-8") as f:
            f.write("Alias: holiday\n\nName: electronics\nDescription: circuits\nMatch: *.kicad_pcb\n")
        write(os.path.join(self.src, "board", "x.kicad_pcb"), "x", 2020)
        vocab = tagger.load_tag_vocab(None, self.vocab)
        self.assertEqual(vocab.canonical(" Holiday "), "travel")
        answers = iter(["holiday, Place: Kyoto"])
        got = tagger.review({"beach trip": [("pets", 0.5)]}, True, ask=lambda _: next(answers), vocab=vocab)
        self.assertEqual(got, {"beach trip": ["travel", "place:kyoto"]})
        self.assertEqual(cat_mod.hierarchical("place:kyoto"), "place|kyoto")
        code, out = run_cli("--home", self.home, "tag", self.src, "--rules-only", "--vocab", self.vocab)
        self.assertEqual(code, 0)
        result = json.loads(out)
        self.assertEqual(result["folder_tags"], {"board": ["electronics"]})
        self.assertEqual((result["agent"], result["authorship"]), ("match rules", "automatic"))

    def test_embeddings_api_engine(self):
        from arv import catalog, tagger
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
        from arv import gui
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

    @unittest.skipUnless(HAVE_IMAGE_TOOLS, "the C arv and 7z required")
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
