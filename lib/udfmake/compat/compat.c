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

#if defined(__wasi__)
/* wasi-libc declares the err(3) family but (in some versions) leaves it out */
#include <stdarg.h>
void vwarn(const char *fmt, va_list ap)
{
	int saved = errno;
	fprintf(stderr, "%s: ", getprogname());
	if (fmt) {
		vfprintf(stderr, fmt, ap);
		fputs(": ", stderr);
	}
	fprintf(stderr, "%s\n", strerror(saved));
}
void vwarnx(const char *fmt, va_list ap)
{
	fprintf(stderr, "%s: ", getprogname());
	if (fmt)
		vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
}
void warn(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vwarn(fmt, ap); va_end(ap); }
void warnx(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vwarnx(fmt, ap); va_end(ap); }
void verr(int status, const char *fmt, va_list ap) { vwarn(fmt, ap); exit(status); }
void verrx(int status, const char *fmt, va_list ap) { vwarnx(fmt, ap); exit(status); }
void err(int status, const char *fmt, ...) { va_list ap; va_start(ap, fmt); verr(status, fmt, ap); }
void errx(int status, const char *fmt, ...) { va_list ap; va_start(ap, fmt); verrx(status, fmt, ap); }

/*
 * walk.c expects readdir(3) to return "." (it becomes the directory's own
 * node). POSIX lets a system leave it out, and some WASI runtimes (Node's,
 * through libuv) do. Give "." first whenever the runtime didn't.
 */
#include <dirent.h>
#undef opendir
#undef readdir
#undef closedir
struct dotdir {
	DIR *dir;
	int started;
	struct dirent *pending;
	struct dotdir *next;
	union { struct dirent ent; char buf[sizeof(struct dirent) + 2]; } dot;
};
static struct dotdir *dotdirs;

DIR *compat_opendir(const char *path)
{
	DIR *dir = opendir(path);
	if (dir) {
		struct dotdir *d = calloc(1, sizeof(*d));
		if (!d)
			err(EXIT_FAILURE, "calloc");
		d->dir = dir;
		strcpy(d->dot.ent.d_name, ".");
		d->dot.ent.d_type = DT_DIR;
		d->next = dotdirs;
		dotdirs = d;
	}
	return dir;
}

struct dirent *compat_readdir(DIR *dir)
{
	struct dotdir *d = dotdirs;
	struct dirent *e;
	while (d && d->dir != dir)
		d = d->next;
	if (!d)
		return readdir(dir);
	if (!d->started) {
		d->started = 1;
		e = readdir(dir);
		if (e && strcmp(e->d_name, ".") == 0)
			return e;
		d->pending = e;
		return &d->dot.ent;
	}
	if (d->pending) {
		e = d->pending;
		d->pending = NULL;
		return e;
	}
	while ((e = readdir(dir)) && strcmp(e->d_name, ".") == 0)
		;
	return e;
}

int compat_closedir(DIR *dir)
{
	for (struct dotdir **p = &dotdirs; *p; p = &(*p)->next)
		if ((*p)->dir == dir) {
			struct dotdir *d = *p;
			*p = d->next;
			free(d);
			break;
		}
	return closedir(dir);
}
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
