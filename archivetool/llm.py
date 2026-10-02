"""Optional local-LLM help with descriptive metadata.

Nothing here runs unless asked for (`arv describe`, `arv make --llm`,
or the GUI's "Suggest" buttons), and nothing is written without the user
accepting it. The model gets a compact inventory of the folder or disc (names,
counts, sizes, dates, extensions, and short README-style text files) and returns
suggested Title / Description / Subject values plus specific questions for the
owner. The owner's answers are kept verbatim as notes: they are the part only
a human can supply.

Works with any OpenAI-compatible chat server: Ollama (http://127.0.0.1:11434/v1),
llama.cpp `llama-server`, LM Studio, vLLM. Only loopback servers are allowed
unless --llm-allow-remote is given, because the inventory describes private files.
"""

import collections
import datetime
import ipaddress
import json
import os
import re
import socket
import urllib.error
import urllib.request
from urllib.parse import urlparse

DEFAULT_URL = "http://127.0.0.1:11434/v1"
TEXT_NAMES = re.compile(r"^(readme|notes?|about|description|info|index|contents|changelog|todo)([._-].*)?$", re.I)
TEXT_EXTS = {".txt", ".md", ".rst", ".org", ".nfo", ".rec"}
MAX_TEXT_FILES = 6
MAX_TEXT_CHARS = 1500
MAX_INVENTORY_CHARS = 9000

SYSTEM_PROMPT = """You are an archivist helping someone document a personal archive disc \
(photos, video, source code, documents) so that they, or their family, can \
understand it decades from now.

You receive an inventory of the disc: folder structure, file counts, sizes, \
dates, file types, and the text of a few README-like files. You never see the \
file contents otherwise.

Rules:
- Use the evidence: folder names, file names, dates and README text often say \
what something is (a place, an event, a project). Mention those specifics.
- Only state what the inventory supports. Do not invent names, places, events \
or purposes. If something is unclear, ask about it instead of guessing.
- Title: short (at most 60 characters), specific, human-friendly. Not the \
folder name, not a date alone, not "Personal Archive".
- Description: 2-5 plain sentences on what the disc holds and how it is \
organised, naming the main contents, useful to someone who has never seen it.
- Subjects: 3-8 short lowercase keywords for the whole disc.
- Folder tags: for the folders listed in the inventory (use the exact folder \
path shown, without the trailing slash), 1-5 short lowercase tags each that \
describe what the folder holds, based only on names, types, dates and text \
shown, including any "What sampled images show" lines. Skip folders you \
cannot say anything useful about.
- Questions: up to %(max_questions)d questions that only the owner can answer \
and that would most improve the description. Never ask something the \
inventory already answers (counts, dates, file names, README text). Ask about \
people, places, occasions, purpose, which items are the only copy or most \
important, and unclear names. Refer to concrete folder or file names. No \
yes/no questions.

Example (for a different disc):
{"title": "Lisbon holiday and garden project, 2016-2017",
 "description": "Photos from a family holiday in Lisbon (June 2016) and \
design files for a raised-bed garden project. Photos are grouped by day; the \
garden folder holds sketches and a parts list.",
 "subjects": ["travel", "portugal", "family", "gardening", "diy"],
 "folder_tags": {"photos/2016-06_Lisbon": ["travel", "lisbon", "family"],
                 "garden": ["diy", "gardening", "plans"]},
 "questions": ["Who is in the Lisbon photos?",
               "What is in scan_03.tif in misc/, and why was it kept?"]}

Reply with a single JSON object with exactly these keys and nothing else:
{"title": "...", "description": "...", "subjects": ["..."],
 "folder_tags": {"folder/path": ["tag", "..."]}, "questions": ["..."]}"""


class LLMError(Exception):
    pass


# ---------------------------------------------------------------- client

def is_loopback(url):
    host = urlparse(url).hostname or ""
    if host == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        try:
            return all(ipaddress.ip_address(info[4][0]).is_loopback for info in socket.getaddrinfo(host, None))
        except OSError:
            return False


