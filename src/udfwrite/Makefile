# udfwrite: closed, read-only UDF 2.50 images for archives (docs/archival-udf.md).
#
#   make          build udfwrite (library object and program) in build/
#   make check    build test images and check them (udftools, 7-Zip)

CC     ?= cc
CFLAGS ?= -O2 -g
WARN    = -std=c99 -Wall -Wextra -Wpedantic -Wshadow
B      ?= build

all: $(B)/udfwrite

$(B)/udfwrite.o: udfwrite.c udfwrite.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(WARN) -D_POSIX_C_SOURCE=200809L -c -o $@ udfwrite.c

$(B)/udfwrite: udfwrite_cli.c udfwrite.h $(B)/udfwrite.o
	$(CC) $(CFLAGS) $(WARN) -o $@ udfwrite_cli.c $(B)/udfwrite.o

check: $(B)/udfwrite
	sh check.sh $(B)/udfwrite

clean:
	rm -rf $(B)

.PHONY: all check clean
