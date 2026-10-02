/* Host configuration for building NetBSD's makefs (UDF only) on another OS.
 *
 * The NetBSD sources include this when HAVE_NBTOOL_CONFIG_H is set, which is
 * NetBSD's own hook for building its tools on other hosts (see the
 * `#if HAVE_NBTOOL_CONFIG_H` blocks in them). It says which NetBSD features
 * the host lacks; compat.c and the other compat/ headers fill in only those.
 * NetBSD itself builds natively, without compat/ (see the Makefile).
 *
 * Tested: Linux with glibc and with musl. Compiled against FreeBSD and
 * NetBSD headers (see README.md). Others (OpenBSD, DragonFly, macOS): untested.
 */
#ifndef COMPAT_NBTOOL_CONFIG_H
#define COMPAT_NBTOOL_CONFIG_H

#if defined(__NetBSD__)
#error "on NetBSD build natively, without compat/ (the Makefile does this)"
#endif

#define HAVE_NBTOOL_CONFIG_H 1
#define _GNU_SOURCE 1		/* Linux: BSD and GNU extensions */

#include <sys/cdefs.h>		/* compat/sys/cdefs.h: the host's, where it has one */
#include <sys/types.h>
#include <sys/time.h>		/* timersub */
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <err.h>

/* --- which NetBSD libc functions the host has (1) or lacks (0) ------------ */
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__) || defined(__APPLE__)
#define HAVE_SETPROGNAME	1
#define HAVE_STRLCPY		1
#elif defined(__GLIBC__)
#define HAVE_SETPROGNAME	0
#if __GLIBC_PREREQ(2, 38)
#define HAVE_STRLCPY		1
#else
#define HAVE_STRLCPY		0
#endif
#elif defined(__linux__) || defined(__wasi__)	/* musl, other non-glibc Linux libcs, WASI (musl-based) */
#define HAVE_SETPROGNAME	0
#define HAVE_STRLCPY		1
#else
#define HAVE_SETPROGNAME	0
#define HAVE_STRLCPY		0
#endif
/* NetBSD-only on every other host: strsuftoll, snprintb, emalloc & co (compat.c, util.h) */

#if defined(__wasi__)	/* readdir(3) that always gives "." first (compat.c) */
#include <dirent.h>
DIR *compat_opendir(const char *);
struct dirent *compat_readdir(DIR *);
int compat_closedir(DIR *);
#define opendir compat_opendir
#define readdir compat_readdir
#define closedir compat_closedir
#endif

#if !HAVE_SETPROGNAME
void setprogname(const char *);
const char *getprogname(void);
#endif
#if !HAVE_STRLCPY
size_t strlcpy(char *, const char *, size_t);
#endif

/* --- NetBSD <sys/cdefs.h> macros the host may lack ------------------------- */
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
/* musl names a struct stat field __unused: read <sys/stat.h> before the macro exists */
#include <sys/stat.h>
#ifndef __unused
#define __unused __attribute__((__unused__))
#endif
#ifndef ALLPERMS		/* BSD and glibc have it; musl does not */
#define ALLPERMS (S_ISUID | S_ISGID | S_ISVTX | S_IRWXU | S_IRWXG | S_IRWXO)
#endif

#endif /* COMPAT_NBTOOL_CONFIG_H */
