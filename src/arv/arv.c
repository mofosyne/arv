/*
 * arv: Archive, Record, Verify (see README.md). Every command; arv describe, tag and models run
 * arv-assist (src/arv-assist), and arv gui runs arv-gui (src/arv-gui).
 *
 *   arv info DISC                 the disc's record, binding and appraisals
 *   arv verify [-v] DISC          every file against the BagIt manifests; extra files in data/
 *   arv ls DISC                   the listing: files, executables and links
 *   arv restore [--no-links] DISC DEST
 *                                  copy data/ to DEST, checking every file on the way; restore
 *                                  modification times, execute bits and the source's links
 *   arv find [-C CATALOG] [--limit N] PATTERN
 *                                  discs, folder tags and files matching PATTERN, on every disc
 *                                  the catalogue knows (substring, or glob with * ? [)
 *   arv id [-C CATALOG] ID         explain a disc id; check its check character (catches typos)
 *   arv list [-C CATALOG] [--in CODE] [--at PLACE] [--made DATE] [--access LEVEL] [--covers DATE]
 *                                  the discs, one per line
 *
 * CATALOG is a disc root, its catalog/ folder, or a home (.arv); without -C: $ARV_HOME, or the
 * first .arv folder or disc root from the current folder up.
 *
 * DISC is the root of a mounted disc or an extracted image (the folder with catalog.rec). Needs
 * only C99 and POSIX: the same reading can be done by hand with sha256sum, cat and ln.
 */
#define _XOPEN_SOURCE 700
#include "arv.h"

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
          "Archive, Record, Verify: put what you keep on discs, record what exists and where, and check that\n"
          "it is still good and can be got back. arv make --help lists its options; docs/workflow.md says more.\n"
          "\n"
          "Archive: what goes on discs\n"
          "  arv make [--set CODE] [--title T] [--split] [--no-ecc] [--output-dir DIR] ... FOLDER\n"
          "         disc images of a folder (arv make --help lists every option)\n"
          "  arv plan new|list|show|add|move|drop|disc|make|again|refresh|delete NAME ...\n"
          "         compose discs by hand from files and folders anywhere (add --copy: the plan keeps a copy)\n"
          "  arv collection init [FOLDER] --code CODE --title TITLE [--set CODE] [--access LEVEL] | list | show CODE\n"
          "  arv collection keep CODE N   keep edition N: never offered for retiring\n"
          "  arv status [-v] [--deep] [--record [--message TEXT]] [FOLDER]   what changed, or what is archived\n"
          "  arv checkpoint [--message TEXT] [FOLDER]   record a workflow folder's state (no discs)\n"
          "  arv link FOLDER COLLECTION|DISC-ID [--past]   say what a folder is (logged)\n"
          "  arv names [--limit N] FOLDER   names a disc cannot hold, or Windows would show changed\n"
          "  arv describe FOLDER|DISC-ID [--save DRAFT] ...   title, description, tags from a local LLM (*)\n"
          "  arv tag FOLDER|DISC-ID [--save DRAFT] ...        folder tags from your vocabulary (*)\n"
          "  arv models fetch|status|build-runtime            the small model arv tag uses (*)\n"
          "\n"
          "Record: what exists, and where\n"
          "  arv burned (--device DRIVE | --copies N) [--media-id ID] [--location PLACE] [--note TEXT] [DISC-ID]\n"
          "         a burned copy (--device: read back against its image first; recorded only if identical;\n"
          "         --temperature hot|warm|cold: default the place's, else cold)\n"
          "  arv stored DISC-ID PATH [--location PLACE] [--temperature T]   a copy on a drive or NAS: the .iso,\n"
          "         or the disc's files as a folder; checked, then recorded (default: warm)\n"
          "  arv objects [NAME] [--json]   what you keep and where every copy is (data objects, collections)\n"
          "  arv find [-C CATALOG] [--limit N] PATTERN\n"
          "  arv list [-C CATALOG] [--in CODE] [--at PLACE] [--made DATE] [--access LEVEL] [--covers DATE]\n"
          "           [--unchecked-since AGE|DATE] [--one-place]   (AGE: 5y, 18m, 90d)\n"
          "  arv id [-C CATALOG] ID   arv info DISC   arv ls DISC\n"
          "  arv note DISC-ID TEXT   arv locate [--add] DISC-ID PLACE...   arv access DISC-ID public|private|sealed\n"
          "  arv location list [-v] | add CODE [NAME] [--in PARENT] [--description TEXT] [--temperature T]\n"
          "               | move CODE [NAME] [--in PARENT] [--temperature T]\n"
          "  arv selection list | show CODE | add|put|drop CODE [ITEM...] | move CODE [--in PARENT] [--name NAME]\n"
          "  arv appraise [TARGET] [--importance 'LEVEL for AUDIENCE']... [--basis TEXT] [--review DATE] [--due [DATE]]\n"
          "  arv log [COLLECTION]   arv diff REV [REV]   (REV: a revision's first digits, or CODE/N)\n"
          "  arv retire CODE [--yes [--accept-loss]]   retire the editions a newer safe one replaces (not kept ones)\n"
          "  arv sets [-v]   arv tags [--namespace NS] [--vocab FILE]   arv keywords [--format tsv|exiftool] DISC-ID\n"
          "\n"
          "Verify: still good, and can be got back\n"
          "  arv todo [--overdue YEARS]   what is owed: copies, read-backs, cold copies, places, checks\n"
          "  arv check (--image FILE [--repair] | --device DRIVE) [--note TEXT] [-v] [DISC-ID]\n"
          "  arv verify [-v] DISC   every file against its checksum\n"
          "  arv restore [--no-links] DISC DEST\n"
          "  arv rebuild [--prefer-disc] [--any-archive] DISC   the catalogue back from a disc\n"
          "\n"
          "The home\n"
          "  arv init [FOLDER] [--pointer HOME] [--name NAME [--default]]   arv where\n"
          "  arv gui   the interface, in your web browser (*)\n"
          "\n"
          "(*) when installed: arv-assist (the local AI helpers), arv-gui (the interface)\n"
          "DISC: the root of a mounted arv disc or an extracted image (the folder with catalog.rec)\n"
          "CATALOG: a disc root, its catalog/ folder or a home (.arv); default: $ARV_HOME, or the\n"
          "first .arv folder or disc root from here up\n",
          to);
}

