# arv-assist: arv's optional local-AI helpers (describe, tag), in C99 and POSIX. It links
# arv's own modules (src/arv, every file but arv.c, which is arv's main) and their libraries.
CC      ?= cc
CFLAGS  ?= -O2 -g
WARN     = -std=c99 -Wall -Wextra -Wpedantic -Wshadow
BUILD    = build
ARV      = $(filter-out ../arv/arv.c, $(wildcard ../arv/*.c)) ../bagit/bagit.c ../udfwrite/udfwrite.c ../rs03/rs03.c

all: $(BUILD)/arv-assist

# the commonest words (dev-tools/common-words.sh), built in; generated at each build, never committed
$(BUILD)/embed: ../../dev-tools/embed.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -o $@ ../../dev-tools/embed.c

$(BUILD)/words-data.c: data/common-words.txt $(BUILD)/embed
	$(BUILD)/embed $@ COMMON_WORDS=data/common-words.txt

$(BUILD)/arv-assist: *.c *.h $(BUILD)/words-data.c $(ARV) ../arv/*.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -D_POSIX_C_SOURCE=200809L -I../arv $(LDFLAGS) -pthread -o $@ *.c $(BUILD)/words-data.c $(ARV) -lm

$(BUILD)/fake-llm: ../../dev-tools/fake-llm.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -o $@ ../../dev-tools/fake-llm.c -lm

check: $(BUILD)/arv-assist $(BUILD)/fake-llm
	$(MAKE) -C ../arv
	sh check.sh $(BUILD)/arv-assist

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
