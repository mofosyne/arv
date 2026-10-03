# arv (Archive, Record, Verify): install on Linux.
#
#   make                       build src/udfmake, src/udfwrite and src/arvc (needs a C compiler)
#   make install               install for everyone: /usr/local (run as root)
#   make install PREFIX=~/.local     install for yourself (~/.local/bin must be on PATH)
#   make uninstall [PREFIX=...]
#   make check                 run the tests
#
# Installs:
#   $(PREFIX)/share/arv/       the tool: exactly the tree every disc carries in tools/
#                              (git archive HEAD; uncommitted changes are not installed)
#   $(PREFIX)/bin/arv          runs it with python3
#   $(PREFIX)/bin/udfmake      UDF 2.50 image builder (NetBSD makefs), the default
#   $(PREFIX)/bin/udfwrite     arv's own UDF 2.50 writer (the default)
#   $(PREFIX)/bin/arvc         arv's reader in C: info, verify, ls, restore (the start of the C port)
#
# Needs at run time: python3, genisoimage, and dvdisaster Light (or the speed47
# fork) on PATH; see README.md, "Install".

PREFIX  ?= /usr/local
DESTDIR ?=
SHARE    = $(PREFIX)/share/arv
BIN      = $(PREFIX)/bin

all:
	$(MAKE) -C src/udfmake
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
	printf '#!/bin/sh\nexec python3 "%s/arv" "$$@"\n' "$(SHARE)" > "$(DESTDIR)$(BIN)/arv"
	chmod 755 "$(DESTDIR)$(BIN)/arv"
	install -m 755 src/udfmake/build/udfmake "$(DESTDIR)$(BIN)/udfmake"
	install -m 755 src/udfwrite/build/udfwrite "$(DESTDIR)$(BIN)/udfwrite"
	install -m 755 src/arvc/build/arvc "$(DESTDIR)$(BIN)/arvc"
	@echo "Installed arv $$(cat "$(DESTDIR)$(SHARE)/VERSION") in $(PREFIX). Try: arv --help"

uninstall:
	rm -rf "$(DESTDIR)$(SHARE)"
	rm -f "$(DESTDIR)$(BIN)/arv" "$(DESTDIR)$(BIN)/udfmake" "$(DESTDIR)$(BIN)/udfwrite" "$(DESTDIR)$(BIN)/arvc"

.PHONY: all check install uninstall
