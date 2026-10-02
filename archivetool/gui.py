"""`archive gui`: a local web interface over the CLI (standard library only).

Serves a single page on 127.0.0.1 and opens it in the default browser. Every
action runs the same `archive` commands as the terminal, so the GUI adds no
behaviour of its own. The page URL and every API request must carry a
per-session token, and the Host header must be the loopback address, so other
web pages and other local users cannot drive it.
"""

import json
import os
import secrets
import subprocess
import sys
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from . import catalog, describe, index, llm, vision

DISC_FIELDS = ("Id", "Part", "Title", "Set", "Category", "Path", "Coverage", "Date", "Location", "Description", "Subject", "Note", "Files", "Copies")


def disc_summary(disc):
    out = {}
    for name in DISC_FIELDS:
        values = disc.get_all(name)
        if values:
            out[name] = values if name in ("Subject", "Note", "Category", "Path", "Location") else values[0]
    return out


HERE = os.path.dirname(os.path.abspath(__file__))
ARCHIVE = os.path.join(os.path.dirname(HERE), "archive")
MAX_OUTPUT_LINES = 5000


class Job:
    def __init__(self, job_id, argv):
        self.id, self.argv = job_id, argv
        self.lines, self.returncode, self.done = [], None, False
        self.proc = subprocess.Popen(
            [sys.executable, "-u", ARCHIVE] + argv, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.proc.stdout:
            self.lines.append(line.rstrip("\n"))
            if len(self.lines) > MAX_OUTPUT_LINES:
                del self.lines[: len(self.lines) - MAX_OUTPUT_LINES]
        self.returncode = self.proc.wait()
        self.done = True

    def as_dict(self, since=0):
        return {"id": self.id, "argv": self.argv, "done": self.done, "returncode": self.returncode,
                "lines": self.lines[since:], "next": len(self.lines)}


class App:
    def __init__(self, home, llm_options=None):
        self.home = catalog.Home(home)
        self.llm_options = llm_options or {}
        self.token = secrets.token_urlsafe(24)
        self.jobs = {}
        self.lock = threading.Lock()

    def start_job(self, argv):
        with self.lock:
            job_id = str(len(self.jobs) + 1)
            self.jobs[job_id] = Job(job_id, ["--home", self.home.path] + argv)
        return self.jobs[job_id]

    # ------------------------------------------------------------ API

    def discs(self, _params):
        cat = self.home.load()
        out = []
        for d in cat.discs:
            summary = disc_summary(d)
            summary.update({k: d.get(k) for k in ("Media", "Bytes", "Ecc", "Rights", "Creator", "Access") if d.get(k)})
            summary["Where"] = cat.where(d)
            summary["Events"] = [dict(e.fields) for e in cat.events_for(d.get("Id"))]
            out.append(summary)
        return {"home": self.home.path, "discs": out}

    def find(self, params):
        pattern = params.get("q", "").strip()
        if not pattern:
            return {"discs": [], "files": [], "total": 0}
        cat = self.home.load()
        if index.is_fresh(self.home):
            disc_hits, file_hits = index.find(self.home, cat, pattern)
        else:
            disc_hits, file_hits = catalog.find(self.home, cat, pattern)
        return {"discs": [disc_summary(d) for d in disc_hits],
                "files": [{"disc": d.get("Id"), "title": d.get("Title"), "location": cat.where(d), "path": p}
                          for d, p in file_hits[:500]],
                "total": len(file_hits)}

    def browse(self, params):
        path = os.path.abspath(os.path.expanduser(params.get("path") or "~"))
        if not os.path.isdir(path):
            path = os.path.dirname(path)
        try:
            names = sorted(n for n in os.listdir(path) if not n.startswith(".") and os.path.isdir(os.path.join(path, n)))
        except OSError as err:
            return {"path": path, "parent": os.path.dirname(path), "dirs": [], "error": str(err)}
        return {"path": path, "parent": os.path.dirname(path), "dirs": names}

    def job(self, params):
        job = self.jobs.get(params.get("id", ""))
        if not job:
            raise LookupError("no such job")
        return job.as_dict(int(params.get("since", 0)))

    def jobs_list(self, _params):
        return {"jobs": [{"id": j.id, "argv": j.argv[2:], "done": j.done, "returncode": j.returncode}
                         for j in self.jobs.values()]}

    # ------------------------------------------------------------ local LLM (optional)

    def llm_client(self):
        o = self.llm_options
        return llm.Client(o.get("url"), o.get("model"), o.get("allow_remote", False))

    def llm_status(self, _params):
        try:
            client = self.llm_client()
            return {"available": True, "url": client.url, "model": client.resolve_model()}
        except llm.LLMError as err:
            return {"available": False, "error": str(err)}

    def post_llm_suggest(self, body):
        """One suggestion round for a folder (source) or a disc (disc_id); may take a minute."""
        try:
            client = self.llm_client()
        except llm.LLMError as err:
            return {"error": str(err)}
        if body.get("disc_id"):
            cat = self.home.load()
            disc = cat.disc(body["disc_id"])
            if not disc:
                raise LookupError("no such disc")
            entries = describe.disc_entries(self.home, disc.get("Id"))
            inv = llm.inventory(entries, disc.get("Id"), describe.existing_metadata(disc))
        else:
            src = os.path.abspath(os.path.expanduser(body["source"]))
            if not os.path.isdir(src):
                raise ValueError("not a folder: %s" % src)
            entries = describe.folder_entries(src)
            inv = llm.inventory(entries, os.path.basename(src), text_root=src)
        answers = [(q, a) for q, a in body.get("answers") or [] if str(a).strip()]
        seen = body.get("seen") or {}
        try:
            if body.get("vision") and not seen and not body.get("disc_id"):
                o = self.llm_options
                vclient = vision.VisionClient(o.get("vision_url") or client.url,
                                              o.get("vision_model") or client.resolve_model())
                seen = vision.analyse(vclient, src, entries)
            inv += vision.inventory_section(seen)
            result = llm.suggest(client, inv, answers=answers or None, previous=body.get("previous"),
                                 folders=llm.folders_of(entries))
        except llm.LLMError as err:
            return {"error": str(err)}
        result = describe.with_vision(result, seen)
        result["seen"] = seen  # sent back on refine so images are only analysed once
        result["agent"] = client.agent
        return result

    def write_draft(self, draft):
        folder = self.home.drafts_dir
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, "draft-%s.json" % secrets.token_hex(4))
        describe.save_draft(path, {
            "title": draft.get("title"), "description": draft.get("description"),
            "subjects": draft.get("subjects") or [], "notes": draft.get("notes") or [],
            "folder_tags": draft.get("folder_tags") or {},
            "folder_captions": draft.get("folder_captions") or {}}, draft.get("agent") or "llm")
        return path

    def post_make(self, body):
        argv = ["make", "-y", body["source"]]
        if body.get("draft"):
            argv += ["--draft", self.write_draft(body["draft"])]
        for key in ("set", "title", "description", "creator", "location", "rights", "medium",
                    "min_redundancy", "snapshot", "output_dir"):
            value = str(body.get(key) or "").strip()
            if value:
                argv += ["--" + key.replace("_", "-"), value]
        for key in ("subject", "note"):
            for value in body.get(key) or []:
                if value.strip():
                    argv += ["--" + key, value.strip()]
        for flag in ("split", "ro_crate", "no_defect_management", "no_ecc"):
            if body.get(flag):
                argv.append("--" + flag.replace("_", "-"))
        return self.start_job(argv).as_dict()

    def post_check(self, body):
        argv = ["check"]
        argv += ["--device", body["device"]] if body.get("device") else ["--image", body["image"]]
        if body.get("disc_id"):
            argv.append(body["disc_id"])
        return self.start_job(argv).as_dict()

    def post_simple(self, body):
        """note / locate / burned / rebuild / index, run as jobs so errors show like the CLI's."""
        command = body["command"]
        if command == "note":
            argv = ["note", body["disc_id"], body["text"]]
        elif command == "locate":
            argv = ["locate", body["disc_id"]] + [l.strip() for l in body["location"].split(";") if l.strip()]
        elif command == "burned":
            argv = ["burned", body["disc_id"], "--copies", str(int(body.get("copies") or 1))]
            if body.get("media_id"):
                argv += ["--media-id", body["media_id"]]
        elif command == "rebuild":
            argv = ["rebuild", body["path"]]
        elif command == "index":
            argv = ["index"]
        elif command == "apply_draft":
            argv = ["describe", body["disc_id"], "--apply", self.write_draft(body["draft"])]
        else:
            raise LookupError("unknown command %r" % command)
        return self.start_job(argv).as_dict()


