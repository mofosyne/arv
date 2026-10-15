"""arv-gui (`arv gui`): a local web interface over the arv command (Python, standard library only).

Serves a single page on 127.0.0.1 and opens it in the default browser. The page does everything
through arv: it builds each command, and this server only runs it (arv, or arv-assist for the local
model's suggestions) and passes back what it printed. So the server knows no features: a new one is
a change to gui.html alone. Besides running commands it lists folders, for the page's file pickers.

The page URL and every API request must carry a per-session token, and the Host header must be the
loopback address, so other web pages and other local users cannot drive it. Commands are run
without a shell, and the first word must be a command (not an option), so nothing can be injected.
"""

import json
import os
import re
import secrets
import shutil
import subprocess
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
MAX_OUTPUT_LINES = 5000
COMMAND = re.compile(r"^[a-z][a-z-]*$")


def program(name, built):
    """arv or arv-assist: as arv gave it ($ARV, $ARV_ASSIST), in this checkout, or on PATH."""
    env = os.environ.get(name.upper().replace("-", "_"))
    candidates = [env, os.path.join(os.path.dirname(os.path.dirname(HERE)), *built), shutil.which(name)]
    for c in candidates:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return name


PROGRAMS = {"arv": program("arv", ("arv", "build", "arv")),
            "arv-assist": program("arv-assist", ("arv-assist", "build", "arv-assist"))}


def command_line(name, argv, llm_options):
    """The program and its arguments, checked: after the home (--home PATH or -C PATH), a command
    word; never `arv gui` itself. A leading ~ is the user's home folder, as in a shell."""
    if name not in PROGRAMS:
        raise ValueError("unknown program %r" % name)
    if not isinstance(argv, list) or not all(isinstance(a, str) for a in argv):
        raise ValueError("argv: a list of strings")
    i = 2 if argv[:1] in (["--home"], ["-C"]) else 0
    if len(argv) <= i or not COMMAND.match(argv[i]) or (name == "arv" and argv[i] == "gui"):
        raise ValueError("not a command: %r" % (argv[i] if len(argv) > i else ""))
    argv = [os.path.expanduser(a) if a == "~" or a.startswith("~/") else a for a in argv]
    if name == "arv-assist":                    # the model server arv gui was started with
        o = llm_options or {}
        for key, flag in (("url", "--llm-url"), ("model", "--llm-model"), ("vision_url", "--vision-url"),
                          ("vision_model", "--vision-model")):
            if o.get(key):
                argv += [flag, o[key]]
        if o.get("allow_remote"):
            argv.append("--llm-allow-remote")
    return [PROGRAMS[name]] + argv


class Job:
    """A command that takes a while (make, check, burned with a drive ...): its output as it comes."""

    def __init__(self, job_id, argv, stdin=None):
        self.id, self.argv = job_id, argv
        self.lines, self.returncode, self.done = [], None, False
        self.proc = subprocess.Popen(
            argv, stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        if stdin is not None:
            self.proc.stdin.write(stdin)
            self.proc.stdin.close()
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.proc.stdout:
            self.lines.append(line.rstrip("\n"))
            if len(self.lines) > MAX_OUTPUT_LINES:
                del self.lines[: len(self.lines) - MAX_OUTPUT_LINES]
        self.returncode = self.proc.wait()
        self.done = True

    def as_dict(self, since=0):
        return {"id": self.id, "argv": self.argv[1:], "done": self.done, "returncode": self.returncode,
                "lines": self.lines[since:], "next": len(self.lines)}


class App:
    def __init__(self, home=None, llm_options=None):
        self.home = os.path.abspath(home) if home else None     # as arv gui was given it; the page finds the rest
        self.llm_options = llm_options or {}
        self.token = secrets.token_urlsafe(24)
        self.jobs = {}
        self.lock = threading.Lock()

    def setup(self, _params):
        """Where arv gui was started, and the home it was given: the page asks arv where --json."""
        return {"home": self.home, "cwd": os.getcwd()}

    def run(self, body):
        """{program, argv, stdin, job}: arv (or arv-assist) run to its end, its exit status and what it
        printed, or, with job, started, to be followed through /api/job."""
        argv = command_line(body.get("program") or "arv", body.get("argv"), self.llm_options)
        stdin = body.get("stdin")
        if stdin is not None and not isinstance(stdin, str):
            raise ValueError("stdin: a string")
        if body.get("job"):
            with self.lock:
                job_id = str(len(self.jobs) + 1)
                self.jobs[job_id] = Job(job_id, argv, stdin)
            return self.jobs[job_id].as_dict()
        proc = subprocess.run(argv, capture_output=True, text=True,
                              **({"input": stdin} if stdin is not None else {"stdin": subprocess.DEVNULL}))
        return {"returncode": proc.returncode, "out": proc.stdout, "err": proc.stderr}

    def job(self, params):
        job = self.jobs.get(params.get("id", ""))
        if not job:
            raise LookupError("no such job")
        return job.as_dict(int(params.get("since", 0)))

    def jobs_list(self, _params):
        return {"jobs": [{"id": j.id, "argv": j.argv[1:], "done": j.done, "returncode": j.returncode}
                         for j in self.jobs.values()]}

    def browse(self, params):
        """A folder's folders (and with files=1, its files and their sizes), for the file pickers."""
        path = os.path.abspath(os.path.expanduser(params.get("path") or "~"))
        if not os.path.isdir(path):
            path = os.path.dirname(path)
        try:
            all_names = sorted(n for n in os.listdir(path) if not n.startswith("."))
        except OSError as err:
            return {"path": path, "parent": os.path.dirname(path), "dirs": [], "files": [], "error": str(err)}
        names = [n for n in all_names if os.path.isdir(os.path.join(path, n))]
        out = {"path": path, "parent": os.path.dirname(path), "dirs": names}
        if params.get("files"):
            files = []
            for n in all_names:
                full = os.path.join(path, n)
                if os.path.isfile(full) and not os.path.islink(full):
                    files.append({"name": n, "bytes": os.path.getsize(full)})
            out["files"] = files
        return out


def make_handler(app, port_holder):
    get_routes = {"/api/setup": app.setup, "/api/job": app.job, "/api/jobs": app.jobs_list, "/api/browse": app.browse}
    post_routes = {"/api/run": app.run}

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
            try:
                self._send(200, handler({k: v[0] for k, v in parse_qs(url.query).items()}))
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
                if not isinstance(body, dict):
                    raise ValueError("a JSON object")
                self._send(200, handler(body))
            except (KeyError, ValueError, OSError) as err:
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
