"""Disc image building (genisoimage) and error correction (dvdisaster RS03)."""

import shutil
import subprocess
import sys

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
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    # genisoimage always warns that level 3 + long names "does not conform to ISO-9660"
    noise = "Warning: creating filesystem that does not conform to ISO-9660."
    output = "\n".join(l for l in proc.stdout.splitlines() if l.strip() != noise)
    if proc.returncode != 0:
        raise SystemExit("Error: genisoimage failed:\n" + output)
    if output:
        print(output, file=sys.stderr)


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
