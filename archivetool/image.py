"""Disc image building (genisoimage, or udfmake for UDF 2.50) and error correction (dvdisaster RS03)."""

import os
import shutil
import subprocess
import sys
import tempfile

MAX_VOLID_LEN = 32


def volume_label(disc_id, text, filesystem):
    """The volume label: the disc id, then a space and ``text`` (usually the title) as far as it fits.

    The id always comes first and whole: tools identify a disc by the label's first word,
    and Joliet shows only 16 characters. Limits: 32 bytes of UTF-8 on the hybrid image
    (genisoimage's limit for ISO 9660 and its UDF), 126 characters on UDF 2.50 (63 with
    any character above U+00FF). Characters beyond U+FFFF (emoji) are dropped: UDF
    cannot store them and genisoimage rejects them. Commas are dropped for UDF 2.50,
    because makefs separates its options with them.
    """
    text = "".join(c for c in (text or "") if ord(c) <= 0xFFFF)
    if filesystem == "udf250":
        text = text.replace(",", "")
    text = " ".join(text.split())
    label = disc_id + (" " + text if text else "")
    if filesystem == "udf250":
        label = label[:63 if any(ord(c) > 0xFF for c in label) else 126]
    else:
        while len(label.encode("utf-8")) > MAX_VOLID_LEN:
            label = label[:-1]
    return (label if label.startswith(disc_id) else disc_id).rstrip()


def disc_id_from_label(label):
    """The disc id at the start of a volume label (older discs: the whole label)."""
    return label.split()[0] if label and label.split() else None


def require(*commands):
    missing = [c for c in commands if shutil.which(c) is None]
    if missing:
        raise SystemExit("Error: missing required tool(s): %s" % ", ".join(missing))


def _graft_escape(path):
    return path.replace("\\", "\\\\").replace("=", "\\=")


def _genisoimage(stage, volume_id, payload_dir=None, payload_files=None, extra=()):
    """Run genisoimage with the staged tag files as root and the payload under data/.

    payload_dir grafts a whole folder; payload_files is [(relative path, source path)]
    for a disc that holds only part of a folder (passed through -path-list).
    Returns stdout. The source files are never copied or modified.
    """
    if len(volume_id) > MAX_VOLID_LEN:
        raise SystemExit("Error: volume id %r is longer than %d characters" % (volume_id, MAX_VOLID_LEN))
    cmd = [
        "genisoimage", "-quiet",
        "-input-charset", "utf-8",   # source names are UTF-8; the default (locale or ISO-8859-1)
                                     # garbles every non-ASCII name in the Joliet and UDF trees
        "-udf", "-R", "-J", "-joliet-long",
        "-allow-lowercase", "-allow-multidot", "-allow-limited-size",
        "-iso-level", "3",
        "-V", volume_id,
        "-graft-points",
    ] + list(extra)
    path_list = None
    try:
        if payload_files is not None:
            fd, path_list = tempfile.mkstemp(prefix="pathlist-", suffix=".txt", dir=os.path.dirname(stage))
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                for rel, src in payload_files:
                    f.write("%s=%s\n" % (_graft_escape("data/" + rel), _graft_escape(src)))
            cmd += ["-path-list", path_list]
        cmd.append("/=" + _graft_escape(stage))
        if payload_dir is not None:
            cmd.append("data/=" + _graft_escape(payload_dir))
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    finally:
        if path_list:
            os.remove(path_list)
    # genisoimage always warns that level 3 + long names "does not conform to ISO-9660"
    noise = "Warning: creating filesystem that does not conform to ISO-9660."
    errors = "\n".join(l for l in proc.stderr.splitlines() if l.strip() != noise)
    if proc.returncode != 0:
        raise SystemExit("Error: genisoimage failed:\n" + errors)
    if errors:
        print(errors, file=sys.stderr)
    return proc.stdout


def build_iso(stage, out, volume_id, payload_dir=None, payload_files=None):
    """Hybrid ISO9660 (Rock Ridge + Joliet, level 3) + UDF image."""
    _genisoimage(stage, volume_id, payload_dir, payload_files, extra=["-o", out])


def print_size(stage, volume_id, payload_dir=None, payload_files=None):
    """Exact size in 2048-byte sectors of the image build_iso would write, without writing it."""
    out = _genisoimage(stage, volume_id, payload_dir, payload_files, extra=["-print-size"])
    return int(out.strip().splitlines()[-1])


# ---------------------------------------------------------------- UDF 2.50 (lib/udfmake)

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILESYSTEMS = {
    "hybrid": "ISO9660 level 3 + Rock Ridge + Joliet, UDF 1.02 bridge",
    "udf250": "UDF 2.50, BD-ROM layout with metadata partition (NetBSD makefs via udfmake)",
}
UDF_OPTIONS = "T=bdrom,v=2.50,V=2.50"


def find_udfmake(explicit=None):
    """Path of the udfmake program: --udfmake, $PATH, or lib/udfmake/build in this repository."""
    for candidate in (explicit, shutil.which("udfmake"),
                      os.path.join(REPO_ROOT, "lib", "udfmake", "build", "udfmake"),
                      os.path.join(REPO_ROOT, "lib", "udfmake", "build-static", "udfmake")):
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


def _udf_view(parent, stage, payload_dir=None, payload_files=None):
    """One folder of symlinks: the staged files at the top, the untouched source under
    data/ (udfmake -L follows them).

    A single folder because makefs -t udf mishandles several source folders (it opens
    every file relative to the first). The payload itself never contains symlinks
    (bag.scan_payload rejects them), so the only links followed are these.
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


def build_udf(stage, out, volume_id, payload_dir=None, payload_files=None, udfmake=None, disc_id=None):
    """UDF 2.50 image (BD-ROM layout) of the stage plus the payload under data/. Returns its sectors.

    volume_id is the label (logical volume identifier); disc_id, when given, also goes
    into the 32-byte primary volume identifier (otherwise udfmake puts a random number there).
    """
    if "," in volume_id or (disc_id and "," in disc_id):
        raise SystemExit("Error: a UDF volume label cannot contain commas (makefs option syntax)")
    tool = find_udfmake(udfmake)
    if not tool:
        raise SystemExit("Error: udfmake not found. Build it with 'make -C %s', put it on PATH, "
                         "or pass --udfmake PATH" % os.path.join(REPO_ROOT, "lib", "udfmake"))
    view = _udf_view(os.path.dirname(stage), stage, payload_dir, payload_files)
    try:
        if os.path.exists(out):
            os.remove(out)
        options = "%s,L=%s" % (UDF_OPTIONS, volume_id) + (",P=%s" % disc_id if disc_id else "")
        proc = subprocess.run([tool, "-L", "-o", options, out, view],
                              stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, errors="replace")
    finally:
        shutil.rmtree(view, ignore_errors=True)
    if proc.returncode != 0 or not os.path.exists(out):
        raise SystemExit("Error: udfmake failed:\n" + "\n".join(proc.stdout.splitlines()[-15:]))
    size = os.path.getsize(out)
    if size % 2048:
        raise SystemExit("Error: udfmake wrote %d bytes, not whole 2048-byte sectors" % size)
    return size // 2048


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
