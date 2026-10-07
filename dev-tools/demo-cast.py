#!/usr/bin/env python3
"""Records docs/demo/arv.cast: a typical arv session (Archive, Record, Verify) as an asciinema
recording (asciicast v2), made by running arv for real, so it can be retaken after a change:

    python3 dev-tools/demo-cast.py [OUT.cast]

Play it with `asciinema play docs/demo/arv.cast`, or on the website (docs/index.html embeds it
with asciinema-player); `asciinema upload docs/demo/arv.cast` puts it on asciinema.org.

Fixed: the clock (SOURCE_DATE_EPOCH), the user, the sample files and their dates, and tools/ (a
small stand-in for arv's source, so the images stay small and quick to make). The session runs in
a temporary folder shown as ~. Needs src/arv/build/arv (make).
"""

import json
import os
import random
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARV = os.path.join(REPO, "src", "arv", "build", "arv")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "docs", "demo", "arv.cast")
WIDTH, HEIGHT = 112, 32
TYPE, AFTER, LINE = 0.045, 1.6, 0.012          # seconds: a typed character, a pause, an output line

# The session: ("#", text) is a comment typed at the prompt; ("$", command) is run; ("pause", s).
# {DISC} becomes the id of the disc the plan made.
SESSION = [
    ("#", "Archive: what goes on discs"),
    ("$", "arv init"),
    ("$", 'arv plan new kyoto --title "Kyoto, July 2025" --set TRIP'),
    ("$", "arv plan add kyoto Videos/kyoto-walk.mp4"),
    ("$", "arv plan add kyoto /media/sdcard/DCIM --copy   # the card won't be here later"),
    ("$", "arv plan show kyoto"),
    ("#", "a small test medium here; real discs are --medium bd25 or bd100"),
    ("$", "arv plan make kyoto -y --medium-sectors 9000 --output-dir images"),
    ("$", "rm -r /media/sdcard/DCIM   # wiped for the next trip; the plan kept a copy"),
    ("pause", 1.0),
    ("#", "Record: what exists, and where"),
    ("$", 'arv location add SHELF "Study shelf" --temperature cold'),
    ("$", 'arv location add NAS "The NAS" --temperature warm'),
    ("$", "arv burned {DISC} --copies 1 --location SHELF   # --device also reads it back"),
    ("$", "arv stored {DISC} images/{DISC}.iso --location NAS"),
    ("$", "arv objects"),
    ("$", "arv find kinkaku"),
    ("pause", 1.0),
    ("#", "Verify: still good, and can be got back"),
    ("$", "arv todo"),
    ("$", "arv check --image images/{DISC}.iso"),
    ("$", "arv status Videos"),
    ("pause", 2.5),
]


def sample_files(root):
    """A video, and a camera card's photos, with July 2025 dates."""
    rnd = random.Random(2025)
    when = 1751600000                               # 2025-07-04

    def write(path, size, t):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(bytes(rnd.getrandbits(8) for _ in range(size)))
        os.utime(path, (t, t))

    write(os.path.join(root, "Videos", "kyoto-walk.mp4"), 900_000, when)
    card = os.path.join(root, "media", "sdcard", "DCIM")
    for i, name in enumerate(["IMG_0101_fushimi-inari.JPG", "IMG_0102_kinkaku-ji.JPG", "IMG_0103_arashiyama.JPG",
                              "IMG_0104_gion.JPG"]):
        write(os.path.join(card, name), 120_000, when + 3600 * (i + 1))
    for d in (card, os.path.dirname(card)):
        os.utime(d, (when, when))


def stand_in_tools(root):
    """tools/: a small stand-in for arv's source (the real one is about 80 MB)."""
    src = os.path.join(root, "arv-source")
    os.makedirs(os.path.join(src, "src", "arv"))
    with open(os.path.join(src, "VERSION"), "w") as f:
        f.write("arv@demo\n")
    with open(os.path.join(src, "README.md"), "w") as f:
        f.write("arv (stand-in for tools/ in this demo)\n")
    with open(os.path.join(src, "src", "arv", "arv.c"), "w") as f:
        f.write("/* arv.c stand-in */\n")
    return src


def main():
    if not os.access(ARV, os.X_OK):
        sys.exit("demo-cast.py: build arv first (make -C src/arv)")
    work = tempfile.mkdtemp(prefix="arv-demo-")
    root = os.path.join(work, "home")                # shown as ~
    os.makedirs(root)
    try:
        sample_files(root)
        os.makedirs(os.path.join(root, "bin"))
        os.symlink(ARV, os.path.join(root, "bin", "arv"))
        env = {"PATH": os.path.join(root, "bin") + ":/usr/bin:/bin", "HOME": root, "USER": "archivist",
               "LOGNAME": "archivist", "TZ": "UTC", "LC_ALL": "C", "SOURCE_DATE_EPOCH": "1754006400",   # 2025-08-01
               "ARV_SOURCE": stand_in_tools(work), "ARV_APE": "none", "XDG_CONFIG_HOME": os.path.join(work, "cfg"),
               "XDG_DATA_HOME": os.path.join(work, "data")}
        # /media/sdcard is the sample card inside ~ (shown as it would be on a real machine)
        shown = lambda text: (text.replace(os.path.join(root, "media"), "/media").replace("'" + root + "/", "~/'")
                              .replace(root, "~").replace(work, "/tmp"))
        real = lambda cmd: cmd.replace("/media/", os.path.join(root, "media") + "/")
        events, t, disc = [], 0.5, None

        def emit(text):
            events.append([round(t, 3), "o", text])

        def prompt():
            emit("\x1b[1;32marchivist\x1b[0m:\x1b[1;34m~\x1b[0m$ ")

        def type_(text):
            nonlocal t
            for ch in text:
                emit(ch)
                t += TYPE
            emit("\r\n")
            t += 0.15

        for kind, text in SESSION:
            if kind == "pause":
                t += text
                continue
            prompt()
            t += 0.4
            if kind == "#":
                emit("\x1b[2m")
                type_("# " + text)
                emit("\x1b[0m")
                t += 0.6
                continue
            line = text.replace("{DISC}", disc or "?")
            type_(line)
            command = line.split("   #")[0]
            proc = subprocess.run(["sh", "-c", real(command)], cwd=root, env=env, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True)
            if command.startswith("arv plan make") and proc.returncode == 0:
                disc = [l.split("\t")[0] for l in proc.stdout.splitlines() if "\t" in l][0]
            elif proc.returncode:
                sys.exit("demo-cast.py: %s failed:\n%s" % (command, proc.stdout))
            for out in proc.stdout.splitlines():
                emit(shown(out).replace("\t", "  ") + "\r\n")
                t += LINE
            t += AFTER
        header = {"version": 2, "width": WIDTH, "height": HEIGHT, "timestamp": 1754006400,
                  "title": "arv: Archive, Record, Verify", "env": {"SHELL": "/bin/sh", "TERM": "xterm-256color"}}
        os.makedirs(os.path.dirname(os.path.abspath(OUT)), exist_ok=True)
        with open(OUT, "w") as f:
            f.write(json.dumps(header) + "\n")
            for e in events:
                f.write(json.dumps(e, ensure_ascii=False) + "\n")
        print("wrote %s (%.0f s)" % (OUT, t))
    finally:
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
