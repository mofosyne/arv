# arv-assist: arv's optional local-AI helpers (describe, tag, models), in C99 and POSIX. It links
# arv's own modules (src/arvc, every file but arvc.c, which is arv's main) and their libraries.
CC      ?= cc
CFLAGS  ?= -O2 -g
WARN     = -std=c99 -Wall -Wextra -Wpedantic -Wshadow
BUILD    = build
ARV      = $(filter-out ../arvc/arvc.c, $(wildcard ../arvc/*.c)) ../bagit/bagit.c ../udfwrite/udfwrite.c ../rs03/rs03.c

all: $(BUILD)/arv-assist

$(BUILD)/arv-assist: *.c *.h $(ARV) ../arvc/*.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -D_POSIX_C_SOURCE=200809L $(LDFLAGS) -pthread -o $@ *.c $(ARV) -lm

$(BUILD)/fake-llm: ../../dev-tools/fake-llm.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -o $@ ../../dev-tools/fake-llm.c

check: $(BUILD)/arv-assist $(BUILD)/fake-llm
	$(MAKE) -C ../arvc
	sh check.sh $(BUILD)/arv-assist

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
