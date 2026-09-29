"""`archive tag`: consistent folder tags from your own tag vocabulary, using a
small built-in embedding model (see models.py). No server or API.

How it works:
- Every tag in <home>/tags.rec has a Description. Each folder gets a short
  summary (its path, sample file names, README text, image captions if any).
- The model turns both into vectors; tags whose descriptions are closest to a
  folder's summary are suggested.
- Tags you accept are remembered (<home>/tag-examples.jsonl). New folders that
  look like folders you tagged before get those tags suggested too, so the
  suggestions follow your own habits without any training.

Tags are suggestions: you review them (or they are marked as unreviewed).
"""

import collections
import json
import os
import re
import shutil

from . import catalog, models, recfile

DEFAULT_VOCAB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "default_tags.rec")
README_NAMES = ("readme", "notes", "about", "description", "info")
EXAMPLE_MIN = 0.75     # tuned on bge-small: similar trip folders ~0.78-0.80, unrelated 0.4-0.71
EXAMPLE_SHIFT = 0.2


class TagError(Exception):
    pass


def vocab_path(home):
    return os.path.join(home.path, "tags.rec")


def load_vocab(home, path=None):
    """[(name, description)]; creates <home>/tags.rec from the default on first use."""
    if not path:
        path = vocab_path(home)
        if not os.path.exists(path):
            os.makedirs(home.path, exist_ok=True)
            shutil.copyfile(DEFAULT_VOCAB, path)
    vocab = [(r.get("Name").strip().lower(), r.get("Description") or r.get("Name"))
             for r in recfile.read(path) if not r.is_descriptor and r.get("Name")]
    if not vocab:
        raise TagError("no tags in %s" % path)
    return vocab


def folder_key(path):
    parent = path.rsplit("/", 1)[0] if "/" in path else "."
    return "/".join(parent.split("/")[:3])


KINDS = {
    "photos": (".jpg", ".jpeg", ".png", ".heic", ".tif", ".tiff", ".raw", ".cr2", ".nef", ".arw", ".dng", ".webp", ".gif", ".bmp"),
    "videos": (".mp4", ".mov", ".mkv", ".avi", ".m4v", ".mts", ".webm", ".wmv"),
    "audio recordings": (".mp3", ".flac", ".wav", ".ogg", ".m4a", ".aac"),
    "PDF documents": (".pdf",),
    "text documents": (".doc", ".docx", ".odt", ".txt", ".md", ".rtf"),
    "spreadsheets": (".xls", ".xlsx", ".ods", ".csv"),
    "source code files": (".c", ".cpp", ".h", ".py", ".js", ".ts", ".rs", ".go", ".java", ".ino", ".sh", ".ini", ".toml"),
    "archives": (".zip", ".tar", ".gz", ".7z", ".rar", ".iso", ".img", ".bundle"),
}
# Camera / phone default names carry no meaning: DSC_4001.JPG, IMG_20211221_1830.jpg, GH010042.MP4 ...
GENERIC_NAME = re.compile(r"^(dsc|dscn|img|pxl|mvi|vid|gopr|gh\d|gx\d|dji|p\d|sam|photo|image|scan)[_-]?\d", re.I)


def _words(text):
    return re.sub(r"[_\-.]+", " ", text).strip()


def summaries(entries, text_root=None, captions=None, samples=8):
    """{folder: text} for every folder (grouped to depth 3), as the model will see it.

    Written as plain words ("photos, 2018 07 Kyoto: 12 photos") rather than a file listing,
    and without camera-style names (DSC_4001.JPG), which only add noise.
    """
    captions = captions or {}
    groups = collections.OrderedDict()
    for e in sorted(entries, key=lambda e: e.path):
        groups.setdefault(folder_key(e.path), []).append(e)
    out = collections.OrderedDict()
    for folder, items in groups.items():
        kinds = collections.Counter()
        names = []
        for e in items:
            base, ext = os.path.splitext(e.path.rsplit("/", 1)[-1])
            kinds[next((k for k, exts in KINDS.items() if ext.lower() in exts), "files")] += 1
            if not GENERIC_NAME.match(base) and len(names) < samples:
                names.append(_words(base))
        text = ", ".join(_words(p) for p in folder.split("/")) + ": "
        text += ", ".join("%d %s" % (n, k) for k, n in kinds.most_common(3))
        if names:
            text += ". Named: " + "; ".join(names)
        if text_root:
            for e in items:
                base = e.path.rsplit("/", 1)[-1].lower()
                if base.split(".")[0] in README_NAMES and e.size < 256 * 1024:
                    try:
                        with open(os.path.join(text_root, e.path), encoding="utf-8", errors="replace") as f:
                            text += ". " + " ".join(f.read(400).replace("#", " ").split())
                    except OSError:
                        pass
                    break
        if captions.get(folder):
            text += ". Images show: " + captions[folder]
        out[folder] = text
    return out


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


