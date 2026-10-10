# arv (Archive, Record, Verify): build and install on Linux and other POSIX systems.
#
#   make                       build arv and its parts (needs a C compiler)
#   make install               install for everyone: /usr/local (run as root)
#   make install PREFIX=~/.local     install for yourself (~/.local/bin must be on PATH)
#                              (from a git checkout, or from a release tarball: packaging/)
#   make uninstall [PREFIX=...]
#   make check                 run the tests
#   make ape [COSMOCC=...]     also build arv.com (Cosmopolitan): installed, and carried by every disc
#
# Installs:
#   $(PREFIX)/bin/arv          arv: every command (C99 and POSIX, no libraries)
#   $(PREFIX)/bin/arv-assist   arv describe, tag, models: the optional local-AI helpers (C)
#   $(PREFIX)/bin/arv-gui      arv gui: the optional web interface (needs python3)
#   $(PREFIX)/bin/udfwrite     arv's UDF 2.50 writer as a program of its own (arv has it built in)
#   $(PREFIX)/bin/bagit        a BagIt (RFC 8493) validator for any bag (arv verify uses the same code)
#   $(PREFIX)/share/arv/       the source, exactly the tree every disc carries in tools/
#                              (git archive HEAD; uncommitted changes are not installed. From a
#                              tarball, which git archive made: the tree as it is, less build output)
#   $(BASHCOMPDIR)/arv, $(ZSHCOMPDIR)/_arv   tab completion, which asks arv (src/arv/completion/)
#
# Needs at run time: nothing beyond the C library to make, check and repair discs; python3 only for
# arv gui; a local model server or llama.cpp only for the AI helpers. upstream/ and dev-tools/ are
# not built or installed.

PREFIX  ?= /usr/local
DESTDIR ?=
SHARE    = $(PREFIX)/share/arv
BIN      = $(PREFIX)/bin
BASHCOMPDIR ?= $(PREFIX)/share/bash-completion/completions
ZSHCOMPDIR  ?= $(PREFIX)/share/zsh/site-functions

all:
	$(MAKE) -C src/bagit
	$(MAKE) -C src/rs03
	$(MAKE) -C src/udfwrite
	$(MAKE) -C src/arv
	$(MAKE) -C src/arv-assist

check: all
	$(MAKE) -C src/bagit check
	$(MAKE) -C src/rs03 check
	$(MAKE) -C src/arv check
	$(MAKE) -C src/arv-assist check
	python3 -m unittest discover -s tests

ape:
	$(MAKE) -C src/arv ape

install: all
	rm -rf "$(DESTDIR)$(SHARE)"
	mkdir -p "$(DESTDIR)$(SHARE)" "$(DESTDIR)$(BIN)"
	@if git rev-parse --git-dir >/dev/null 2>&1; then \
	    git diff --quiet HEAD -- || echo "Note: uncommitted changes are not installed (discs carry the last commit too)"; \
	    git archive --format=tar HEAD | tar -x -C "$(DESTDIR)$(SHARE)"; \
	    printf 'arv@%s\n' "$$(git rev-parse --short=12 HEAD)" > "$(DESTDIR)$(SHARE)/VERSION"; \
	else \
	    echo "Not a git checkout: installing this tree (a release tarball), less its build output"; \
	    tar -c --exclude=.git --exclude='./src/*/build' --exclude='./debian/arv' --exclude='./debian/.debhelper' \
	        --exclude='./debian/tmp' --exclude='./debian/files' --exclude='./debian/*.substvars' \
	        --exclude='./debian/debhelper-build-stamp' --exclude='*/__pycache__' . | tar -x -C "$(DESTDIR)$(SHARE)"; \
	    c=$$(cat COMMIT 2>/dev/null); case "$$c" in ''|*Format*) c=unknown;; esac; \
	    printf 'arv@%.12s\n' "$$c" > "$(DESTDIR)$(SHARE)/VERSION"; \
	fi
	if [ -f src/arv/build/arv.com ]; then install -m 755 src/arv/build/arv.com "$(DESTDIR)$(SHARE)/arv.com"; fi
	install -m 755 src/arv/build/arv "$(DESTDIR)$(BIN)/arv"
	install -m 755 src/arv-assist/build/arv-assist "$(DESTDIR)$(BIN)/arv-assist"
	printf '#!/bin/sh\nexec python3 "%s/src/arv-gui/arv-gui" "$$@"\n' "$(SHARE)" > "$(DESTDIR)$(BIN)/arv-gui"
	chmod 755 "$(DESTDIR)$(BIN)/arv-gui"
	install -m 755 src/udfwrite/build/udfwrite "$(DESTDIR)$(BIN)/udfwrite"
	install -m 755 src/bagit/build/bagit "$(DESTDIR)$(BIN)/bagit"
	install -D -m 644 src/arv/completion/arv.bash "$(DESTDIR)$(BASHCOMPDIR)/arv"
	install -D -m 644 src/arv/completion/_arv "$(DESTDIR)$(ZSHCOMPDIR)/_arv"
	@echo "Installed arv $$(cat "$(DESTDIR)$(SHARE)/VERSION") in $(PREFIX). Try: arv --help"

uninstall:
	rm -rf "$(DESTDIR)$(SHARE)"
	rm -f "$(DESTDIR)$(BIN)/arv" "$(DESTDIR)$(BIN)/arv-assist" "$(DESTDIR)$(BIN)/arv-gui" "$(DESTDIR)$(BIN)/udfwrite" \
	      "$(DESTDIR)$(BIN)/bagit" "$(DESTDIR)$(BIN)/arvc" "$(DESTDIR)$(BIN)/arv-py" \
	      "$(DESTDIR)$(BASHCOMPDIR)/arv" "$(DESTDIR)$(ZSHCOMPDIR)/_arv"

clean:
	rm -rf src/bagit/build src/rs03/build src/udfwrite/build src/arv/build src/arv-assist/build

.PHONY: all check ape install uninstall clean
