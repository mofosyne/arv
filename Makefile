# arv (Archive, Record, Verify): install on Linux.
#
#   make                       build src/udfwrite and src/arvc (needs a C compiler)
#   make install               install for everyone: /usr/local (run as root)
#   make install PREFIX=~/.local     install for yourself (~/.local/bin must be on PATH)
#   make uninstall [PREFIX=...]
#   make check                 run the tests
#
# Installs:
#   $(PREFIX)/share/arv/       the tool: exactly the tree every disc carries in tools/
#                              (git archive HEAD; uncommitted changes are not installed)
#   $(PREFIX)/bin/arv          arv: the C program (arvc) for what it covers, the Python arv for the
#                              rest (AI helpers, drafts, gui, ...), automatically
#   $(PREFIX)/bin/arv-py       the Python arv, always
#   $(PREFIX)/bin/udfwrite     arv's own UDF 2.50 writer (the default)
#   $(PREFIX)/bin/arvc         the C program alone (no Python fallback)
#
# Needs at run time: dvdisaster Light (or the speed47 fork) for error correction; python3
# only for what arv hands to the Python arv (AI helpers, drafts, gui, ...); see README.md,
# "Install". src/udfmake (NetBSD makefs, kept for the upstream fixes) is not built or installed.

PREFIX  ?= /usr/local
DESTDIR ?=
SHARE    = $(PREFIX)/share/arv
BIN      = $(PREFIX)/bin

all:
	$(MAKE) -C src/udfwrite
	$(MAKE) -C src/arvc

check: all
	python3 -m unittest discover -s tests

install: all
	@git rev-parse --git-dir >/dev/null 2>&1 || { echo "make install: run it in a git checkout"; exit 1; }
	@git diff --quiet HEAD -- || echo "Note: uncommitted changes are not installed (discs carry the last commit too)"
	rm -rf "$(DESTDIR)$(SHARE)"
	mkdir -p "$(DESTDIR)$(SHARE)" "$(DESTDIR)$(BIN)"
	git archive --format=tar HEAD | tar -x -C "$(DESTDIR)$(SHARE)"
	printf 'arv@%s\n' "$$(git rev-parse --short=12 HEAD)" > "$(DESTDIR)$(SHARE)/VERSION"
	printf '#!/bin/sh\nexec python3 "%s/arv" "$$@"\n' "$(SHARE)" > "$(DESTDIR)$(BIN)/arv-py"
	chmod 755 "$(DESTDIR)$(BIN)/arv-py"
	rm -f "$(DESTDIR)$(BIN)/arv"
	ln -s arvc "$(DESTDIR)$(BIN)/arv"
	install -m 755 src/udfwrite/build/udfwrite "$(DESTDIR)$(BIN)/udfwrite"
	install -m 755 src/arvc/build/arvc "$(DESTDIR)$(BIN)/arvc"
	@echo "Installed arv $$(cat "$(DESTDIR)$(SHARE)/VERSION") in $(PREFIX). Try: arv --help"

uninstall:
	rm -rf "$(DESTDIR)$(SHARE)"
	rm -f "$(DESTDIR)$(BIN)/arv" "$(DESTDIR)$(BIN)/arv-py" "$(DESTDIR)$(BIN)/udfmake" "$(DESTDIR)$(BIN)/udfwrite" "$(DESTDIR)$(BIN)/arvc"

.PHONY: all check install uninstall
