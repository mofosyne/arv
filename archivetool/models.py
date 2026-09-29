"""Built-in AI tier: a pinned embedding model run by llama.cpp's command-line tool.

Nothing here is needed for archiving. It backs `archive tag`:

- Models are pinned to an exact Hugging Face revision and SHA-256, downloaded
  once into <home>/models/, never committed to the repository (every disc
  carries the repository's history, so weights would ride along forever).
- The runtime is llama.cpp's `llama-embedding` program, run as a plain
  subprocess (texts in, vectors out): no server, no port, no API. It is taken
  from --llama-embedding / $ARCHIVE_LLAMA_EMBEDDING, <home>/runtime/, or PATH;
  `archive models build-runtime` compiles it from source (needs git, cmake and
  a C++ compiler).
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

MODELS = {
    "bge-small-en-v1.5": {
        "file": "bge-small-en-v1.5-q8_0.gguf",
        "url": "https://huggingface.co/CompendiumLabs/bge-small-en-v1.5-gguf/resolve/"
               "d32f8c040ea3b516330eeb75b72bcc2d3a780ab7/bge-small-en-v1.5-q8_0.gguf",
        "sha256": "ec38e8da142596baa913124ae50550de284b6916bf59577ef2f0cb9660c2f514",
        "size": 36806944,
        "licence": "MIT (BAAI/bge-small-en-v1.5; GGUF by CompendiumLabs)",
        "kind": "embedding",
        # bge v1.5 expects this instruction on the short "query" side (here: tag descriptions)
        "query_prefix": "Represent this sentence for searching relevant passages: ",
    },
}
DEFAULT_EMBEDDING = "bge-small-en-v1.5"
LLAMA_CPP_REPO = "https://github.com/ggml-org/llama.cpp.git"


class ModelError(Exception):
    pass


def log(msg):
    print(msg, file=sys.stderr)


def models_dir(home):
    return os.path.join(home.path, "models")


def model_path(home, name):
    return os.path.join(models_dir(home), MODELS[name]["file"])


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(home, name, url=None):
    """Download a pinned model (or verify it if present). Returns its path."""
    spec = MODELS[name]
    dest = model_path(home, name)
    if os.path.exists(dest):
        if sha256_of(dest) == spec["sha256"]:
            return dest
        log("Existing %s does not match its pinned checksum; downloading again." % dest)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(dest), suffix=".part")
    try:
        log("Downloading %s (%.1f MB, %s) ..." % (name, spec["size"] / 1e6, spec["licence"]))
        h = hashlib.sha256()
        with os.fdopen(fd, "wb") as out, urllib.request.urlopen(url or spec["url"], timeout=120) as resp:
            for chunk in iter(lambda: resp.read(1 << 20), b""):
                out.write(chunk)
                h.update(chunk)
        if h.hexdigest() != spec["sha256"]:
            raise ModelError("checksum mismatch for %s: got %s, expected %s" % (name, h.hexdigest(), spec["sha256"]))
        os.replace(tmp, dest)
        return dest
    except (urllib.error.URLError, OSError) as err:
        raise ModelError("download of %s failed: %s" % (name, err))
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def install_model_file(home, name, source):
    """Use a model file you already have (e.g. copied from a disc's tools/extra/), after verifying it."""
    spec = MODELS[name]
    if sha256_of(source) != spec["sha256"]:
        raise ModelError("%s is not the pinned %s (checksum differs)" % (source, name))
    dest = model_path(home, name)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.copyfile(source, dest)
    return dest


# ---------------------------------------------------------------- runtime

RUNTIME = "llama-embedding"
SEPARATOR = "<#archive-sep#>"


def find_runtime(home, explicit=None):
    candidates = [explicit, os.environ.get("ARCHIVE_LLAMA_EMBEDDING"),
                  os.path.join(home.path, "runtime", RUNTIME), shutil.which(RUNTIME)]
    for c in candidates:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


def build_runtime(home, ref="master"):
    """Clone and build llama-embedding into <home>/runtime/ (portable build, no CPU-specific flags)."""
    for tool in ("git", "cmake"):
        if not shutil.which(tool):
            raise ModelError("building %s needs %s" % (RUNTIME, tool))
    work = os.path.join(home.path, "runtime", "llama.cpp")
    if not os.path.isdir(work):
        subprocess.run(["git", "clone", "--depth", "1", "--branch", ref, LLAMA_CPP_REPO, work], check=True)
    build = os.path.join(work, "build")
    # GGML_NATIVE=OFF: a binary tuned for this exact CPU crashes ("Illegal instruction") on others
    subprocess.run(["cmake", "-S", work, "-B", build, "-DGGML_NATIVE=OFF", "-DLLAMA_CURL=OFF",
                    "-DCMAKE_BUILD_TYPE=Release"], check=True)
    subprocess.run(["cmake", "--build", build, "--target", RUNTIME, "-j", str(os.cpu_count() or 2)], check=True)
    binary = os.path.join(build, "bin", RUNTIME)
    link = os.path.join(home.path, "runtime", RUNTIME)
    if os.path.lexists(link):
        os.remove(link)
    os.symlink(binary, link)
    return link


def embed(binary, model, texts, batch=64):
    """L2-normalised embedding vectors for ``texts`` (so a dot product is the cosine similarity)."""
    vectors = []
    for i in range(0, len(texts), batch):
        chunk = [t.replace(SEPARATOR, " ").replace("\x00", " ").strip() or "(empty)" for t in texts[i:i + batch]]
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".txt", delete=False) as f:
            f.write(SEPARATOR.join(chunk))
            prompt_file = f.name
        try:
            proc = subprocess.run(
                [binary, "-m", model, "-f", prompt_file, "--embd-separator", SEPARATOR,
                 "--embd-output-format", "array", "--embd-normalize", "2", "--pooling", "cls",
                 "-t", str(os.cpu_count() or 2), "-c", "512", "-b", "512", "-ub", "512"],
                stdin=subprocess.DEVNULL, capture_output=True, text=True)
        finally:
            os.remove(prompt_file)
        start = proc.stdout.find("[[")
        if proc.returncode != 0 or start < 0:
            raise ModelError("%s failed (%s): %s" % (RUNTIME, proc.returncode, (proc.stderr or proc.stdout)[-600:]))
        got = json.loads(proc.stdout[start:proc.stdout.rindex("]]") + 2])
        if len(got) != len(chunk):
            raise ModelError("%s returned %d vectors for %d texts" % (RUNTIME, len(got), len(chunk)))
        vectors += got
    return vectors
