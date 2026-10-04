# Everyday commands for working on arv. Run `just` to list them.
# `just` is optional: everything here is a plain command, and `make` alone builds and installs.

# List the recipes
default:
    @just --list

# Run arv from this checkout, e.g. `just arv --home samples/home list`
arv *args:
    ./arv {{args}}

# Build arv and its parts: bagit, rs03, udfwrite, arv-assist (needs a C compiler)
build:
    make

# Run the tests: bagit, rs03, arv against the reference outputs, arv-assist, then arv-gui (python3)
test:
    make check

# Rewrite tests/reference/expected after a change in behaviour made on purpose (then read the diff)
bless:
    sh tests/reference/generate.sh

# Install arv into PREFIX (installs the last commit)
install prefix=(env_var("HOME") + "/.local"):
    make install PREFIX="{{prefix}}"

# Remove what `just install` put in PREFIX (never touches catalogues)
uninstall prefix=(env_var("HOME") + "/.local"):
    make uninstall PREFIX="{{prefix}}"

# Rebuild the sample discs (dvdisaster Light on PATH; commit first: discs carry the last commit)
samples:
    sh samples/make-samples.sh

# Download the sample disc images from the "samples" release into samples/discs/
samples-fetch:
    sh samples/fetch-discs.sh

# Publish samples/discs/*.iso as the "samples" release (gh CLI; after pushing samples/home)
samples-publish:
    sh samples/publish-discs.sh

# Redraw the diagrams in docs/img
diagrams:
    python3 dev-tools/architecture-svg.py
    python3 dev-tools/shelving-svg.py

# Preview the website (docs/) at http://localhost:8000
site port="8000":
    python3 -m http.server {{port}} -d docs

# Show the three NetBSD makefs bugs and their fixes (fetches upstream NetBSD sources)
netbsd-repro:
    sh upstream/netbsd-makefs/repro/repro.sh

# Check arv's own UDF writer and make test images for readers (src/udfwrite/build/check/)
udfwrite-check:
    make -C src/udfwrite check

# Check arv alone: reference outputs, a real disc, dvdisaster Light and Siegfried when present
arv-check:
    make -C src/arv check

# Check arv-assist (the local AI helpers) against a fake model server
assist-check:
    make -C src/arv-assist check

# Remove build output
clean:
    make -C upstream/netbsd-makefs clean
    make -C src/bagit clean
    make -C src/rs03 clean
    make -C src/udfwrite clean
    make -C src/arv clean
    make -C src/arv-assist clean
