# arvc: arv's reader in C (see arvc.c). C99 and POSIX only.
CC      ?= cc
CFLAGS  ?= -O2
WARN     = -std=c99 -Wall -Wextra -Wpedantic -Wshadow
BUILD    = build
LIB      = $(BUILD)/rec.o $(BUILD)/sha256.o $(BUILD)/sha512.o $(BUILD)/edtf.o $(BUILD)/discid.o $(BUILD)/vocab.o $(BUILD)/data.o
OBJS     = $(BUILD)/arvc.o $(BUILD)/util.o $(BUILD)/disc.o $(BUILD)/catalogue.o $(BUILD)/names.o $(BUILD)/home.o $(BUILD)/archive.o $(BUILD)/bag.o $(BUILD)/html.o $(BUILD)/make.o $(BUILD)/record.o $(BUILD)/edit.o $(BUILD)/tags.o $(BUILD)/formats.o $(BUILD)/rocrate.o $(BUILD)/json.o $(BUILD)/udfwrite.o $(LIB)

DATA     = DATA_DESCRIPTORS=../arv/descriptors.rec DATA_README=../arv/readme.txt \
           DATA_DEFAULT_SETS=../arv/default_sets.rec DATA_INDEX_CSS=../arv/index.css \
           DATA_DEFAULT_TAGS=../arv/default_tags.rec

all: $(BUILD)/arvc $(BUILD)/fixtures $(BUILD)/ptyrun

COSMOCC ?= cosmocc

# arv.com: arvc as an Actually Portable Executable (Cosmopolitan: one file that runs on Linux, macOS,
# Windows and the BSDs, x86-64 and ARM64). Every disc made where it exists carries it as tools/arv.com.
# cosmocc: https://cosmo.zip/pub/cosmocc/ (make ape COSMOCC=/path/to/cosmocc/bin/cosmocc)
ape: $(BUILD)/arv.com

$(BUILD)/arv.com: *.c *.h ../udfwrite/udfwrite.c ../udfwrite/udfwrite.h
	@mkdir -p $(BUILD)
	$(COSMOCC) -O2 -std=c99 -o $@ *.c ../udfwrite/udfwrite.c
	rm -f $(BUILD)/arv.com.dbg $(BUILD)/arv.aarch64.elf

$(BUILD)/arvc: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/fixtures: dev/fixtures.c $(LIB) $(BUILD)/names.o $(BUILD)/util.o
	$(CC) $(CFLAGS) $(WARN) $(LDFLAGS) -o $@ dev/fixtures.c $(LIB) $(BUILD)/names.o $(BUILD)/util.o

$(BUILD)/ptyrun: dev/ptyrun.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -o $@ dev/ptyrun.c

$(BUILD)/embed: dev/embed.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -o $@ dev/embed.c

# data.c is committed (one-cc-line builds need it); regenerate after editing the shared files
data: $(BUILD)/embed
	$(BUILD)/embed data.c $(DATA)

data-check: $(BUILD)/embed
	@$(BUILD)/embed $(BUILD)/data.c $(DATA)
	@cmp -s data.c $(BUILD)/data.c || { echo "data.c is out of date: run make -C src/arvc data"; exit 1; }

$(BUILD)/udfwrite.o: ../udfwrite/udfwrite.c ../udfwrite/udfwrite.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -D_POSIX_C_SOURCE=200809L -c -o $@ ../udfwrite/udfwrite.c

$(BUILD)/%.o: %.c arvc.h rec.h sha256.h sha512.h edtf.h discid.h data.h vocab.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -c -o $@ $<

check-ape: $(BUILD)/arv.com $(BUILD)/fixtures $(BUILD)/ptyrun
	sh check.sh $(BUILD)/arv.com

check: all data-check
	$(BUILD)/fixtures ../../tests/fixtures
	sh check.sh $(BUILD)/arvc

clean:
	rm -rf $(BUILD)

.PHONY: all ape check check-ape clean data data-check
