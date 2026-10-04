"""Disc images (UDF 2.50 by src/udfwrite) and error correction (dvdisaster RS03)."""

import functools
import os
import shutil
import subprocess
import sys
import tempfile

MAX_VOLID_LEN = 32


def volume_label(disc_id, text):
    """The volume label: the disc id, then a space and ``text`` (usually the title) as far as it fits.

    The id always comes first and whole: tools identify a disc by the label's first word.
    UDF 2.50 holds 126 characters (63 with any character above U+00FF). Characters beyond
    U+FFFF (emoji) are dropped: UDF cannot store them. Commas are dropped too (they were
    option separators for the first UDF writer), so labels stay what they have been.
    """
    text = "".join(c for c in (text or "") if ord(c) <= 0xFFFF).replace(",", "")
    text = " ".join(text.split())
    label = disc_id + (" " + text if text else "")
    label = label[:63 if any(ord(c) > 0xFF for c in label) else 126]
    return (label if label.startswith(disc_id) else disc_id).rstrip()


def disc_id_from_label(label):
    """The disc id at the start of a volume label (older discs: the whole label)."""
    return label.split()[0] if label and label.split() else None


def require(*commands):
    missing = [c for c in commands if shutil.which(c) is None]
    if missing:
        raise SystemExit("Error: missing required tool(s): %s" % ", ".join(missing))


# ---------------------------------------------------------------- UDF 2.50 (src/udfwrite)

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # src/arv/ -> repository
# the one image arv makes (docs/archival-udf.md); older discs may also be iso9660+udf-1.02 (hybrid)
# or UDF 2.50 by NetBSD makefs, and stay readable
CONTAINER = "udf-2.50"   # Binding Container token
FILESYSTEM = "UDF 2.50, BD-ROM layout with metadata partition and a real mirror (arv udfwrite)"


