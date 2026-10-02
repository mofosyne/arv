# Everyday commands for working on arv. Run `just` to list them.
# `just` is optional: everything here is a plain command, and `make` alone builds and installs.

# List the recipes
default:
    @just --list

# Run arv from this checkout, e.g. `just arv --home samples/home list`
arv *args:
    ./arv {{args}}

# Build src/udfmake, the UDF 2.50 image builder (needs a C compiler)
build:
    make

# Run the tests
test:
    python3 -m unittest discover -s tests

# Run the tests including the dvdisaster ones (dvdisaster Light or the speed47 fork on PATH)
test-ecc:
    ARCHIVE_TEST_ECC=1 python3 -m unittest discover -s tests

# Install arv and udfmake into PREFIX (installs the last commit)
install prefix=(env_var("HOME") + "/.local"):
    make install PREFIX="{{prefix}}"

# Remove what `just install` put in PREFIX (never touches catalogues)
uninstall prefix=(env_var("HOME") + "/.local"):
    make uninstall PREFIX="{{prefix}}"

# Rebuild the sample discs (dvdisaster Light on PATH; commit first: discs carry the last commit)
samples:
    sh samples/make-samples.sh

# Redraw the diagrams in docs/img
diagrams:
    python3 docs/img/architecture-svg.py
    python3 docs/img/shelving-svg.py

# Preview the website (docs/) at http://localhost:8000
site port="8000":
    python3 -m http.server {{port}} -d docs

# Show the three NetBSD makefs bugs and their fixes (fetches upstream NetBSD sources)
netbsd-repro:
    sh src/udfmake/upstream/repro/repro.sh

# Remove build output
clean:
    make -C src/udfmake clean
