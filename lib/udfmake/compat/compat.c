/* NetBSD libc/libutil functions that other hosts lack. Each is built only
 * where nbtool_config.h says the host does not have it. Not used on NetBSD. */
#include "nbtool_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <sys/stat.h>

#if !HAVE_SETPROGNAME
static const char *progname = "udfmake";

void setprogname(const char *name)
{
	const char *slash = strrchr(name, '/');
	progname = slash ? slash + 1 : name;
}

const char *getprogname(void) { return progname; }
#endif

#if !HAVE_STRLCPY
size_t strlcpy(char *dst, const char *src, size_t size)
{
	size_t len = strlen(src);
	if (size) {
		size_t n = len < size - 1 ? len : size - 1;
		memcpy(dst, src, n);
		dst[n] = '\0';
	}
	return len;
}
#endif

int snprintb(char *buf, size_t len, const char *fmt, uint64_t val)
{	/* debug output only: print the value in hex */
	(void)fmt;
	return snprintf(buf, len, "0x%llx", (unsigned long long)val);
}

long long strsuftoll(const char *desc, const char *val, long long min, long long max)
{	/* number with optional b/k/m/g/t suffix (x for products is not supported) */
	char *end;
	errno = 0;
	long long n = strtoll(val, &end, 0);
	if (errno || end == val)
		errx(1, "%s: invalid number `%s'", desc, val);
	switch (*end) {
	case 'b': n *= 512; end++; break;
	case 'k': n <<= 10; end++; break;
	case 'm': n <<= 20; end++; break;
	case 'g': n <<= 30; end++; break;
	case 't': n <<= 40; end++; break;
	}
	if (*end)
		errx(1, "%s: invalid number `%s'", desc, val);
	if (n < min || n > max)
		errx(1, "%s %lld is out of range %lld..%lld", desc, n, min, max);
	return n;
}