def _udf_view(parent, stage, payload_dir=None, payload_files=None):
    """One folder of symlinks: the staged files at the top, the untouched source under
    data/ (udfwrite follows them).

    The writer takes one folder. Links in the payload never get here: bag.scan_payload
    turns them into files or listing rows, so the only links followed are these.
    """
    view = tempfile.mkdtemp(prefix="udfview-", dir=parent)
    for name in os.listdir(stage):
        os.symlink(os.path.abspath(os.path.join(stage, name)), os.path.join(view, name))
    data = os.path.join(view, "data")
    if payload_dir is not None and not payload_files:
        os.symlink(os.path.abspath(payload_dir), data)       # whole folder, empty folders included
        return view
    os.mkdir(data)
    if payload_dir is not None:
        for name in os.listdir(payload_dir):
            os.symlink(os.path.abspath(os.path.join(payload_dir, name)), os.path.join(data, name))
    for rel, src in payload_files or []:
        dest = os.path.join(data, rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        os.symlink(os.path.abspath(src), dest)
    return view


def find_udfwrite(explicit=None):
    """Path of arv's own UDF writer: --udfwrite, $PATH, or src/udfwrite/build in this repository."""
    for candidate in (explicit, shutil.which("udfwrite"),
                      os.path.join(REPO_ROOT, "src", "udfwrite", "build", "udfwrite")):
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


def build_udfwrite(stage, out, label, disc_id, volume_set, time, extents=None,
                   payload_dir=None, payload_files=None, tool=None):
    """UDF 2.50 image written by src/udfwrite (docs/archival-udf.md). Returns its sectors.

    Reproducible: the same stage, payload, ids and time give the same bytes. ``extents``
    receives each file's start sector and size (Binding data for the home catalogue).
    """
    tool = find_udfwrite(tool)
    if not tool:
        raise SystemExit("Error: udfwrite not found. Build it with 'make -C %s'"
                         % os.path.join(REPO_ROOT, "src", "udfwrite"))
    view = _udf_view(os.path.dirname(stage), stage, payload_dir, payload_files)
    try:
        if os.path.exists(out):
            os.remove(out)
        cmd = [tool, "-V", disc_id, "-L", label, "-S", volume_set, "-t", str(int(time))]
        if extents:
            cmd += ["-x", extents]
        proc = subprocess.run(cmd + [out, view], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True, errors="replace")
    finally:
        shutil.rmtree(view, ignore_errors=True)
    if proc.returncode != 0 or not os.path.exists(out):
        raise SystemExit("Error: udfwrite failed:\n" + "\n".join(proc.stdout.splitlines()[-15:]))
    return os.path.getsize(out) // 2048


def read_udf_volume_id(f):
    """Logical volume identifier of a UDF image (no ISO9660 descriptor on UDF-only discs)."""
    f.seek(256 * 2048)                                    # anchor volume descriptor pointer
    avdp = f.read(2048)
    if int.from_bytes(avdp[0:2], "little") != 2:
        return None
    length = int.from_bytes(avdp[16:20], "little")
    start = int.from_bytes(avdp[20:24], "little")
    for i in range(min(length // 2048, 64)):              # main volume descriptor sequence
        f.seek((start + i) * 2048)
        desc = f.read(2048)
        tag = int.from_bytes(desc[0:2], "little")
        if tag == 6:                                      # logical volume descriptor
            ident = desc[84:84 + 128]                     # dstring: compression id, chars, length
            n = ident[127]
            if ident[0] == 8:
                return ident[1:n].decode("latin-1").strip() or None
            if ident[0] == 16:
                return ident[1:n].decode("utf-16-be", "replace").strip() or None
            return None
        if tag == 8:                                      # terminating descriptor
            break
    return None


@functools.lru_cache(maxsize=None)
def dvdisaster_sets_medium_size():
    """True for dvdisaster builds (the speed47 fork) that honour -n <sectors> for RS03 images."""
    proc = subprocess.run(["dvdisaster", "--help"], stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return "no-bdr-defect-management" in proc.stdout


def add_ecc(image, medium_sectors=None):
    """Augment the image in place with RS03 error correction data. Returns dvdisaster's output.

    medium_sectors makes RS03 fill exactly that medium (speed47 fork; older builds
    ignore it and pick the smallest standard medium that fits).
    """
    cmd = ["dvdisaster", "-i", image, "-mRS03", "-o", "image", "-c", "--no-progress",
           "-x", str(os.cpu_count() or 1)]
    if medium_sectors and dvdisaster_sets_medium_size():
        cmd += ["-n", str(medium_sectors)]
    proc = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if proc.returncode != 0:
        raise SystemExit("Error: dvdisaster failed:\n" + proc.stdout)
    return proc.stdout


def verify_ecc(image):
    """Run ``dvdisaster -t``. Returns (ok, output)."""
    proc = subprocess.run(
        ["dvdisaster", "-i", image, "-t", "--no-progress"],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    out = proc.stdout
    ok = proc.returncode == 0 and "all sectors present" in out and "fail" not in out.lower()
    return ok, out


def read_volume_id(path):
    """Disc id of an image file or device: the first word of its volume label."""
    return disc_id_from_label(read_volume_label(path))


def read_volume_label(path):
    """Volume label of an image file or device: ISO9660 primary volume descriptor, else UDF."""
    with open(path, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        if pvd[1:6] == b"CD001":
            return pvd[40:72].decode("ascii", "replace").strip() or None
        return read_udf_volume_id(f)


def scan_device(device):
    """Run ``dvdisaster -s`` on a drive: reads every sector and checks it against the RS03 data.

    Returns (ok, output).
    """
    proc = subprocess.run(
        ["dvdisaster", "-d", device, "-s", "--no-progress"],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    return proc.returncode == 0, proc.stdout


def summary(output, lines=6):
    """The last few meaningful lines of dvdisaster output, for an Event note."""
    keep = [l.strip() for l in output.splitlines() if l.strip() and not l.startswith(("Copyright", "This software", "is free", "under the", "See the file", "dvdisaster "))]
    return "\n".join(keep[-lines:])