def make_handler(app, port_holder):
    get_routes = {"/api/discs": app.discs, "/api/find": app.find, "/api/browse": app.browse,
                  "/api/job": app.job, "/api/jobs": app.jobs_list, "/api/llm/status": app.llm_status}
    post_routes = {"/api/make": app.post_make, "/api/check": app.post_check, "/api/command": app.post_simple,
                   "/api/llm/suggest": app.post_llm_suggest}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def _send(self, code, body, content_type="application/json"):
            data = body if isinstance(body, bytes) else json.dumps(body).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", content_type + "; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(data)

        def _allowed(self):
            host = self.headers.get("Host", "")
            if host not in ("127.0.0.1:%d" % port_holder[0], "localhost:%d" % port_holder[0]):
                return False
            url = urlparse(self.path)
            if url.path.startswith("/api/"):
                supplied = self.headers.get("X-Archive-Token", "")
            else:
                supplied = parse_qs(url.query).get("t", [""])[0]
            return secrets.compare_digest(supplied, app.token)

        def do_GET(self):
            if not self._allowed():
                return self._send(403, {"error": "forbidden"})
            url = urlparse(self.path)
            if url.path == "/":
                with open(os.path.join(HERE, "gui.html"), encoding="utf-8") as f:
                    page = f.read().replace("__TOKEN__", app.token)
                return self._send(200, page.encode("utf-8"), "text/html")
            handler = get_routes.get(url.path)
            if not handler:
                return self._send(404, {"error": "not found"})
            params = {k: v[0] for k, v in parse_qs(url.query).items()}
            try:
                self._send(200, handler(params))
            except LookupError as err:
                self._send(404, {"error": str(err)})

        def do_POST(self):
            if not self._allowed():
                return self._send(403, {"error": "forbidden"})
            handler = post_routes.get(urlparse(self.path).path)
            if not handler:
                return self._send(404, {"error": "not found"})
            try:
                length = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(length) or b"{}")
                self._send(200, handler(body))
            except (KeyError, ValueError, LookupError) as err:
                self._send(400, {"error": "bad request: %s" % err})

    return Handler


def serve(home=None, port=0, open_browser=True, llm_options=None):
    """Start the server. Returns (server, url); call server.serve_forever() to run it."""
    app = App(home, llm_options)
    port_holder = [port]
    server = ThreadingHTTPServer(("127.0.0.1", port), make_handler(app, port_holder))
    port_holder[0] = server.server_address[1]
    url = "http://127.0.0.1:%d/?t=%s" % (port_holder[0], app.token)
    server.app = app
    if open_browser:
        threading.Timer(0.3, webbrowser.open, [url]).start()
    return server, url


def main(args):
    llm_options = {"url": args.llm_url, "model": args.llm_model, "allow_remote": args.llm_allow_remote,
                   "vision_url": args.vision_url, "vision_model": args.vision_model}
    server, url = serve(args.home, args.port, not args.no_browser, llm_options)
    print("Archive GUI running at %s  (Ctrl+C to stop)" % url, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0
