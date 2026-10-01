#!/usr/bin/env python3
"""RS03 tool experiments behind docs/research-notes.md ("RS03 tools compared").

    scripts/research/rs03/build-tools.sh /tmp/rs03-tools
    scripts/research/rs03/experiments.py /tmp/rs03-tools [--big]

Compares dvdisaster (speed47 fork), dvdisaster Light and LCSAS's lcsas-ecc (native and
WASI) on the sample discs in samples/discs/: verifying, re-creating RS03 bit for bit,
repair limits, damage to the RS03 bookkeeping sectors, recovering from two damaged copies,
and dvdisaster Light's rescue reading. --big adds a time and memory test on a 1 GiB image.
Prints Markdown. Standard library only; randomness is seeded, so runs repeat.
"""

import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
SAMPLES = os.path.join(REPO, "samples", "discs")
SECTOR = 2048
MEDIUM = 3200          # the sample discs' custom medium (samples/make-samples.sh)
DEAD = b"dvdisaster dead sector marker"

T = sys.argv[1] if len(sys.argv) > 1 else "/tmp/rs03-tools"
SPEED47 = os.path.join(T, "speed47", "dvdisaster")
LIGHT = os.path.join(T, "light", "dvdisaster")
LCSAS = os.path.join(T, "lcsas-ecc")
WASM = os.path.join(T, "lcsas-ecc.wasm")
RUN_WASI = os.path.join(REPO, "lib", "udfmake", "wasi", "run.mjs")
HAVE_WASM = os.path.exists(WASM) and shutil.which("node") is not None
WORK = tempfile.mkdtemp(prefix="rs03-")


def run(cmd, timeout=1800):
    p = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def same(a, b):
    with open(a, "rb") as fa, open(b, "rb") as fb:
        while True:
            x, y = fa.read(1 << 20), fb.read(1 << 20)
            if x != y:
                return False
            if not x:
                return True


def info(image):
    _, out = run([LCSAS, "info", image])
    get = lambda k: int(re.search(k + r"\s*:\s*(\d+)", out).group(1))
    return {"data": get("data sectors"), "nroots": get("nroots"), "spl": get("sectors per layer"),
            "total": get("total sectors"), "header": get("ecc header pos"), "crc": get("first CRC sector"),
            "ecc": get("first ECC sector"),
            "redundancy": re.search(r"redundancy\s*:\s*([\d.]+%)", out).group(1)}


# --- repairing a copy with each tool; True when the result is byte-identical to the original ---

def fix_dvdisaster(binary):
    def fix(image, original):
        run([binary, "-i", image, "-f", "--no-progress"])
        return same(image, original)
    return fix


def fix_lcsas(image, original):
    run([LCSAS, "fix", image])
    return same(image, original)


def fix_wasm(image, original):
    run(["node", "--no-warnings", RUN_WASI, WASM, "fix", os.path.abspath(image)])
    return same(image, original)


FIXERS = [("dvdisaster (speed47)", fix_dvdisaster(SPEED47)), ("dvdisaster Light", fix_dvdisaster(LIGHT)),
          ("lcsas-ecc", fix_lcsas)] + ([("lcsas-ecc.wasm", fix_wasm)] if HAVE_WASM else [])


def copy(src, name):
    dst = os.path.join(WORK, name)
    shutil.copyfile(src, dst)
    return dst


def garble(image, sectors, seed):
    rnd = random.Random(seed)
    with open(image, "r+b") as f:
        for s in sectors:
            f.seek(s * SECTOR)
            f.write(bytes(rnd.getrandbits(8) for _ in range(SECTOR)))


def erase(image, sectors):
    """Mark sectors unreadable, as dvdisaster's reader does for a sector the drive could not read.

    The marker is taken from dvdisaster itself (--erase on a scratch copy); only its sector-number
    field (decimal text at 0x160) differs from sector to sector.
    """
    sectors = sorted(sectors)
    if not sectors:
        return
    scratch = copy(image, "marker.iso")
    run([LIGHT, "--debug", "-i", scratch, "--erase", "%d-%d" % (sectors[0], sectors[0])])
    with open(scratch, "rb") as f:
        f.seek(sectors[0] * SECTOR)
        template = bytearray(f.read(SECTOR))
    os.remove(scratch)
    with open(image, "r+b") as f:
        for s in sectors:
            m = bytearray(template)
            m[0x160:0x180] = str(s).encode().ljust(32, b"\0")
            f.seek(s * SECTOR)
            f.write(m)


