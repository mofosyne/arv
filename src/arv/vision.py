"""Optional image understanding with a local vision model (``--vision``).

A few images per folder (and one frame per sampled video, when ffmpeg is
installed) are shown to a vision-capable model on the same machine. Each
returns a one-line caption and tags; per folder these are merged into a
caption and tags that feed the text model's description and become folder
tags you review.

Unlike the name-only inventory this sends file contents, so it only ever talks
to a loopback server: there is deliberately no remote override.

Thumbnails are made with Pillow or ffmpeg when available; otherwise small
JPEG/PNG/WebP/GIF files are sent as they are and other formats are skipped.
"""

import base64
import collections
import io
import json
import os
import re
import shutil
import subprocess

from . import llm

IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp", ".tif", ".tiff", ".heic"}
VIDEO_EXTS = {".mp4", ".mov", ".mkv", ".avi", ".m4v", ".mts", ".webm"}
PASSTHROUGH = {".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".png": "image/png", ".webp": "image/webp",
               ".gif": "image/gif"}
MAX_PASSTHROUGH = 4 * 1024 * 1024
MAX_SIDE = 768

PROMPT = """Describe this image for a personal archive catalogue. Only describe \
what is visible; do not guess names of people or places unless readable text \
in the image says so.

Answer in exactly two lines, like this example:
Caption: Two children building a sandcastle on a sunny beach.
Tags: beach, children, sandcastle, summer"""

# Text a small model may copy from the instructions instead of answering
ECHOES = ("two children building a sandcastle", "one short sentence", "short lowercase tags")


class VisionClient(llm.Client):
    """An llm.Client that refuses non-loopback servers, whatever the options say."""

    def __init__(self, url=None, model=None, timeout=600):
        super().__init__(url, model, allow_remote=False, timeout=timeout)


def _pillow_thumbnail(path):
    try:
        from PIL import Image, ImageOps
    except ImportError:
        return None
    try:
        with Image.open(path) as img:
            img = ImageOps.exif_transpose(img)
            img.thumbnail((MAX_SIDE, MAX_SIDE))
            out = io.BytesIO()
            img.convert("RGB").save(out, "JPEG", quality=85)
            return out.getvalue(), "image/jpeg"
    except Exception:
        return None


def _ffmpeg_frame(path, video):
    if not shutil.which("ffmpeg"):
        return None
    cmd = ["ffmpeg", "-v", "error"] + (["-ss", "5"] if video else []) + [
        "-i", path, "-frames:v", "1", "-vf", "scale='min(%d,iw)':-2" % MAX_SIDE, "-f", "image2pipe",
        "-vcodec", "mjpeg", "-"]
    try:
        proc = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0 or not proc.stdout:
        if video and "-ss" in cmd:  # short clip: take the first frame instead
            return _ffmpeg_frame_start(path)
        return None
    return proc.stdout, "image/jpeg"


def _ffmpeg_frame_start(path):
    proc = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-frames:v", "1", "-f", "image2pipe",
                           "-vcodec", "mjpeg", "-"], stdin=subprocess.DEVNULL, capture_output=True, timeout=120)
    return (proc.stdout, "image/jpeg") if proc.returncode == 0 and proc.stdout else None


def image_data(path):
    """(bytes, mime) small enough to send, or None if this file cannot be shown."""
    ext = os.path.splitext(path)[1].lower()
    if ext in VIDEO_EXTS:
        return _ffmpeg_frame(path, video=True)
    result = _pillow_thumbnail(path) or _ffmpeg_frame(path, video=False)
    if result:
        return result
    if ext in PASSTHROUGH and os.path.getsize(path) <= MAX_PASSTHROUGH:
        with open(path, "rb") as f:
            return f.read(), PASSTHROUGH[ext]
    return None


def sample(entries, per_folder=3, max_total=40, videos=True):
    """{folder: [entries]}: up to ``per_folder`` evenly spaced images (and videos) per folder."""
    exts = IMAGE_EXTS | (VIDEO_EXTS if videos else set())
    by_folder = collections.OrderedDict()
    for e in sorted(entries, key=lambda e: e.path):
        if os.path.splitext(e.path)[1].lower() in exts and e.size >= 1024:
            by_folder.setdefault(e.path.rsplit("/", 1)[0] if "/" in e.path else ".", []).append(e)
    picked, total = collections.OrderedDict(), 0
    for folder, items in by_folder.items():
        if total >= max_total:
            break
        n = min(per_folder, len(items), max_total - total)
        step = len(items) / n
        picked[folder] = [items[int(i * step)] for i in range(n)]
        total += n
    return picked


