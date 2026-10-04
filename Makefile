# arv (Archive, Record, Verify): install on Linux.
#
#   make                       build src/bagit, src/rs03, src/udfwrite and src/arvc (needs a C compiler)
#   make install               install for everyone: /usr/local (run as root)
#   make install PREFIX=~/.local     install for yourself (~/.local/bin must be on PATH)
#   make uninstall [PREFIX=...]
#   make check                 run the tests
#   make ape [COSMOCC=...]     also build arv.com (Cosmopolitan): installed, and carried by every disc
#
# Installs:
#   $(PREFIX)/share/arv/       the tool: exactly the tree every disc carries in tools/
#                              (git archive HEAD; uncommitted changes are not installed)
#   $(PREFIX)/bin/arv          arv: the C program (arvc); it hands the AI helpers and gui to the add-on
#   $(PREFIX)/bin/arv-py       the Python add-on (describe, tag, models, gui) alone
#   $(PREFIX)/bin/udfwrite     arv's UDF 2.50 writer as a program of its own (arv has it built in)
#   $(PREFIX)/bin/bagit        a BagIt (RFC 8493) validator for any bag (arv verify uses the same code)
#   $(PREFIX)/bin/arvc         the C program alone (never hands over)
#
# Needs at run time: nothing beyond the C library to make, check and repair discs; python3 only
# for the optional add-on (the local AI helpers and gui); see README.md, "Install". upstream/ (work for other projects) is not built or installed.

PREFIX  ?= /usr/local
DESTDIR ?=
SHARE    = $(PREFIX)/share/arv
BIN      = $(PREFIX)/bin

all:
	$(MAKE) -C src/bagit
	$(MAKE) -C src/rs03
	$(MAKE) -C src/udfwrite
	$(MAKE) -C src/arvc

check: all
	$(MAKE) -C src/bagit check
	$(MAKE) -C src/rs03 check
	$(MAKE) -C src/arvc check
	python3 -m unittest discover -s tests

ape:
	$(MAKE) -C src/arvc ape

install: all
	@git rev-parse --git-dir >/dev/null 2>&1 || { echo "make install: run it in a git checkout"; exit 1; }
	@git diff --quiet HEAD -- || echo "Note: uncommitted changes are not installed (discs carry the last commit too)"
	rm -rf "$(DESTDIR)$(SHARE)"
	mkdir -p "$(DESTDIR)$(SHARE)" "$(DESTDIR)$(BIN)"
	git archive --format=tar HEAD | tar -x -C "$(DESTDIR)$(SHARE)"
	printf 'arv@%s\n' "$$(git rev-parse --short=12 HEAD)" > "$(DESTDIR)$(SHARE)/VERSION"
	if [ -f src/arvc/build/arv.com ]; then install -m 755 src/arvc/build/arv.com "$(DESTDIR)$(SHARE)/arv.com"; fi
	printf '#!/bin/sh\nexec python3 "%s/arv" "$$@"\n' "$(SHARE)" > "$(DESTDIR)$(BIN)/arv-py"
	chmod 755 "$(DESTDIR)$(BIN)/arv-py"
	rm -f "$(DESTDIR)$(BIN)/arv"
	ln -s arvc "$(DESTDIR)$(BIN)/arv"
	install -m 755 src/udfwrite/build/udfwrite "$(DESTDIR)$(BIN)/udfwrite"
	install -m 755 src/bagit/build/bagit "$(DESTDIR)$(BIN)/bagit"
	install -m 755 src/arvc/build/arvc "$(DESTDIR)$(BIN)/arvc"
	@echo "Installed arv $$(cat "$(DESTDIR)$(SHARE)/VERSION") in $(PREFIX). Try: arv --help"

uninstall:
	rm -rf "$(DESTDIR)$(SHARE)"
	rm -f "$(DESTDIR)$(BIN)/arv" "$(DESTDIR)$(BIN)/arv-py" "$(DESTDIR)$(BIN)/udfmake" "$(DESTDIR)$(BIN)/udfwrite" "$(DESTDIR)$(BIN)/bagit" "$(DESTDIR)$(BIN)/arvc"

.PHONY: all check ape install uninstall
