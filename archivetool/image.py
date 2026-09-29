"""Disc image building (genisoimage) and error correction (dvdisaster RS03)."""

import os
import shutil
import subprocess
import sys
import tempfile

MAX_VOLID_LEN = 32


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
    """Volume id from the ISO9660 primary volume descriptor of an image file or device."""
    with open(path, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
    if pvd[1:6] != b"CD001":
        return None
    return pvd[40:72].decode("ascii", "replace").strip() or None


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