def look(client, data, mime):
    """Caption and tags for one image: {"caption": str, "tags": [...]}."""
    content = [{"type": "text", "text": PROMPT},
               {"type": "image_url", "image_url": {"url": "data:%s;base64,%s" % (mime, base64.b64encode(data).decode())}}]
    try:
        reply = client.chat([{"role": "user", "content": content}], temperature=0.1, json_mode=False)
    except llm.LLMError as err:
        if "image" in str(err).lower() or "multimodal" in str(err).lower():
            raise llm.LLMError("the model %s does not accept images (%s); choose a vision model with "
                               "--vision-model" % (client.model, err))
        raise
    return parse_image_reply(reply)


def analyse(client, root, entries, per_folder=3, max_total=40, progress=None):
    """Run the vision model over a sample. Returns {folder: {"caption": str, "tags": [..], "images": n}}."""
    results = collections.OrderedDict()
    picked = sample(entries, per_folder, max_total)
    count = sum(len(v) for v in picked.values())
    done = 0
    for folder, items in picked.items():
        captions, tags = [], collections.Counter()
        for e in items:
            done += 1
            if progress:
                progress("Looking at %d/%d: %s" % (done, count, e.path))
            got = image_data(os.path.join(root, e.path))
            if not got:
                continue
            one = look(client, *got)
            if one["caption"]:
                captions.append(one["caption"])
            tags.update(one["tags"])
        if captions or tags:
            results[folder] = {"caption": " / ".join(captions[:3]),
                               "tags": [t for t, _ in tags.most_common(6)],
                               "images": len(items)}
    return results


def parse_image_reply(content):
    """{"caption", "tags"} from a 'Caption: / Tags:' reply; JSON and bare text are accepted too."""
    caption, tags = "", []
    m_caption = re.search(r"caption\s*[:=]\s*(.+)", content, re.I)
    m_tags = re.search(r"tags?\s*[:=]\s*(.+)", content, re.I)
    if m_caption or m_tags:
        caption = m_caption.group(1) if m_caption else ""
        tags = re.split(r"[,;]", m_tags.group(1)) if m_tags else []
    else:
        start, end = content.find("{"), content.rfind("}")
        data = {}
        if start >= 0 and end > start:
            try:
                data = json.loads(content[start:end + 1])
            except json.JSONDecodeError:
                data = {}
        if data:
            caption, tags = str(data.get("caption") or ""), data.get("tags") or []
            if isinstance(tags, str):
                tags = re.split(r"[,;]", tags)
        elif not content.strip().startswith("{"):
            caption = content  # the model ignored the format: its text is still a caption
    caption = re.sub(r"\s+", " ", caption).strip().strip('"').strip()[:200]
    clean = []
    for t in tags:
        t = re.sub(r"\s+", " ", str(t)).strip().strip('".').lower()[:30]
        if t and t not in clean:
            clean.append(t)
    if any(echo in (caption + " " + " ".join(clean)).lower() for echo in ECHOES):
        return {"caption": "", "tags": []}
    return {"caption": caption, "tags": clean[:6]}


def inventory_section(results):
    """Text added to the inventory so the text model can use what the images show."""
    if not results:
        return ""
    lines = ["", "What sampled images show (from a local vision model; may be imperfect):"]
    for folder, r in results.items():
        lines.append("  %s/ (%d sampled): %s [%s]" % (folder, r["images"], r["caption"], ", ".join(r["tags"])))
    return "\n".join(lines)


def merge_tags(folder_tags, results, limit=8):
    """Add vision tags to the suggested folder tags (names first, then what the images show)."""
    merged = {k: list(v) for k, v in (folder_tags or {}).items()}
    for folder, r in results.items():
        tags = merged.setdefault(folder, [])
        for t in r["tags"]:
            if t not in tags and len(tags) < limit:
                tags.append(t)
    return merged
