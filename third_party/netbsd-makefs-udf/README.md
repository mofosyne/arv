# NetBSD makefs (UDF): upstream reference

This folder is about **upstream NetBSD**, not our code. Our extracted and modified
copy, built as a C library, is in [`lib/udfmake/`](../../lib/udfmake/).

| File | What |
|---|---|
| `BUG-REPORT.md` | **Unconfirmed draft, not sent**: a report for three bugs in upstream `makefs -t udf`, with the exact commit, file revisions and lines |
| `patches/01-udf_copy_file-padding-overread.patch` | Bug 1: heap bytes written into file padding (`usr.sbin/makefs/udf.c`) |
| `patches/02-udf_set_regid-strcpy-overrun.patch` | Bug 2: 1-byte `strcpy` overrun (`sbin/newfs_udf/udf_core.c`) |
| `patches/03-unix_to_udf_name-l_fi-overflow.patch` | Bug 3: over-long names corrupt the image (`sbin/newfs_udf/udf_core.c`) |
| `repro/repro.sh` | Fetches **unmodified** upstream at the pinned commit, builds it on Linux, shows each bug, then shows each patch fixes its own bug and only that one |
| `repro/padding.py` | Test files, and the image check used by `repro.sh` |

Each patch has a short description at the top, is against upstream paths (`patch -p1` in
a NetBSD src tree), and applies on its own or together with the others in any order.
Together they are exactly the changes in `lib/udfmake/netbsd/`.

Pinned upstream: NetBSD src `477d71b4d1b73a66b61a03b5f6d3dc9212d4f888`
(https://github.com/NetBSD/src, a mirror of NetBSD CVS; trunk as of 2026-09-30).
BSD licence.
