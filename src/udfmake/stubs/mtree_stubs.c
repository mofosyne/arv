/* makefs.c and walk.c call into mtree(8) code for -F spec files and -N user
 * databases. That code is not built here on any platform; these stand in. */
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include "makefs.h"
#include "mtree.h"

NODE *spec(FILE *fp) { (void)fp; errx(1, "-F (mtree spec files) is not supported in this build"); }
void free_nodes(NODE *n) { (void)n; }
u_int nodetoino(u_int type) { (void)type; return 0; }
const char *inotype(u_int mode) {
	switch (mode & S_IFMT) {
	case S_IFDIR: return "dir"; case S_IFREG: return "file"; case S_IFLNK: return "link";
	case S_IFBLK: return "block"; case S_IFCHR: return "char"; case S_IFIFO: return "fifo";
#if S_IFSOCK != S_IFIFO		/* WASI gives both the same value */
	case S_IFSOCK: return "socket";
#endif
	default: return "unknown";
	}
}
int setup_getid(const char *dir) { (void)dir; errno = ENOTSUP; return -1; }
