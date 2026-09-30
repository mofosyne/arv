# NetBSD makefs (UDF): upstream reference

This folder is about **upstream NetBSD**, not our code. Our extracted and modified
copy, built as a C library, is in [`lib/udfmake/`](../../lib/udfmake/).

| File | What |
|---|---|
| `BUG-REPORT.md` | Draft report for two memory bugs in upstream `makefs -t udf`, with the exact commit, file revisions and lines |
| `proposed.patch` | The fix for both, against upstream paths (`patch -p1` in a NetBSD src tree) |
| `repro/repro.sh` | Fetches **unmodified** upstream at the pinned commit, builds it on Linux, shows both bugs, then shows the patch fixes them |
| `repro/padding.py` | Test files, and the image check used by `repro.sh` |

Pinned upstream: NetBSD src `477d71b4d1b73a66b61a03b5f6d3dc9212d4f888`
(https://github.com/NetBSD/src, a mirror of NetBSD CVS; trunk as of 2026-09-30).
BSD licence.
