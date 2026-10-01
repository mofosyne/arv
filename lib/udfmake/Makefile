# libudfmake.a and the udfmake program: NetBSD makefs, UDF backend.
#
#   make            build libudfmake.a and udfmake
#   make check      also build a BD-ROM UDF 2.50 image and verify it
#   make asan       the same with AddressSanitizer (in build-asan/)
#   make static     a statically linked udfmake with no runtime dependencies (in build-static/)
#   make wasi       a WebAssembly (WASI) udfmake, checked by running it with Node (in build-wasi/);
#                   needs clang with the wasm32 target and a WASI libc (WASI_SYSROOT)
#
# On NetBSD the sources build natively (their own headers and libraries). On
# any other host (Linux, FreeBSD, ...) they build the way NetBSD builds its own
# tools on foreign hosts: with HAVE_NBTOOL_CONFIG_H and compat/ filling in only
# what the host lacks. HOST_OS picks the mode (default: this machine's).
# Needs only a C compiler and the C library.

CC      ?= cc
CFLAGS  ?= -O2 -g
B       ?= build
NB       = netbsd
HOST_OS ?= $(shell uname -s)

NB_SRCS  = $(NB)/usr.sbin/makefs/makefs.c $(NB)/usr.sbin/makefs/walk.c \
           $(NB)/usr.sbin/makefs/udf.c $(NB)/sbin/newfs_udf/udf_core.c \
           $(NB)/sys/fs/udf/udf_osta.c
OWN_SRCS = stubs/other_fs.c stubs/mtree_stubs.c udfmake.c
INCS     = -Istubs -I$(NB)/usr.sbin/makefs -I$(NB)/usr.sbin/mtree \
           -I$(NB)/sbin/newfs_udf -I$(NB)/sys/fs/udf
WARN     = -Wno-address-of-packed-member
DEFS     = -DDEFAULT_FSTYPE='"udf"'

ifeq ($(HOST_OS),NetBSD)
LIBS    ?= -lutil -lprop -lm
else
OWN_SRCS += compat/compat.c
INCS     := -Icompat $(INCS)
DEFS     += -include compat/nbtool_config.h
LIBS    ?= -lm
endif

OBJS     = $(patsubst %.c,$(B)/%.o,$(notdir $(NB_SRCS) $(OWN_SRCS)))

vpath %.c $(dir $(NB_SRCS)) stubs compat .

all: $(B)/libudfmake.a $(B)/udfmake

$(B)/makefs.o: $(NB)/usr.sbin/makefs/makefs.c | $(B)
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INCS) -Dmain=netbsd_makefs_main -c $< -o $@

$(B)/%.o: %.c | $(B)
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INCS) -c $< -o $@

$(B)/libudfmake.a: $(OBJS)
	$(AR) rcs $@ $^

$(B)/udfmake: udfmake_cli.c $(B)/libudfmake.a
	$(CC) $(CFLAGS) $(LDFLAGS) -I. $< $(B)/libudfmake.a $(LIBS) -o $@

$(B):
	mkdir -p $@

check: all
	./check.sh $(B)/udfmake

asan:
	$(MAKE) B=build-asan CFLAGS="-O0 -g -fsanitize=address" LDFLAGS=-fsanitize=address check

static:
	$(MAKE) B=build-static LDFLAGS=-static check

WASI_SYSROOT ?= /usr
WASI_AR      ?= llvm-ar
wasi:
	$(MAKE) B=build-wasi CC="clang --target=wasm32-wasi --sysroot=$(WASI_SYSROOT)" AR=$(WASI_AR) \
	    CFLAGS="-O2 -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS" \
	    LDFLAGS="-lwasi-emulated-signal -lwasi-emulated-process-clocks" all
	UDFMAKE_WASM=build-wasi/udfmake ./check.sh wasi/udfmake

clean:
	rm -rf build build-*

.PHONY: all check asan static wasi clean
