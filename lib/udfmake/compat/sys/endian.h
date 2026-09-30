/* <sys/endian.h> as the NetBSD sources expect it: BYTE_ORDER and bswap16/32/64. */
#ifndef COMPAT_SYS_ENDIAN_H
#define COMPAT_SYS_ENDIAN_H
#if defined(__linux__)
#include <endian.h>
#elif defined(__APPLE__)
#include <machine/endian.h>
#else				/* FreeBSD, OpenBSD, DragonFly */
#include_next <sys/endian.h>
#endif
#ifndef bswap16
#define bswap16(x) __builtin_bswap16(x)
#endif
#ifndef bswap32
#define bswap32(x) __builtin_bswap32(x)
#endif
#ifndef bswap64
#define bswap64(x) __builtin_bswap64(x)
#endif
#endif
