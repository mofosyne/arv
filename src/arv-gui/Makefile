# arv-gui: arv gui, the interface in your web browser, in C99 and POSIX. Like arv-assist it links
# arv's own modules (src/arv, every file but arv.c, which is arv's main), for their JSON and strings.
CC      ?= cc
CFLAGS  ?= -O2 -g
WARN     = -std=c99 -Wall -Wextra -Wpedantic -Wshadow
BUILD    = build
ARV      = $(filter-out ../arv/arv.c, $(wildcard ../arv/*.c)) ../bagit/bagit.c ../udfwrite/udfwrite.c ../rs03/rs03.c

all: $(BUILD)/arv-gui

$(BUILD)/arv-gui: arv-gui.c $(ARV) ../arv/*.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -D_POSIX_C_SOURCE=200809L $(LDFLAGS) -pthread -o $@ arv-gui.c $(ARV) -lm

clean:
	rm -rf $(BUILD)

.PHONY: all clean
