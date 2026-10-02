/* udfmake: build a UDF image (up to 2.60, e.g. Blu-ray UDF 2.50) from a folder.
 *
 * A library wrapper around NetBSD's `makefs -t udf` (see README.md).
 * Upstream reports errors with err(3), which exits the process, so run it in a
 * child process (the udfmake program) if the caller must survive failures.
 */
#ifndef UDFMAKE_H
#define UDFMAKE_H

/* Build `image` from the contents of `dir`. `options` is makefs's -o list,
 * e.g. "T=bdrom,v=2.50,V=2.50,L=MYDISC" (NULL for defaults). Returns 0 on success. */
int udfmake(const char *image, const char *dir, const char *options);

/* Same as the udfmake program: makefs arguments, "-t udf" implied. */
int udfmake_main(int argc, char *argv[]);

#endif
