/* Linux stand-ins for the NetBSD library functions makefs uses.
 * mtree spec files (-F) and user/group databases (-N) are not supported. */
#include "nbtool_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <sys/stat.h>
#include "makefs.h"
#include "mtree.h"

NODE *spec(FILE *fp) { (void)fp; errx(1, "-F (mtree spec files) is not supported in this build"); }
void free_nodes(NODE *n) { (void)n; }
u_int nodetoino(u_int type) { (void)type; return 0; }
const char *inotype(u_int mode) {
	switch (mode & S_IFMT) {
	case S_IFDIR: return "dir"; case S_IFREG: return "file"; case S_IFLNK: return "link";
	case S_IFBLK: return "block"; case S_IFCHR: return "char"; case S_IFIFO: return "fifo";
	case S_IFSOCK: return "socket"; default: return "unknown";
	}
}
int setup_getid(const char *dir) { (void)dir; errno = ENOTSUP; return -1; }

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