def bursts(total, pct, seed, avoid=()):
    """Sectors hit by clustered damage (scratches, edge rot): runs of 8-128 sectors up to pct%."""
    rnd, hit = random.Random(seed), set()
    while len(hit) < total * pct // 100:
        start, length = rnd.randrange(total), rnd.randint(8, 128)
        hit.update(s for s in range(start, min(total, start + length)) if s not in avoid)
    return sorted(hit)


def ranges(sorted_sectors):
    out = []
    for s in sorted_sectors:
        if out and s == out[-1][1] + 1:
            out[-1][1] = s
        else:
            out.append([s, s])
    return out


def table(head, rows):
    print("| " + " | ".join(head) + " |")
    print("|" + "---|" * len(head))
    for r in rows:
        print("| " + " | ".join(str(c) for c in r) + " |")
    print()


def mark(ok):
    return "repaired" if ok else "**failed**"


def samples():
    return sorted(os.path.join(SAMPLES, n) for n in os.listdir(SAMPLES) if n.endswith(".iso"))


def e1_verify():
    print("### 1. Verifying the sample discs\n")
    rows = []
    for img in samples():
        i = info(img)
        r1 = run([SPEED47, "-i", img, "-t", "--no-progress"])
        r2 = run([LIGHT, "-i", img, "-t", "--no-progress"])
        r3 = run([LCSAS, "verify", img])
        ok = lambda r: "pass" if r[0] == 0 and "fail" not in r[1].lower() else "**fail**"
        rows.append([os.path.basename(img), i["data"], i["redundancy"], ok(r1), ok(r2),
                     "pass" if r3[0] == 0 and r3[1].startswith("OK") else "**fail**"])
    table(["Disc", "Data sectors", "RS03 redundancy", "speed47 -t", "Light -t", "lcsas-ecc verify"], rows)


def e2_recreate():
    print("### 2. Re-creating RS03 from the bare image (same medium size)\n")
    rows = []
    for img in samples():
        i = info(img)
        cells = [os.path.basename(img)]
        for name, binary in (("speed47", SPEED47), ("light", LIGHT)):
            bare = copy(img, "bare-" + name + ".iso")
            with open(bare, "r+b") as f:
                f.truncate(i["data"] * SECTOR)
            run([binary, "-i", bare, "-mRS03", "-o", "image", "-c", "-n", str(MEDIUM), "--no-progress"])
            cells.append("identical" if same(bare, img) else "**differs**")
        rows.append(cells)
    table(["Disc", "speed47 -c -n %d" % MEDIUM, "Light -c -n %d" % MEDIUM], rows)
    print("lcsas-ecc augment only targets the standard media sizes (CD, DVD, BD), so it cannot\n"
          "re-create these custom-size samples; see the notes.\n")