/* ------------------------------------------------------------------ the helpers: arv-assist, arv-gui */

/* the command word after the global options, or NULL */
static const char *command_word(int argc, char **argv)
{
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if ((!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home") || !strcmp(argv[i], "--archive")) && i + 1 < argc) i += 2;
        else return NULL;
    }
    return i < argc ? argv[i] : NULL;
}

/* a helper program: $ENV, next to this arv (an installed bin/), or in the checkout this arv was
 * built in (src/arv/build/arv -> src/<name>/...); NULL: look on PATH */
static char *helper(const char *env, const char *name, const char *in_checkout)
{
    if (getenv(env) && *getenv(env)) return xstrdup(getenv(env));
    char *dir = exe_dir();
    if (!dir) return NULL;
    char *beside = join(dir, name), *built = xprintf("%s/../../%s", dir, in_checkout);
    char *found = !access(beside, X_OK) ? beside : !access(built, X_OK) ? built : NULL;
    found = found ? xstrdup(found) : NULL;
    free(beside);
    free(built);
    free(dir);
    return found;
}

/* arv describe|tag|models runs arv-assist; arv gui runs arv-gui (with the home this arv finds) */
static void run_helper(int argc, char **argv, const char *word)
{
    int gui = !strcmp(word, "gui");
    char *assist = helper("ARV_ASSIST", "arv-assist", "arv-assist/build/arv-assist");
    char *program = gui ? helper("ARV_GUI", "arv-gui", "arv-gui/arv-gui") : assist;
    char *self = exe_dir(), *me = self ? join(self, strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0]) : NULL;
    if (me && !access(me, X_OK)) setenv("ARV", me, 1);          /* arv-gui runs this arv */
    if (assist) setenv("ARV_ASSIST", assist, 1);
    const char *name = gui ? "arv-gui" : "arv-assist";
    char **args = xmalloc(((size_t)argc + 4) * sizeof *args);
    int n = 0;
    args[n++] = program ? program : (char *)name;
    if (gui) {                       /* the home, found here; then gui's own options */
        const char *given = NULL;
        int i = 1;
        for (; i < argc && strcmp(argv[i], "gui"); i += 2) {
            if (!strcmp(argv[i], "--archive")) home_archive_name = argv[i + 1];
            else given = argv[i + 1];
        }
        arv_home h;
        home_find(&h, given, NULL);
        args[n++] = "--home";
        args[n++] = h.path;
        for (i++; i < argc; i++) args[n++] = argv[i];
    } else {
        for (int k = 1; k < argc; k++) args[n++] = argv[k];
    }
    args[n] = NULL;
    execvp(args[0], args);
    fprintf(stderr, "Error: arv %s needs %s (%s), which was not found next to arv, in this checkout, or on PATH: "
                    "build and install it with make install%s\n", word, name,
            gui ? "arv's interface, in Python" : "arv's local AI helpers", gui ? " (and python3)" : "");
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
                 { "access", cmd_access }, { "location", cmd_location }, { "selection", cmd_selection }, { "collection", cmd_collection },
                 { "status", cmd_status }, { "checkpoint", cmd_checkpoint }, { "log", cmd_log }, { "diff", cmd_diff },
                 { "link", cmd_link }, { "todo", cmd_todo }, { "objects", cmd_objects }, { "retire", cmd_retire }, { "stored", cmd_stored },
                 { "appraise", cmd_appraise }, { "sets", cmd_sets }, { "names", cmd_names },
                 { "where", cmd_where }, { "rebuild", cmd_rebuild },
                 { "tags", cmd_tags }, { "keywords", cmd_keywords }, { "plan", cmd_plan } };
    arv_argv0 = argv[0];
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        puts(VERSION);
        return 0;
    }
    /* the local AI helpers and the interface are programs of their own */
    const char *word = command_word(argc, argv);
    if (word && (!strcmp(word, "describe") || !strcmp(word, "tag") || !strcmp(word, "models") || !strcmp(word, "gui")))
        run_helper(argc, argv, word);
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(argc < 2 ? stderr : stdout);
        return argc < 2 ? 2 : 0;
    }
    /* "arv -C HOME make ..." (as "arv --home HOME make ..."): the home goes to the command */
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
    usage(stderr);
    return 2;
}
