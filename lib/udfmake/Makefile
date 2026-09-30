# libudfmake.a and the udfmake program: NetBSD makefs, UDF backend, for Linux.
# Needs only a C compiler and the C library (glibc).
#
#   make            build libudfmake.a and udfmake
#   make check      also build a BD-ROM UDF 2.50 image and verify it
#   make asan       the same with AddressSanitizer (in build-asan/)
#   make static     a statically linked udfmake with no runtime dependencies (in build-static/)

CC      ?= cc
CFLAGS  ?= -O2 -g
B       ?= build
NB       = netbsd
DEFS     = -DDEFAULT_FSTYPE='"udf"' -include compat/nbtool_config.h
INCS     = -Icompat -I$(NB)/usr.sbin/makefs -I$(NB)/usr.sbin/mtree \
           -I$(NB)/sbin/newfs_udf -I$(NB)/sys/fs/udf
WARN     = -Wno-address-of-packed-member
LIBS     = -lm

NB_SRCS  = $(NB)/usr.sbin/makefs/makefs.c $(NB)/usr.sbin/makefs/walk.c \
           $(NB)/usr.sbin/makefs/udf.c $(NB)/sbin/newfs_udf/udf_core.c \
           $(NB)/sys/fs/udf/udf_osta.c
OWN_SRCS = compat/compat.c compat/other_fs.c udfmake.c
OBJS     = $(patsubst %.c,$(B)/%.o,$(notdir $(NB_SRCS) $(OWN_SRCS)))

vpath %.c $(dir $(NB_SRCS)) compat .

all: $(B)/libudfmake.a $(B)/udfmake

$(B)/makefs.o: $(NB)/usr.sbin/makefs/makefs.c | $(B)
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INCS) -Dmain=netbsd_makefs_main -c $< -o $@

$(B)/%.o: %.c | $(B)
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INCS) -c $< -o $@

$(B)/libudfmake.a: $(OBJS)
	ar rcs $@ $^

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

clean:
	rm -rf build build-asan

.PHONY: all check asan static clean
