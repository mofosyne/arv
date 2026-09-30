/* makefs.c lists every filesystem type; only UDF is built here. These stand in
 * for the others, so the NetBSD sources need no edits to leave them out. */
#include "nbtool_config.h"
#include <err.h>
#include <stdlib.h>
#include "makefs.h"

#define NOT_BUILT(fs)							\
void fs ## _prep_opts(fsinfo_t *f) { (void)f; }				\
int fs ## _parse_opts(const char *o, fsinfo_t *f) { (void)o; (void)f; return 0; } \
void fs ## _cleanup_opts(fsinfo_t *f) { (void)f; }			\
void fs ## _makefs(const char *i, const char *d, fsnode *r, fsinfo_t *f)	\
{ (void)i; (void)d; (void)r; (void)f; errx(1, "only -t udf is built into udfmake"); }

NOT_BUILT(ffs)
NOT_BUILT(cd9660)
NOT_BUILT(chfs)
NOT_BUILT(v7fs)
NOT_BUILT(msdos)
