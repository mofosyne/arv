"""arv-gui (`arv gui`): a local web interface over the arv command (Python, standard library only).

Serves a single page on 127.0.0.1 and opens it in the default browser. Every
action runs the same arv commands as the terminal (and arv-assist for the local
LLM's suggestions), so the GUI adds no behaviour of its own; it only reads the
catalogue to show it. The page URL and every API request must carry a
per-session token, and the Host header must be the loopback address, so other
web pages and other local users cannot drive it.
"""

import json
import os
import secrets
import shutil
import subprocess
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from . import catalog

DISC_FIELDS = ("Id", "Part", "Title", "Set", "Category", "Path", "Coverage", "Date", "Location", "Description", "Subject", "Note", "Files", "Copies")


DRAFT_FIELDS = ("title", "description", "subjects", "folder_tags")

def disc_summary(disc):
    out = {}
    for name in DISC_FIELDS:
        values = disc.get_all(name)
        if values:
            out[name] = values if name in ("Subject", "Note", "Category", "Path", "Location") else values[0]
    return out


HERE = os.path.dirname(os.path.abspath(__file__))
MAX_OUTPUT_LINES = 5000


def program(name, built):
    """arv or arv-assist: as arv gave it ($ARV, $ARV_ASSIST), in this checkout, or on PATH."""
    env = os.environ.get(name.upper().replace("-", "_"))
    candidates = [env, os.path.join(os.path.dirname(os.path.dirname(HERE)), *built), shutil.which(name)]
    for c in candidates:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return name


ARV = program("arv", ("arv", "build", "arv"))
ASSIST = program("arv-assist", ("arv-assist", "build", "arv-assist"))


class Job:
    def __init__(self, job_id, argv):
        self.id, self.argv = job_id, argv
        self.lines, self.returncode, self.done = [], None, False
        self.proc = subprocess.Popen(
            [ARV] + argv, stdin=subprocess.DEVNULL,
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
            full = cat.with_binding(d)
            summary.update({k: full.get(k) for k in ("Media", "Bytes", "Ecc", "Rights", "Creator", "Access") if full.get(k)})
            summary["Where"] = cat.where(d)
            summary["Events"] = [dict(e.fields) for e in cat.events_for(d.get("Id"))]
            out.append(summary)
        return {"home": self.home.path, "discs": out}

    def find(self, params):
        pattern = params.get("q", "").strip()
        if not pattern:
            return {"discs": [], "files": [], "total": 0}
        cat = self.home.load()
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

    def assist(self, argv, request=None):
        """arv-assist's JSON answer to one of the GUI's calls (llm-status, suggest)."""
        o = self.llm_options
        opts = []
        for key, flag in (("url", "--llm-url"), ("model", "--llm-model"), ("vision_url", "--vision-url"),
                          ("vision_model", "--vision-model")):
            if o.get(key):
                opts += [flag, o[key]]
        if o.get("allow_remote"):
            opts.append("--llm-allow-remote")
        try:
            proc = subprocess.run([ASSIST, "--home", self.home.path] + argv + opts, capture_output=True, text=True,
                                  input=json.dumps(request) if request is not None else "")
            return json.loads(proc.stdout)
        except (OSError, ValueError) as err:
            return {"error": "arv-assist did not answer (%s)" % err}

    def llm_status(self, _params):
        return self.assist(["llm-status"])

    def post_llm_suggest(self, body):
        """One suggestion round for a folder (source) or a disc (disc_id); may take a minute."""
        if body.get("disc_id"):
            if not self.home.load().disc(body["disc_id"]):
                raise LookupError("no such disc")
        elif not os.path.isdir(os.path.abspath(os.path.expanduser(body["source"]))):
            raise ValueError("not a folder: %s" % body["source"])
        request = {k: body.get(k) for k in ("disc_id", "answers", "previous", "seen", "vision") if body.get(k)}
        if not body.get("disc_id"):
            request["source"] = os.path.abspath(os.path.expanduser(body["source"]))
        result = self.assist(["suggest"], request)
        if "error" not in result:
            self.last_suggestion = {k: result.get(k) for k in DRAFT_FIELDS}
        return result

    def draft_authorship(self, draft):
        """accepted when the person sent back the model's last suggestion unchanged, else edited."""
        if not catalog.is_model(draft.get("agent") or "llm"):
            return "human"
        last = getattr(self, "last_suggestion", None)
        sent = {k: draft.get(k) for k in DRAFT_FIELDS}
        norm = lambda d: {k: v or None for k, v in d.items()}
        return "accepted" if last is not None and norm(last) == norm(sent) else "edited"

    def write_draft(self, draft):
        folder = self.home.drafts_dir
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, "draft-%s.json" % secrets.token_hex(4))
        data = {"title": draft.get("title"), "description": draft.get("description"),
                "subjects": draft.get("subjects") or [], "notes": draft.get("notes") or [],
                "folder_tags": draft.get("folder_tags") or {},
                "folder_captions": draft.get("folder_captions") or {},
                "authorship": self.draft_authorship(draft), "agent": draft.get("agent") or "llm"}
        with open(path, "w", encoding="utf-8") as f:    # a draft, as arv describe --save writes them
            json.dump(data, f, ensure_ascii=False, indent=2)
            f.write("\n")
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
        """note / locate / burned / rebuild, run as jobs so errors show like the CLI's."""
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


def main(argv=None):
    """arv-gui [--home H] [--port N] [--no-browser] [--llm-url U] [--llm-model M] [--llm-allow-remote]
    [--vision-url U] [--vision-model M]: arv runs it for `arv gui`."""
    import argparse
    p = argparse.ArgumentParser(prog="arv gui", description="arv's graphical interface, in your web browser")
    p.add_argument("--home", help="the home catalogue (arv passes the one it found)")
    p.add_argument("--port", type=int, default=0, help="port on 127.0.0.1 (default: any free port)")
    p.add_argument("--no-browser", action="store_true", help="print the URL instead of opening a browser")
    for flag in ("--llm-url", "--llm-model", "--vision-url", "--vision-model"):
        p.add_argument(flag)
    p.add_argument("--llm-allow-remote", action="store_true")
    p.add_argument("--vision", action="store_true", help=argparse.SUPPRESS)
    args = p.parse_args(argv)
    llm_options = {"url": args.llm_url, "model": args.llm_model, "allow_remote": args.llm_allow_remote,
                   "vision_url": args.vision_url, "vision_model": args.vision_model}
    server, url = serve(args.home, args.port, not args.no_browser, llm_options)
    print("arv's interface is running at %s  (Ctrl+C to stop)" % url, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0
