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

static void usage(FILE *to)
{
    fputs("usage: arv [-C HOME | --archive NAME] COMMAND ...\n"
          "       arv init [FOLDER] [--pointer HOME] [--name NAME [--default]]\n"
          "       arv make [-C HOME] [--set CODE] [--title T] [--no-ecc] [--output-dir DIR] ... FOLDER\n"
          "              (arv make --help lists every option)\n"
          "       arv check [-C HOME] (--image FILE | --device DRIVE) [--note TEXT] [-v] [DISC-ID]\n"
          "       arv burned [-C HOME] [--copies N] [--media-id ID] [--location PLACE] [--note TEXT] DISC-ID\n"
          "       arv note [-C HOME] DISC-ID TEXT\n"
          "       arv locate [-C HOME] [--add] DISC-ID PLACE...\n"
          "       arv access DISC-ID public|private|sealed\n"
          "       arv location list [-v] | add CODE [NAME] [--in PARENT] [--description TEXT] | move CODE [NAME] [--in PARENT]\n"
          "       arv collection list | show CODE | add|put|drop CODE [ITEM...] | move CODE [--in PARENT] [--name NAME]\n"
          "       arv appraise [TARGET] [--importance 'LEVEL for AUDIENCE']... [--basis TEXT] [--review DATE] [--due [DATE]]\n"
          "       arv sets [-v]\n"
          "       arv names [--limit N] FOLDER\n"
          "       arv where\n"
          "       arv tags [--namespace NS] [--vocab FILE]\n"
          "       arv keywords [--format tsv|exiftool] DISC-ID\n"
          "       arv rebuild [--prefer-disc] DISC   merge a disc's catalogue into the home\n"
          "       arv info DISC\n"
          "       arv verify [-v] DISC\n"
          "       arv ls DISC\n"
          "       arv restore [--no-links] DISC DEST\n"
          "       arv find [-C CATALOG] [--limit N] PATTERN\n"
          "       arv id [-C CATALOG] ID\n"
          "       arv list [-C CATALOG] [--in CODE] [--at PLACE] [--made DATE] [--access LEVEL] [--covers DATE]\n"
          "with the Python add-on (make install puts it in place):\n"
          "       arv describe FOLDER|DISC-ID [--save DRAFT] ...   title, description, tags from a local LLM\n"
          "       arv tag FOLDER|DISC-ID [--save DRAFT] ...        folder tags from your vocabulary\n"
          "       arv models fetch|status, arv gui\n"
          "DISC: the root of a mounted arv disc or an extracted image (the folder with catalog.rec)\n"
          "CATALOG: a disc root, its catalog/ folder or a home (.arv); default: $ARV_HOME, or the\n"
          "first .arv folder or disc root from here up\n",
          to);
}

/* ------------------------------------------------------------------ arv: C first, Python for the rest */

/* Is this one of the Python add-on's commands (the local AI helpers, gui)? */
static int needs_python(int argc, char **argv)
{
    int i = 1;
    while (i < argc && argv[i][0] == '-') {            /* global options */
        if ((!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home") || !strcmp(argv[i], "--archive")) && i + 1 < argc) i += 2;
        else return 0;
    }
    if (i >= argc) return 0;
    static const char *const addon[] = { "describe", "tag", "models", "gui", NULL };
    for (int k = 0; addon[k]; k++)
        if (!strcmp(argv[i], addon[k])) return 1;
    return 0;
}

/* runs the Python arv with the same arguments: arv-py on PATH, or the tree next to this program */
static void run_python(int argc, char **argv)
{
    /* $ARV_PYTHON_ENTRY, an installed share/arv/arv, arv/arv next to this program (on a disc), or the
     * checkout it was built in (a few folders up) */
    char *dir = exe_dir(), *python_entry = NULL;
    struct stat st;
    if (getenv("ARV_PYTHON_ENTRY") && *getenv("ARV_PYTHON_ENTRY")) python_entry = xstrdup(getenv("ARV_PYTHON_ENTRY"));
    for (int k = 0; dir && !python_entry && k < 6; k++) {
        char *root = k == 0 ? xprintf("%s/../share/arv", dir) : k == 1 ? xprintf("%s/arv", dir) : NULL;
        if (k >= 2) {
            sbuf up = { 0 };
            sb_puts(&up, dir);
            for (int j = 1; j < k; j++) sb_puts(&up, "/..");
            root = up.s;
        }
        char *script = join(root, "arv"), *cli = join(root, "src/arv/cli.py");
        if (!stat(script, &st) && S_ISREG(st.st_mode) && !access(cli, R_OK)) python_entry = xstrdup(script);
        free(script);
        free(cli);
        free(root);
    }
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
    fprintf(stderr, "Error: this command is in arv's optional Python add-on (the AI helpers, gui), which was not "
                    "found (%s): install it with make install\n", python_entry ? "python3" : "arv-py");
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
                 { "where", cmd_where }, { "rebuild", cmd_rebuild },
                 { "tags", cmd_tags }, { "keywords", cmd_keywords } };
    arv_argv0 = argv[0];
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        puts(VERSION);
        return 0;
    }
    /* called as arv (or arv.com): the add-on's commands run in Python */
    const char *self = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    if ((!strcmp(self, "arv") || !strcmp(self, "arv.com")) && needs_python(argc, argv)) run_python(argc, argv);
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(argc < 2 ? stderr : stdout);
        return argc < 2 ? 2 : 0;
    }
    /* "arvc -C HOME make ..." (as "arv --home HOME make ..."): the home goes to the command */
    const char *home = NULL;
    while (argc >= 3 && (!strcmp(argv[1], "-C") || !strcmp(argv[1], "--home") || !strcmp(argv[1], "--archive"))) {
        if (!strcmp(argv[1], "--archive")) home_archive_name = argv[2];
        else home = argv[2];
        argv += 2;
        argc -= 2;
    }
    for (size_t i = 0; argc >= 2 && i < sizeof cmds / sizeof *cmds; i++)
        if (!strcmp(argv[1], cmds[i].name)) {
            if (strcmp(cmds[i].name, "make"))         /* make has its own help */
                for (int k = 2; k < argc; k++)
                    if (!strcmp(argv[k], "-h") || !strcmp(argv[k], "--help")) {
                        usage(stdout);
                        return 0;
                    }
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
            if (rc == 2) usage(stderr);
            return rc;
        }
    if (argc >= 2 && needs_python(argc, argv))
        fprintf(stderr, "Error: %s is in arv's optional Python add-on: run it as arv (or ./arv in a checkout)\n", argv[1]);
    else
        usage(stderr);
    return 2;
}
