/* Linux build of NetBSD makefs (UDF only): what NetBSD's <sys/cdefs.h> and
 * libc provide that glibc does not. Only the C library is needed. */
#define _GNU_SOURCE 1
#include <sys/cdefs.h>
#include <sys/types.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <err.h>
#include <stdint.h>
#include <endian.h>
#ifndef __RCSID
#define __RCSID(x)
#endif
#ifndef __KERNEL_RCSID
#define __KERNEL_RCSID(n, x)
#endif
#ifndef __UNCONST
#define __UNCONST(a) ((void *)(uintptr_t)(const void *)(a))
#endif
#ifndef __arraycount
#define __arraycount(a) (sizeof(a) / sizeof(a[0]))
#endif
#ifndef __packed
#define __packed __attribute__((__packed__))
#endif
#ifndef __dead
#define __dead __attribute__((__noreturn__))
#endif
#ifndef __printflike
#define __printflike(a, b) __attribute__((__format__(__printf__, a, b)))
#endif
#define HAVE_NBTOOL_CONFIG_H 1
#include <inttypes.h>
#ifndef __unused
#define __unused __attribute__((__unused__))
#endif
#include <sys/time.h>

/* NetBSD libc functions, in compat.c */
void setprogname(const char *);
const char *getprogname(void);
#if !defined(__GLIBC__) || !__GLIBC_PREREQ(2, 38)
size_t strlcpy(char *, const char *, size_t);
#define COMPAT_NEED_STRLCPY 1
#endif
