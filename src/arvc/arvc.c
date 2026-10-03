/*
 * arvc: arv's reader in C (research/plan.md, the C port; it becomes `arv` when the port is done).
 *
 *   arvc info DISC                 the disc's record, binding and appraisals
 *   arvc verify [-v] DISC          every file against the BagIt manifests; extra files in data/
 *   arvc ls DISC                   the listing: files, executables and links
 *   arvc restore [--no-links] DISC DEST
 *                                  copy data/ to DEST, checking every file on the way; restore
 *                                  modification times, execute bits and the source's links
 *   arvc find [-C CATALOG] [--limit N] PATTERN
 *                                  discs, folder tags and files matching PATTERN, on every disc
 *                                  the catalogue knows (substring, or glob with * ? [)
 *   arvc id [-C CATALOG] ID         explain a disc id; check its check character (catches typos)
 *   arvc list [-C CATALOG] [--in CODE] [--at PLACE] [--made DATE] [--access LEVEL] [--covers DATE]
 *                                  the discs, one per line
 *
 * CATALOG is a disc root, its catalog/ folder, or a home (.arv); without -C: $ARV_HOME, or the
 * first .arv folder or disc root from the current folder up.
 *
 * DISC is the root of a mounted disc or an extracted image (the folder with catalog.rec). Needs
 * only C99 and POSIX: the same reading can be done by hand with sha256sum, cat and ln.
 */
#define _POSIX_C_SOURCE 200809L
#include "arvc.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ main */

static void usage(void)
{
    fputs("usage: arvc info DISC\n"
          "       arvc verify [-v] DISC\n"
          "       arvc ls DISC\n"
          "       arvc restore [--no-links] DISC DEST\n"
          "       arvc find [-C CATALOG] [--limit N] PATTERN\n"
          "       arvc id [-C CATALOG] ID\n"
          "       arvc list [-C CATALOG] [--in CODE] [--at PLACE] [--made DATE] [--access LEVEL] [--covers DATE]\n"
          "DISC: the root of a mounted arv disc or an extracted image (the folder with catalog.rec)\n"
          "CATALOG: a disc root, its catalog/ folder or a home (.arv); default: $ARV_HOME, or the\n"
          "first .arv folder or disc root from here up\n",
          stderr);
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        int (*fn)(int, char **);
    } cmds[] = { { "info", cmd_info }, { "verify", cmd_verify }, { "ls", cmd_ls }, { "restore", cmd_restore },
                 { "find", cmd_find }, { "list", cmd_list }, { "id", cmd_id } };
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        puts(VERSION);
        return 0;
    }
    for (size_t i = 0; argc >= 2 && i < sizeof cmds / sizeof *cmds; i++)
        if (!strcmp(argv[1], cmds[i].name)) {
            int rc = cmds[i].fn(argc - 2, argv + 2);
            if (rc == 2) usage();
            return rc;
        }
    usage();
    return 2;
}
