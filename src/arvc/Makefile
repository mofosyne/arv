# arvc: arv's reader in C (see arvc.c). C99 and POSIX only.
CC      ?= cc
CFLAGS  ?= -O2
WARN     = -std=c99 -Wall -Wextra -Wpedantic -Wshadow
BUILD    = build
OBJS     = $(BUILD)/arvc.o $(BUILD)/rec.o $(BUILD)/sha256.o

all: $(BUILD)/arvc

$(BUILD)/arvc: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: %.c rec.h sha256.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -c -o $@ $<

check: $(BUILD)/arvc
	sh check.sh $(BUILD)/arvc

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
