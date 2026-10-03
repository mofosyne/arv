# arvc: arv's reader in C (see arvc.c). C99 and POSIX only.
CC      ?= cc
CFLAGS  ?= -O2
WARN     = -std=c99 -Wall -Wextra -Wpedantic -Wshadow
BUILD    = build
LIB      = $(BUILD)/rec.o $(BUILD)/sha256.o $(BUILD)/edtf.o
OBJS     = $(BUILD)/arvc.o $(LIB)

all: $(BUILD)/arvc $(BUILD)/fixtures

$(BUILD)/arvc: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/fixtures: $(BUILD)/fixtures.o $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BUILD)/fixtures.o $(LIB)

$(BUILD)/%.o: %.c rec.h sha256.h edtf.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -c -o $@ $<

check: all
	$(BUILD)/fixtures ../../tests/fixtures
	sh check.sh $(BUILD)/arvc

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
