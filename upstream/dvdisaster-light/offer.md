# Draft: offering rs03 to dvdisaster Light

**Status: draft, not sent.** For teaching-droid/dvdisaster-light, as a comment on issue #1 (the
proposal to split it into an RS03 codec library and a drive-reading library). Edit before sending.

---

Following up on the idea of an RS03 library: we needed RS03 inside our archiving tool, arv
(https://github.com/mofosyne/arv), which is plain C99 and POSIX with no libraries so that it can
also ship as a single Cosmopolitan "Actually Portable Executable" on every disc. dvdisaster's code
leans on GLib throughout (memory, threads, strings, types), so instead we wrote the RS03 encoder
and an image test from the format, against dvdisaster Light's source:

- `src/rs03/` in arv: `rs03.h` and `rs03.c` (about 600 lines), a small CLI and a test script;
- C99 + POSIX threads, no GLib, no other libraries; GPLv3, the same as dvdisaster;
- augmenting is **byte-identical** to dvdisaster Light 0.3.0 on every case we tried (odd sizes,
  `-n`, the automatic medium choice, the 170-root clip, larger images), and each tool's test
  accepts the other's image; Light repairs damaged rs03 images back to whole;
- on four cores it augments a 200 MB image in 3.2 s (Light: 4.1 s), with a portable 8-byte XOR
  inner loop and one thread per core; no SIMD yet;
- the test (`-t` for image files) finds the layout from the CRC layer, then checks the header,
  each data sector's CRC, the CRC sectors and every parity sector.

It does not cover repair (`-f`), ecc files, or reading drives; those stay Light's strengths.

If it would help your library split, you are welcome to take it, in whole or in part: as the
core of a GLib-free `librs03` encoder, as a cross-check for your own, or just as a reference.
We'd be glad to adapt the API to what you'd want (for instance a buffer-based interface rather
than a file path, or your naming), to contribute the test script, or to send it as a pull
request. Equally, if you'd rather keep GLib, no problem: the format notes and test cases may
still be useful.

Thanks for dvdisaster Light: its CLI, `--rescue` and the clear code made this possible.
