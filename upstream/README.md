# upstream: work for other projects

**Not part of arv.** Nothing here is built by `make` or used to make or read a disc. It is what
we offer back to the projects arv learned from or depends on. Every disc still carries it, in
`tools/arv/`, with the rest of the source.

| Folder | For | What | Status |
|---|---|---|---|
| [`netbsd-makefs/`](netbsd-makefs/) | NetBSD | udfmake, our copy of `makefs -t udf` as a library, where arv's UDF work began (arv now uses its own [`src/udfwrite`](../src/udfwrite/)); a bug report for three bugs, one patch per bug, and a script that shows each against unmodified upstream | draft, not confirmed, not sent |
| [`dvdisaster-light/`](dvdisaster-light/) | dvdisaster Light | an offer of arv's RS03 encoder ([`src/rs03`](../src/rs03/)) as a GLib-free refactor of its codec, for its issue #1 | draft, not sent |

Nothing here is sent without a decision to send it. When something is, change its status to say
where and when (with a link).
