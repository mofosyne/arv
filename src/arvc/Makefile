# arvc: arv's reader in C (see arvc.c). C99 and POSIX only.
CC      ?= cc
CFLAGS  ?= -O2
WARN     = -std=c99 -Wall -Wextra -Wpedantic -Wshadow
BUILD    = build
LIB      = $(BUILD)/rec.o $(BUILD)/sha256.o $(BUILD)/sha512.o $(BUILD)/edtf.o $(BUILD)/discid.o
OBJS     = $(BUILD)/arvc.o $(BUILD)/util.o $(BUILD)/disc.o $(BUILD)/catalogue.o $(LIB)

all: $(BUILD)/arvc $(BUILD)/fixtures

$(BUILD)/arvc: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/fixtures: $(BUILD)/fixtures.o $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BUILD)/fixtures.o $(LIB)

$(BUILD)/%.o: %.c arvc.h rec.h sha256.h sha512.h edtf.h discid.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -c -o $@ $<

check: all
	$(BUILD)/fixtures ../../tests/fixtures
	sh check.sh $(BUILD)/arvc

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