def e3_limits():
    print("### 3. Repair limits: random sectors across the whole image\n")
    print("*Unreadable* is what a damaged disc gives: the drive reports a read error and dvdisaster\n"
          "marks the sector. *Garbled* is silent corruption (wrong bytes returned as good), which\n"
          "optical drives' own error correction makes rare.\n")
    for name, levels in (("SCAN-01_1995-2008_D.iso", (10, 15, 18, 19, 20, 22)),
                         ("TRIP-01_2019_4.iso", (20, 40, 48, 50, 52, 55))):
        img = os.path.join(SAMPLES, name)
        i = info(img)
        rows = []
        for kind in ("unreadable", "garbled"):
            for pct in levels:
                hit = random.Random(pct).sample(range(i["total"]), i["total"] * pct // 100)
                damaged = copy(img, "damaged.iso")
                erase(damaged, hit) if kind == "unreadable" else garble(damaged, hit, pct)
                row = ["%s %d%% (%d sectors)" % (kind, pct, len(hit))]
                for label, fix in FIXERS:
                    row.append(mark(fix(copy(damaged, "dmg.iso"), img)))
                rows.append(row)
        for pct in levels[:3]:
            hit = random.Random(pct).sample(range(i["data"]), i["total"] * pct // 100)
            damaged = copy(img, "damaged.iso")
            erase(damaged, hit)
            row = ["unreadable %d%%, data area only" % pct]
            for label, fix in FIXERS:
                row.append(mark(fix(copy(damaged, "dmg.iso"), img)))
            rows.append(row)
        print("%s: %s redundancy, %d roots per 255-byte codeword (%.1f%% of the image)\n"
              % (name, i["redundancy"], i["nroots"], 100.0 * i["nroots"] / 255))
        table(["Damage"] + [l for l, _ in FIXERS], rows)


def e4_bookkeeping():
    print("### 4. Damage to the filesystem and to RS03's own bookkeeping\n")
    img = os.path.join(SAMPLES, "SCAN-01_1995-2008_D.iso")
    i = info(img)
    cases = [("filesystem area (sectors 0-299)", range(0, 300)),
             ("RS03 header sector (%d)" % i["header"], [i["header"]]),
             ("header + first CRC layer", [i["header"]] + list(range(i["crc"], i["crc"] + i["spl"]))),
             ("all CRC sectors (%d-%d)" % (i["crc"], i["ecc"] - 1), range(i["crc"], i["ecc"])),
             ("last 300 sectors (ECC area)", range(i["total"] - 300, i["total"]))]
    rows = []
    for kind in ("unreadable", "garbled"):
        for label, sectors in cases:
            row = ["%s: %s" % (kind, label)]
            for _, fix in FIXERS:
                c = copy(img, "dmg.iso")
                erase(c, list(sectors)) if kind == "unreadable" else garble(c, list(sectors), 1)
                row.append(mark(fix(c, img)))
            rows.append(row)
    table(["Damaged"] + [l for l, _ in FIXERS], rows)


def e5_two_copies():
    print("### 5. Two damaged copies of the same disc (each beyond repair alone)\n")
    img = os.path.join(SAMPLES, "SCAN-01_1995-2008_D.iso")
    i = info(img)
    keep = {16, i["header"]}   # see section 6: these two must be readable on at least the copy read first
    a_bad, b_bad = bursts(i["total"], 30, 1, keep), bursts(i["total"], 30, 2, keep)
    a, b = copy(img, "copyA.iso"), copy(img, "copyB.iso")
    erase(a, a_bad)
    erase(b, b_bad)
    both = len(set(a_bad) & set(b_bad))
    rows = []
    alone = [fix_dvdisaster(LIGHT)(copy(c, "alone.iso"), img) for c in (a, b)]
    merged = os.path.join(WORK, "merged.iso")
    with open(a, "rb") as fa, open(b, "rb") as fb, open(merged, "wb") as fm:
        while True:
            x, y = fa.read(SECTOR), fb.read(SECTOR)
            if not x:
                break
            fm.write(y if DEAD in x[:64] else x)
    rows.append(["merge the two images sector by sector, then `-f`", "%s / %s" % (mark(alone[0]), mark(alone[1])),
                 mark(fix_dvdisaster(LIGHT)(merged, img))])
    # stock dvdisaster: reading into an existing image reads only the sectors still missing
    for label, binary in (("speed47", SPEED47), ("Light", LIGHT)):
        out = os.path.join(WORK, "read-%s.iso" % label)
        if os.path.exists(out):
            os.remove(out)
        for disc in (a, b):
            run([binary, "--debug", "--sim-cd=" + disc, "-d", "sim-cd", "-r", "-j", "1", "-i", out, "--no-progress"])
            if disc == a:
                one_ok = fix_dvdisaster(binary)(copy(out, "first.iso"), img)
        rows.append(["%s: read disc A, then disc B into the same image (`-r -j 1`), then `-f`" % label,
                     mark(one_ok), mark(fix_dvdisaster(binary)(out, img))])
    print("Clustered damage (runs of 8-128 sectors), 30%% of each copy; %d sectors (%.1f%%) are bad on both.\n"
          % (both, 100.0 * both / i["total"]))
    table(["Method", "One copy alone", "Both copies"], rows)


def e6_rescue():
    print("### 6. Reading a damaged disc (simulated drive, permanent damage)\n")
    img = os.path.join(SAMPLES, "SCAN-01_1995-2008_D.iso")
    i = info(img)
    rows = []
    cases = [("clustered 5%", bursts(i["total"], 5, 105, {16, i["header"]})),
             ("clustered 15%", bursts(i["total"], 15, 115, {16, i["header"]})),
             ("clustered 18%", bursts(i["total"], 18, 118, {16, i["header"]})),
             ("only the RS03 header sector (%d)" % i["header"], [i["header"]]),
             ("only ISO/UDF sector 16", [16])]
    for label, hit in cases:
        disc = copy(img, "disc.iso")
        erase(disc, hit)
        for tool, cmd in (("Light `-r --rescue`", [LIGHT, "-r", "--rescue", "--mapfile"]),
                          ("speed47 `-r -j 1` then `-f`", [SPEED47, "-r", "-j", "1"])):
            out, mapfile = os.path.join(WORK, "rescue.iso"), os.path.join(WORK, "rescue.map")
            for p in (out, mapfile):
                if os.path.exists(p):
                    os.remove(p)
            args = [cmd[0], "--debug", "--sim-cd=" + disc, "-d", "sim-cd"] + cmd[1:]
            if "--mapfile" in cmd:
                args.append(mapfile)
            code, log = run(args + ["-i", out, "--no-progress"])
            if cmd[0] == SPEED47:
                run([SPEED47, "-i", out, "-f", "--no-progress"])
            size = os.path.getsize(out) // SECTOR if os.path.exists(out) else 0
            rows.append(["%s (%d sectors)" % (label, len(hit)), tool, size,
                         "identical" if os.path.exists(out) and same(out, img) else "**incomplete**"])
    table(["Permanently unreadable", "Tool", "Sectors in the image read", "Result"], rows)
    print("The simulated drive's own `--sim-defects` failures are not permanent: Light's reverse\n"
          "and retry passes read all of them back, so permanent damage is simulated by marking\n"
          "sectors unreadable in the image the simulated drive serves. With damage scattered\n"
          "evenly (not clustered), reading with the default 16-sector skip after an error gives up on\n"
          "almost the whole disc (3,058 of 3,060 sectors at 35%), so these tests use clustered damage.\n")


def peak(cmd):
    """Run cmd; return (seconds, peak resident memory in MiB) of that command alone."""
    probe = ("import resource, subprocess, sys, time; t = time.time(); "
             "subprocess.run(sys.argv[1:], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); "
             "print(time.time() - t, resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)")
    out = subprocess.run([sys.executable, "-c", probe] + cmd, stdout=subprocess.PIPE).stdout.split()
    return float(out[0]), int(out[1]) // 1024


def e7_big():
    print("### 7. A 1 GiB image: time and peak memory\n")
    raw = os.path.join(WORK, "big.iso")
    sectors = 524288
    run([LIGHT, "--debug", "-i", raw, "--random-image", str(sectors), "--random-seed", "3"])
    medium = sectors * 5 // 4
    rows = []
    for label, binary in (("speed47", SPEED47), ("Light", LIGHT)):
        img = copy(raw, "big-%s.iso" % label)
        took, mib = peak([binary, "-i", img, "-mRS03", "-o", "image", "-c", "-n", str(medium), "--no-progress",
                          "-x", str(os.cpu_count() or 1)])
        rows.append(["%s: create RS03 (-n %d, %d threads)" % (label, medium, os.cpu_count() or 1),
                     "%.1f s" % took, "%d MiB" % mib, ""])
    ref = os.path.join(WORK, "big-Light.iso")
    print("speed47 and Light outputs identical: %s\n" % same(ref, os.path.join(WORK, "big-speed47.iso")))
    os.remove(os.path.join(WORK, "big-speed47.iso"))
    i = info(ref)
    hit = random.Random(4).sample(range(i["data"]), i["total"] // 20)
    damaged = copy(ref, "big-damaged.iso")
    erase(damaged, hit)
    fixers = [("Light -f", [LIGHT, "-i", "{img}", "-f", "--no-progress"]),
              ("lcsas-ecc fix", [LCSAS, "fix", "{img}"])]
    if HAVE_WASM:
        fixers.append(("lcsas-ecc.wasm fix (Node)", ["node", "--no-warnings", RUN_WASI, WASM, "fix", "{img}"]))
    for label, cmd in fixers:
        img = copy(damaged, "big-dmg.iso")
        took, mib = peak([c.replace("{img}", img) for c in cmd])
        rows.append(["%s: 5%% unreadable (%d sectors, data area)" % (label, len(hit)), "%.1f s" % took,
                     "%d MiB" % mib, "identical" if same(img, ref) else "**not repaired**"])
        os.remove(img)
    print("Image: %d data sectors, %s redundancy, %d sectors in all.\n" % (i["data"], i["redundancy"], i["total"]))
    table(["Step", "Time", "Peak memory", "Result"], rows)


def main():
    for tool in (SPEED47, LIGHT, LCSAS):
        if not os.path.exists(tool):
            raise SystemExit("missing %s: run scripts/research/rs03/build-tools.sh %s" % (tool, T))
    print("Tools: %s; %s; lcsas-ecc%s\n" % (run([SPEED47, "--version"])[1].strip().splitlines()[0],
                                             run([LIGHT, "--version"])[1].strip().splitlines()[0],
                                             " (+ WASI build under Node)" if HAVE_WASM else ""))
    try:
        e1_verify()
        e2_recreate()
        e3_limits()
        e4_bookkeeping()
        e5_two_copies()
        e6_rescue()
        if "--big" in sys.argv:
            e7_big()
    finally:
        shutil.rmtree(WORK, ignore_errors=True)


if __name__ == "__main__":
    main()
