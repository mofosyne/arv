/* Linux build of NetBSD makefs (UDF only) */
#define _GNU_SOURCE 1
#include <bsd/sys/cdefs.h>
#include <bsd/string.h>
#include <bsd/stdlib.h>
#include <bsd/err.h>
#include <sys/types.h>
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
