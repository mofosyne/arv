# bagit: the library (bagit.c) and the program. C99 and POSIX, no libraries.
#   make            build/bagit
#   make check      the checks (check.sh)
CC      ?= cc
CFLAGS  ?= -O2 -g
WARN     = -std=c99 -Wall -Wextra -pedantic
BUILD    = build

all: $(BUILD)/bagit

$(BUILD)/bagit: bagit.c bagit_cli.c bagit.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(LDFLAGS) -o $@ bagit.c bagit_cli.c

check: $(BUILD)/bagit
	sh check.sh $(BUILD)/bagit

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
