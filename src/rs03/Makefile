# rs03: dvdisaster's RS03 error correction in C99 and POSIX (threads), no libraries.
CC     ?= cc
CFLAGS ?= -O2
WARN    = -std=c99 -Wall -Wextra -Wpedantic -Wshadow
BUILD   = build

all: $(BUILD)/rs03

$(BUILD)/rs03: rs03.c rs03.h rs03_cli.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -pthread -o $@ rs03.c rs03_cli.c

check: $(BUILD)/rs03
	sh check.sh $(BUILD)/rs03

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
