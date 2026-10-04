/* Library entry points around NetBSD makefs (compiled with main renamed to netbsd_makefs_main). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "udfmake.h"

int netbsd_makefs_main(int argc, char *argv[]);

int
udfmake_main(int argc, char *argv[])
{
	char **args = calloc((size_t)argc + 3, sizeof(char *));
	int i, n = 0, rc;

	if (args == NULL)
		return 1;
	args[n++] = argc > 0 ? argv[0] : "udfmake";
	args[n++] = "-t";
	args[n++] = "udf";
	for (i = 1; i < argc; i++)
		args[n++] = argv[i];
	optind = 1;		/* makefs uses getopt; allow repeated calls */
	rc = netbsd_makefs_main(n, args);
	free(args);
	return rc;
}

int
udfmake(const char *image, const char *dir, const char *options)
{
	char *argv[6];
	int argc = 0;

	argv[argc++] = "udfmake";
	if (options != NULL) {
		argv[argc++] = "-o";
		argv[argc++] = (char *)options;
	}
	argv[argc++] = (char *)image;
	argv[argc++] = (char *)dir;
	argv[argc] = NULL;
	return udfmake_main(argc, argv);
}