class Client:
    def __init__(self, url=None, model=None, allow_remote=False, timeout=600):
        self.url = (url or os.environ.get("ARCHIVE_LLM_URL") or DEFAULT_URL).rstrip("/")
        self.model = model or os.environ.get("ARCHIVE_LLM_MODEL")
        self.timeout = timeout
        if not allow_remote and not is_loopback(self.url):
            raise LLMError("refusing to send the inventory to %s: not a local address "
                           "(use --llm-allow-remote for a server you trust)" % self.url)

    def _request(self, path, body=None):
        data = json.dumps(body).encode("utf-8") if body is not None else None
        req = urllib.request.Request(self.url + path, data=data, headers={"Content-Type": "application/json"})
        # Loopback requests must not go through an HTTP proxy from the environment
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        try:
            with opener.open(req, timeout=self.timeout) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as err:
            raise LLMError("%s from %s: %s" % (err.code, self.url + path, err.read().decode("utf-8", "replace")[:300]))
        except (urllib.error.URLError, OSError) as err:
            raise LLMError("cannot reach the LLM server at %s (%s). Is it running?" % (self.url, err))

    def resolve_model(self):
        if not self.model:
            models = self._request("/models").get("data") or []
            if not models:
                raise LLMError("the server at %s lists no models; pass --llm-model" % self.url)
            self.model = models[0]["id"]
        return self.model

    def chat(self, messages, temperature=0.2, json_mode=True):
        body = {"model": self.resolve_model(), "messages": messages, "temperature": temperature, "stream": False}
        if json_mode:
            body["response_format"] = {"type": "json_object"}
        try:
            reply = self._request("/chat/completions", body)
        except LLMError as err:
            if "response_format" not in str(err):
                raise
            del body["response_format"]  # older servers reject it; the prompt still asks for JSON
            reply = self._request("/chat/completions", body)
        try:
            return reply["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError):
            raise LLMError("unexpected reply from the server: %r" % str(reply)[:300])

    @property
    def agent(self):
        return "llm:%s" % (self.model or "unknown")


# ---------------------------------------------------------------- inventory

def _human(n):
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if n < 1024 or unit == "TiB":
            return ("%d %s" % (n, unit)) if unit == "B" else ("%.1f %s" % (n, unit))
        n /= 1024


def inventory(entries, name, existing=None, text_root=None, text_reader=None):
    """Compact, size-bounded text description of a folder or disc for the model.

    entries: objects with .path, .size, .mtime (bag.Entry). text_root: folder the
    paths are relative to (to read README-like files), or text_reader(path) -> str.
    existing: dict of current metadata (Title, Description, Subject, Note ...).
    """
    lines = ["Archive name: %s" % name]
    total = sum(e.size for e in entries)
    lines.append("Files: %d, total %s" % (len(entries), _human(total)))
    if entries:
        years = sorted(datetime.date.fromtimestamp(e.mtime).year for e in entries if e.mtime)
        if years:
            lines.append("File dates (modification time): %d to %d" % (years[0], years[-1]))
    exts = collections.Counter(os.path.splitext(e.path)[1].lower() or "(none)" for e in entries)
    lines.append("File types: " + ", ".join("%s x%d" % (x, n) for x, n in exts.most_common(15)))

    if existing:
        lines.append("")
        lines.append("Metadata already recorded:")
        for key, value in existing.items():
            for v in value if isinstance(value, list) else [value]:
                if v:
                    lines.append("  %s: %s" % (key, v))

    lines.append("")
    lines.append("Folders (files, size, sample names):")
    dirs = collections.OrderedDict()
    for e in entries:
        parent = e.path.rsplit("/", 1)[0] if "/" in e.path else "."
        parts = parent.split("/")
        key = "/".join(parts[:3])  # group deeper folders under depth 3
        d = dirs.setdefault(key, {"n": 0, "size": 0, "samples": [], "deeper": set()})
        d["n"] += 1
        d["size"] += e.size
        if len(parts) > 3:
            d["deeper"].add(parts[3])
        if len(d["samples"]) < 4:
            d["samples"].append(e.path.rsplit("/", 1)[-1])
    for key, d in list(dirs.items())[:80]:
        extra = " (+%d subfolders)" % len(d["deeper"]) if d["deeper"] else ""
        lines.append("  %s/ - %d files, %s%s: %s" % (key, d["n"], _human(d["size"]), extra, ", ".join(d["samples"])))
    if len(dirs) > 80:
        lines.append("  ... and %d more folders" % (len(dirs) - 80))

    texts = [e for e in entries
             if (TEXT_NAMES.match(e.path.rsplit("/", 1)[-1]) or os.path.splitext(e.path)[1].lower() in TEXT_EXTS)
             and e.size <= 256 * 1024]
    texts.sort(key=lambda e: (e.path.count("/"), not TEXT_NAMES.match(e.path.rsplit("/", 1)[-1]), e.path))
    shown = 0
    for e in texts:
        if shown >= MAX_TEXT_FILES:
            break
        try:
            if text_reader:
                text = text_reader(e.path)
            elif text_root:
                with open(os.path.join(text_root, e.path), encoding="utf-8", errors="replace") as f:
                    text = f.read(MAX_TEXT_CHARS + 1)
            else:
                break
        except OSError:
            continue
        if not text or not text.strip():
            continue
        shown += 1
        lines.append("")
        lines.append("--- %s ---" % e.path)
        lines.append(text[:MAX_TEXT_CHARS].strip() + (" [...]" if len(text) > MAX_TEXT_CHARS else ""))

    out = "\n".join(lines)
    if len(out) > MAX_INVENTORY_CHARS:
        out = out[:MAX_INVENTORY_CHARS] + "\n[inventory truncated]"
    return out


# ---------------------------------------------------------------- suggestions

def _parse(content, folders=None):
    content = content.strip()
    fence = re.search(r"```(?:json)?\s*(.*?)```", content, re.S)
    if fence:
        content = fence.group(1)
    start, end = content.find("{"), content.rfind("}")
    if start < 0 or end < start:
        raise LLMError("the model did not return JSON: %r" % content[:200])
    try:
        data = json.loads(content[start:end + 1])
    except json.JSONDecodeError as err:
        raise LLMError("the model returned invalid JSON (%s): %r" % (err, content[:200]))

    def text(value, limit):
        return re.sub(r"\s+", " ", str(value or "")).strip()[:limit]

    def items(value, limit, count):
        if isinstance(value, str):
            value = re.split(r"[,;\n]", value)
        out = []
        for v in value or []:
            v = text(v, limit)
            if v and v.lower() not in (x.lower() for x in out):
                out.append(v)
        return out[:count]

    folder_tags = {}
    raw = data.get("folder_tags")
    if isinstance(raw, dict):
        for folder, tags in raw.items():
            folder = str(folder).strip().strip("/")
            if folders is not None and folder not in folders:
                continue  # only folders that really exist on the disc
            tags = [t.lower() for t in items(tags, 30, 5)]
            if tags:
                folder_tags[folder] = tags

    return {
        "title": text(data.get("title"), 80),
        "description": text(data.get("description"), 1500),
        "subjects": [s.lower() for s in items(data.get("subjects"), 40, 10)],
        "folder_tags": folder_tags,
        "questions": items(data.get("questions"), 300, 8),
    }


def folders_of(entries):
    """Every folder path (and its parents) that contains files, as used in the inventory."""
    out = {"."}
    for e in entries:
        parts = e.path.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            out.add("/".join(parts[:i]))
    return out


def suggest(client, inventory_text, answers=None, previous=None, max_questions=5, folders=None):
    """One round: returns {"title", "description", "subjects", "questions"}.

    answers: [(question, answer)] from earlier rounds; previous: the last suggestion.
    """
    messages = [
        {"role": "system", "content": SYSTEM_PROMPT % {"max_questions": max_questions}},
        {"role": "user", "content": "Inventory:\n\n" + inventory_text},
    ]
    if previous:
        messages.append({"role": "assistant", "content": json.dumps(previous)})
    if answers:
        qa = "\n\n".join("Q: %s\nA: %s" % qa for qa in answers)
        messages.append({"role": "user", "content":
                         "The owner answered your questions:\n\n" + qa +
                         "\n\nRevise the title, description, subjects and folder tags using these answers (the answers are "
                         "authoritative). Ask only new questions that are still worth asking, or none."})
    return _parse(client.chat(messages), folders)
