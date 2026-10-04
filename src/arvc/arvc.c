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
#define _XOPEN_SOURCE 700
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
    fputs("usage: arvc init [FOLDER]\n"
          "       arvc make [-C HOME] [--set CODE] [--title T] [--no-ecc] [--output-dir DIR] ... FOLDER\n"
          "              (arvc make --help lists every option)\n"
          "       arvc check [-C HOME] (--image FILE | --device DRIVE) [--note TEXT] [-v] [DISC-ID]\n"
          "       arvc burned [-C HOME] [--copies N] [--media-id ID] [--location PLACE] [--note TEXT] DISC-ID\n"
          "       arvc note [-C HOME] DISC-ID TEXT\n"
          "       arvc locate [-C HOME] [--add] DISC-ID PLACE...\n"
          "       arvc access DISC-ID public|private|sealed\n"
          "       arvc location list [-v] | add CODE [NAME] [--in PARENT] [--description TEXT] | move CODE [NAME] [--in PARENT]\n"
          "       arvc collection list | show CODE | add|put|drop CODE [ITEM...] | move CODE [--in PARENT] [--name NAME]\n"
          "       arvc appraise [TARGET] [--importance 'LEVEL for AUDIENCE']... [--basis TEXT] [--review DATE] [--due [DATE]]\n"
          "       arvc sets [-v]\n"
          "       arvc names [--filesystem hybrid|udf250] [--limit N] FOLDER\n"
          "       arvc where\n"
          "       arvc info DISC\n"
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

/* ------------------------------------------------------------------ arv: C first, Python for the rest */

static int has_arg(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], name)) return 1;
    return 0;
}

static const char *arg_value(int argc, char **argv, const char *name)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], name)) return argv[i + 1];
    return NULL;
}

/* Does this command line need the Python arv? (a command or option arvc does not have, the
 * prompts of an interactive make, or help text) */
static int needs_python(int argc, char **argv)
{
    int i = 1;
    if (argc < 2) return 1;
    while (i < argc && argv[i][0] == '-') {            /* global options */
        if ((!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home")) && i + 1 < argc) i += 2;
        else return 1;                                 /* --archive, --help, ... */
    }
    if (i >= argc) return 1;
    const char *cmd = argv[i];
    static const char *const ported[] = { "init", "where", "make", "names", "find", "list", "sets", "id", "note",
                                          "locate", "location", "appraise", "collection", "access", "check",
                                          "burned", "info", "verify", "ls", "restore", NULL };
    int known = 0;
    for (int k = 0; ported[k]; k++) known |= !strcmp(cmd, ported[k]);
    if (!known) return 1;
    if (has_arg(argc, argv, "-h") || has_arg(argc, argv, "--help")) return 1;
    if (strcmp(cmd, "make")) return 0;
    static const char *const python_only[] = { "--split", "--llm", "--llm-rounds", "--draft", "--ro-crate",
                                               "--extra-tools", "--tools-history", "--sf-home", "--udfmake",
                                               "--llm-url", "--llm-model", "--llm-allow-remote", "--vision",
                                               "--vision-model", "--vision-url", "--vision-per-folder",
                                               "--vision-max", NULL };
    for (int k = 0; python_only[k]; k++)
        if (has_arg(argc, argv, python_only[k])) return 1;
    const char *fs = arg_value(argc, argv, "--filesystem"), *writer = arg_value(argc, argv, "--udf-writer"),
               *formats = arg_value(argc, argv, "--formats");
    if (fs && strcmp(fs, "udf250")) return 1;
    if (writer && strcmp(writer, "udfwrite")) return 1;
    if (formats && !strcmp(formats, "yes")) return 1;
    if ((!formats || strcmp(formats, "no")) && on_path("sf")) return 1;    /* Python identifies formats */
    if (isatty(0) && !has_arg(argc, argv, "-y") && !has_arg(argc, argv, "--yes")) return 1;   /* it asks */
    return 0;
}

/* runs the Python arv with the same arguments: arv-py on PATH, or the tree next to this program */
static void run_python(int argc, char **argv)
{
    char *dir = exe_dir(), *python_entry = NULL;
    const char *candidates[] = { getenv("ARV_PYTHON_ENTRY"), NULL, NULL };
    char *installed = dir ? xprintf("%s/../share/arv/arv", dir) : NULL, *checkout = dir ? xprintf("%s/../../../arv", dir) : NULL;
    candidates[1] = installed;
    candidates[2] = checkout;
    for (int k = 0; k < 3 && !python_entry; k++)
        if (candidates[k] && *candidates[k] && !access(candidates[k], R_OK)) python_entry = (char *)candidates[k];
    char **args = xmalloc(((size_t)argc + 2) * sizeof *args);
    int n = 0;
    if (python_entry) {
        args[n++] = "python3";
        args[n++] = python_entry;
    } else {
        args[n++] = "arv-py";
    }
    for (int k = 1; k < argc; k++) args[n++] = argv[k];
    args[n] = NULL;
    execvp(args[0], args);
    fprintf(stderr, "Error: this needs the Python arv (%s), which was not found: install it with make install\n",
            python_entry ? "python3" : "arv-py");
    exit(1);
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        int (*fn)(int, char **);
    } cmds[] = { { "info", cmd_info }, { "verify", cmd_verify }, { "ls", cmd_ls }, { "restore", cmd_restore },
                 { "find", cmd_find }, { "list", cmd_list }, { "id", cmd_id },
                 { "init", cmd_init }, { "make", cmd_make }, { "check", cmd_check },
                 { "burned", cmd_burned }, { "note", cmd_note }, { "locate", cmd_locate },
                 { "access", cmd_access }, { "location", cmd_location }, { "collection", cmd_collection },
                 { "appraise", cmd_appraise }, { "sets", cmd_sets }, { "names", cmd_names },
                 { "where", cmd_where } };
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        puts(VERSION);
        return 0;
    }
    /* called as arv: what is ported runs here, the rest in the Python arv */
    const char *self = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    if (!strcmp(self, "arv") && needs_python(argc, argv)) run_python(argc, argv);
    /* "arvc -C HOME make ..." (as "arv --home HOME make ..."): the home goes to the command */
    const char *home = NULL;
    if (argc >= 3 && (!strcmp(argv[1], "-C") || !strcmp(argv[1], "--home"))) {
        home = argv[2];
        argv += 2;
        argc -= 2;
    }
    for (size_t i = 0; argc >= 2 && i < sizeof cmds / sizeof *cmds; i++)
        if (!strcmp(argv[1], cmds[i].name)) {
            char **args = argv + 2;
            int n = argc - 2;
            if (home) {             /* the command reads -C HOME first */
                args = xmalloc(((size_t)n + 3) * sizeof *args);
                args[0] = "-C";
                args[1] = (char *)home;
                for (int k = 0; k < n; k++) args[k + 2] = argv[k + 2];
                args[n + 2] = NULL;
                n += 2;
            }
            int rc = cmds[i].fn(n, args);
            if (rc == 2) usage();
            return rc;
        }
    usage();
    return 2;
}