class Tagger:
    """Embeds with the built-in llama-embedding program, or with an OpenAI-compatible
    /v1/embeddings server when ``embed_url`` is given (a fallback engine if llama.cpp is
    ever unavailable). Examples learned from reviews are kept per model, because vectors
    from different models cannot be compared."""

    def __init__(self, home, binary=None, model_name=models.DEFAULT_EMBEDDING, vocab_file=None,
                 embed_url=None, embed_model=None, query_prefix=None, allow_remote=False):
        self.home = home
        self.vocab = load_vocab(home, vocab_file)
        self.examples_path = os.path.join(home.path, "tag-examples.jsonl")
        self.client = None
        if embed_url:
            from . import llm
            try:
                self.client = llm.Client(embed_url, embed_model, allow_remote)
                self.model_name = self.client.resolve_model()
            except llm.LLMError as err:
                raise TagError(str(err))
            known = next((spec for name, spec in models.MODELS.items() if name in self.model_name), {})
            self.query_prefix = query_prefix if query_prefix is not None else known.get("query_prefix", "")
            return
        self.model_name = model_name
        self.query_prefix = query_prefix if query_prefix is not None else models.MODELS[model_name].get("query_prefix", "")
        self.binary = models.find_runtime(home, binary)
        if not self.binary:
            raise TagError("llama.cpp's llama-embedding was not found. Install llama.cpp (it is in Homebrew and "
                           "many Linux distributions), pass --llama-embedding PATH, run "
                           "'archive models build-runtime', or use an embeddings server with --embed-url")
        self.model = models.model_path(home, model_name)
        if not os.path.exists(self.model):
            raise TagError("model %s is not downloaded yet: run 'archive models fetch'" % model_name)

    @property
    def agent(self):
        return "embeddings:%s" % self.model_name

    def embed(self, texts):
        if not self.client:
            return models.embed(self.binary, self.model, texts)
        import math
        from . import llm
        out = []
        for i in range(0, len(texts), 64):
            try:
                reply = self.client._request("/embeddings", {"model": self.model_name, "input": texts[i:i + 64]})
            except llm.LLMError as err:
                raise TagError(str(err))
            for item in sorted(reply["data"], key=lambda d: d.get("index", 0)):
                vec = item["embedding"]
                norm = math.sqrt(sum(x * x for x in vec)) or 1.0
                out.append([x / norm for x in vec])  # normalise so dot product = cosine
        return out

    def examples(self):
        out = []
        if os.path.exists(self.examples_path):
            with open(self.examples_path, encoding="utf-8") as f:
                for line in f:
                    ex = json.loads(line)
                    if ex.get("model") == self.model_name:
                        out.append(ex)
        return out

    def suggest(self, folder_texts, top=3, margin=0.05):
        """{folder: [(tag, score), ...]} best first, at most ``top`` within ``margin`` of the best."""
        if not folder_texts:
            return {}
        names = [n for n, _ in self.vocab]
        vectors = self.embed([self.query_prefix + d for _, d in self.vocab] + list(folder_texts.values()))
        tag_vecs, folder_vecs = vectors[:len(names)], vectors[len(names):]
        examples = self.examples()
        out = {}
        for folder, fv in zip(folder_texts, folder_vecs):
            scores = {n: dot(fv, tv) for n, tv in zip(names, tag_vecs)}
            # Learn from past reviews: a folder very similar to one you tagged earlier inherits those
            # tags. Folder-to-folder similarities run higher than folder-to-description ones, so they
            # count only above EXAMPLE_MIN and are shifted down onto the description scale.
            for ex in examples:
                sim = dot(fv, ex["vector"])
                if sim < EXAMPLE_MIN:
                    continue
                for t in ex["tags"]:
                    scores[t] = max(scores.get(t, -1), sim - EXAMPLE_SHIFT)
            ranked = sorted(scores.items(), key=lambda kv: -kv[1])
            best = ranked[0][1]
            out[folder] = [(t, round(s, 3)) for t, s in ranked[:top] if s >= best - margin]
        return out

    def remember(self, folder_texts, accepted):
        """Store the reviewed tags so future suggestions follow them."""
        items = [(f, folder_texts[f], tags) for f, tags in accepted.items() if tags and f in folder_texts]
        if not items:
            return
        vectors = self.embed([text for _, text, _ in items])
        with open(self.examples_path, "a", encoding="utf-8") as f:
            for (folder, text, tags), vec in zip(items, vectors):
                f.write(json.dumps({"model": self.model_name, "folder": folder, "text": text, "tags": tags,
                                    "vector": [round(x, 5) for x in vec]}) + "\n")


def merge_into_tags_file(home, disc_id, accepted):
    """Replace the tags of the reviewed folders in the disc's tags file, keeping image captions."""
    path = home.disc_file("tags", disc_id)
    info = catalog.read_tag_info(path) if os.path.exists(path) else {}
    tags = {f: t for f, (t, _) in info.items()}
    captions = {f: c for f, (_, c) in info.items() if c}
    tags.update({f: t for f, t in accepted.items() if t})
    os.makedirs(os.path.dirname(path), exist_ok=True)
    catalog.write_tags(path, tags, captions)
    return path


# ---------------------------------------------------------------- command line

def _log(msg=""):
    import sys
    print(msg, file=sys.stderr)


