"""Disc image building (genisoimage) and error correction (dvdisaster RS03)."""

import shutil
import subprocess

MAX_VOLID_LEN = 32


def require(*commands):
    missing = [c for c in commands if shutil.which(c) is None]
    if missing:
        raise SystemExit("Error: missing required tool(s): %s" % ", ".join(missing))


def _graft_escape(path):
    return path.replace("\\", "\\\\").replace("=", "\\=")


def build_iso(stage, payload, out, volume_id):
    """Hybrid ISO9660 (Rock Ridge + Joliet, level 3) + UDF image.

    The staged tag files become the image root and ``payload`` is grafted in as
    ``data/``, so the source folder is never copied or modified.
    """
    if len(volume_id) > MAX_VOLID_LEN:
        raise SystemExit("Error: volume id %r is longer than %d characters" % (volume_id, MAX_VOLID_LEN))
    cmd = [
        "genisoimage", "-quiet",
        "-udf", "-R", "-J", "-joliet-long",
        "-allow-lowercase", "-allow-multidot", "-allow-limited-size",
        "-iso-level", "3",
        "-V", volume_id,
        "-o", out,
        "-graft-points",
        "/=" + _graft_escape(stage),
        "data/=" + _graft_escape(payload),
    ]
    subprocess.run(cmd, check=True)


def add_ecc(image):
    """Augment the image in place with RS03 error correction data."""
    subprocess.run(
        ["dvdisaster", "-i", image, "-mRS03", "-o", "image", "-c", "--no-progress"],
        check=True, stdin=subprocess.DEVNULL,
    )


def verify_ecc(image):
    """Run ``dvdisaster -t``. Returns (ok, output)."""
    proc = subprocess.run(
        ["dvdisaster", "-i", image, "-t", "--no-progress"],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    out = proc.stdout
    ok = proc.returncode == 0 and "all sectors present" in out and "fail" not in out.lower()
    return ok, out