def review(suggestions, interactive, ask=input):
    """Show suggestions; in a terminal let the owner accept or edit each folder. Returns {folder: [tags]}."""
    accepted = {}
    if not interactive:
        return {f: [t for t, _ in s] for f, s in suggestions.items()}
    _log("Suggested tags. [Enter] accept, type tags (comma separated) to replace, '-' for none, 'a' to accept all the rest.")
    accept_rest = False
    for folder, ranked in suggestions.items():
        tags = [t for t, _ in ranked]
        if accept_rest:
            accepted[folder] = tags
            continue
        _log("")
        _log("  %s/" % folder)
        _log("    " + ", ".join("%s (%.2f)" % (t, s) for t, s in ranked))
        try:
            answer = ask("  > ").strip()
        except EOFError:
            answer = ""
        if answer == "a":
            accept_rest = True
            accepted[folder] = tags
        elif answer == "-":
            accepted[folder] = []
        elif answer:
            accepted[folder] = [t.strip().lower() for t in answer.split(",") if t.strip()]
        else:
            accepted[folder] = tags
    return accepted


def run(args):
    """`archive tag <folder|disc-id>`"""
    import sys
    from . import describe
    home = catalog.Home(args.home)
    cat = home.load()
    disc = cat.disc(args.target)
    captions = {}
    if disc:
        entries = describe.disc_entries(home, disc.get("Id"))
        text_root = os.path.join(args.disc_root, "data") if args.disc_root else None
        tags_file = home.disc_file("tags", disc.get("Id"))
        if os.path.exists(tags_file):
            captions = {f: c for f, (_, c) in catalog.read_tag_info(tags_file).items() if c}
    elif os.path.isdir(args.target):
        entries = describe.folder_entries(os.path.abspath(args.target))
        text_root = os.path.abspath(args.target)
    else:
        raise SystemExit("Error: %s is neither a disc id in the catalogue nor a folder" % args.target)

    try:
        if args.model not in models.MODELS:
            raise TagError("unknown model %r (known: %s)" % (args.model, ", ".join(models.MODELS)))
        tagger = Tagger(home, args.llama_embedding, args.model, vocab_file=args.vocab, embed_url=args.embed_url,
                        embed_model=args.embed_model, allow_remote=args.llm_allow_remote)
        texts = summaries(entries, text_root, captions)
        if args.show_summaries:
            for folder, text in texts.items():
                print("%s\n    %s" % (folder, text))
            return 0
        suggestions = tagger.suggest(texts, top=args.top)
    except (TagError, models.ModelError) as err:
        raise SystemExit("Error: %s" % err)

    interactive = sys.stdin.isatty()
    accepted = review(suggestions, interactive)
    reviewed = interactive
    if interactive:
        tagger.remember(texts, accepted)
    agent = "%s%s" % (tagger.agent, " + owner review" if reviewed else " (unreviewed)")

    if args.save:
        draft = {"folder_tags": {f: t for f, t in accepted.items() if t}, "agent": agent}
        if os.path.exists(args.save):  # merge into an existing draft (e.g. from `archive describe`)
            with open(args.save, encoding="utf-8") as f:
                old = json.load(f)
            old.setdefault("folder_tags", {}).update(draft["folder_tags"])
            old["agent"] = "%s; %s" % (old.get("agent", "draft"), agent)
            draft = old
        with open(args.save, "w", encoding="utf-8") as f:
            json.dump(draft, f, ensure_ascii=False, indent=2)
            f.write("\n")
        _log("Saved to %s (use: archive make --draft %s ...)" % (args.save, args.save))
    elif disc and (args.apply or interactive):
        merge_into_tags_file(home, disc.get("Id"), accepted)
        cat.events.append(catalog.new_event(disc.get("Id"), "metadata modification", "success", agent,
                                            "folder tags for %d folders from the tag vocabulary"
                                            % sum(1 for t in accepted.values() if t)))
        home.save(cat)
        _log("Updated tags of %s." % disc.get("Id"))
    else:
        print(json.dumps({"folder_tags": accepted, "agent": agent}, ensure_ascii=False, indent=2))
    return 0


def models_command(args):
    """`archive models fetch|status|build-runtime`"""
    home = catalog.Home(args.home)
    try:
        if args.action == "fetch":
            path = (models.install_model_file(home, args.model, args.from_file) if args.from_file
                    else models.fetch(home, args.model))
            print("%s ready: %s" % (args.model, path))
        elif args.action == "build-runtime":
            print("Built %s" % models.build_runtime(home))
        runtime = models.find_runtime(home, getattr(args, "llama_embedding", None))
        if args.action == "status" or args.action == "fetch":
            for name, spec in models.MODELS.items():
                path = models.model_path(home, name)
                state = "downloaded" if os.path.exists(path) else "not downloaded (archive models fetch)"
                print("%-20s %-10s %6.1f MB  %s  [%s]" % (name, spec["kind"], spec["size"] / 1e6, state, spec["licence"]))
            print("%-20s %s" % ("runtime", runtime or "llama-embedding not found (install llama.cpp, or "
                                                        "archive models build-runtime)"))
    except (models.ModelError, OSError) as err:
        raise SystemExit("Error: %s" % err)
    return 0
