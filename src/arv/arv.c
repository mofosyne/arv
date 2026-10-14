/*
 * arv: Archive, Record, Verify (see README.md). Every command; arv describe and tag run
 * arv-assist (src/arv-assist), and arv gui runs arv-gui (src/arv-gui).
 *
 *   arv info DISC|IMAGE|DISC-ID [--files]
 *                                  a disc's record, binding and appraisals (--files: its listing);
 *                                  which disc and copy an image is; what a disc id says and names
 *   arv restore DISC DEST
 *                                  copy data/ to DEST, checking every file on the way; restore
 *                                  modification times, execute bits and the source's links
 *   arv find [-C CATALOG] [--limit N] PATTERN
 *                                  discs, folder tags and files matching PATTERN, on every disc
 *                                  the catalogue knows (substring, or glob with * ? [)
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

/* in two parts: C99 compilers need take no single string over 4095 characters */
static const char USAGE_ARCHIVE[] =
          "usage: arv [-C HOME | --archive NAME] COMMAND ...\n"
          "Archive, Record, Verify: put what you keep on discs, record what exists and where, and check that\n"
          "it is still good and can be got back. arv make --help lists its options; docs/workflow.md says more.\n"
          "The commands come in four groups: arv archive, arv record, arv verify and arv home list theirs,\n"
          "and a group's name may come first (arv record burned is arv burned).\n"
          "\n"
          "Archive: what goes on discs\n"
          "  arv make [--set CODE] [--title T] [--split] [--no-ecc] [--output-dir DIR] ... FOLDER\n"
          "         disc images of a folder, recorded in the home (arv make --help lists every option)\n"
          "  arv plan new|list|show|add|rename|tag|layout|move|drop|disc|make|again|delete NAME ...\n"
          "         discs composed by hand from files and folders anywhere; the sources stay where they are:\n"
          "         add NAME SOURCE... [--disc N|new|auto] [--as PATH] [--each] puts them on a disc, under PATH\n"
          "         (--each: each file in a folder an object of its own);\n"
          "         add NAME --list FILE|- takes SOURCE<tab>PATH[<tab>DISC] lines, all or none;\n"
          "         rename NAME PATH NEWPATH gives an item a new place, or splits one off a planned folder;\n"
          "         tag NAME SOURCE TAGS (or --list FILE|-: SOURCE<tab>TAGS lines) tags files before they are on discs;\n"
          "         layout NAME 'year/event, year' [--disc N] makes a disc's tree from its objects' tags;\n"
          "         move NAME PATH... --disc N|new moves them; drop NAME PATH... takes them off; disc NAME add|drop N;\n"
          "         show NAME [--duplicates];"
          " make NAME makes the images; again NAME NEW starts the same plan anew; delete NAME\n"
          "  arv collection init [FOLDER] --code CODE --title TITLE [--set CODE] [--access LEVEL] | list | show CODE\n"
          "         a folder kept over time, its states made into editions of discs (show: its editions)\n"
          "  arv collection keep CODE N   keep edition N: never offered for retiring\n"
          "  arv status [-v] [--deep] [FOLDER]   what changed in a folder since it was archived, or what of it is\n"
          "         on discs (--deep: read every file again, not trusting sizes and times)\n"
          "  arv link FOLDER COLLECTION|DISC-ID [--past]   say what a folder is: a collection's workflow folder,\n"
          "         or a disc's source (--past: an older state, kept for reference); logged, nothing written in it\n"
          "  arv names [--limit N] FOLDER   names a disc cannot hold, or Windows would show changed\n"
          "  arv describe FOLDER|DISC-ID|OBJECT [--save|--apply DRAFT|--answer] ...   title, description, tags (*)\n"
          "  arv tag FOLDER|DISC-ID [--save DRAFT] ...   folder tags from your vocabulary (*)\n"
          "\n";
static const char USAGE_RECORD[] =
          "Record: what exists, and where\n"
          "  arv burned (--device DRIVE | --copies N) [--copy X] [--location PLACE] [--temperature T]\n"
          "             [--identifier ID | --barcode CODE]... [--media-type TYPE] [--note TEXT] [DISC-ID]\n"
          "         a burned copy, named by a letter (A, B ...: write it on the hub and case after the id;\n"
          "         --device: read back against its image first, recorded only if identical, with its BCA serial;\n"
          "         temperature hot|warm|cold: default the place's, else cold)\n"
          "  arv stored DISC-ID PATH [--location PLACE] [--temperature T] [--carrier barcode:CODE|serial:SERIAL] [--copy X]\n"
          "         a copy on a drive, NAS or tape: the .iso, or the disc's files as a folder; checked, then\n"
          "         recorded (default: warm); --copy X: that copy, moved here (to check one: arv check PATH)\n"
          "  arv identify DISC-ID COPY [ID...] [--barcode CODE] [--carrier ID]   what a copy is known by, or is on\n"
          "         (ID: bca:SERIAL, barcode:CODE or serial:SERIAL; a carrier is the tape or drive holding it)\n"
          "  arv relabel DISC-ID COPY NEW [--at PLACE | --identifier ID]   give a copy a new letter (when two\n"
          "         catalogues gave one letter to two copies: --at or --identifier says which)\n"
          "  arv locate DISC-ID COPY PLACE   where a copy is kept now\n"
          "  arv note DISC-ID TEXT   a note on a disc\n"
          "  arv rename DISC-ID PATH NEWPATH   a burned disc's file or folder called NEWPATH now, in the record\n"
          "  arv access DISC-ID public|private|sealed   what later discs' catalogues may show of it\n"
          "  arv objects [NAME|DISC-ID] [--json]   what you keep and where every copy is (a disc: its copies)\n"
          "  arv find [-C CATALOG] [--limit N] PATTERN   discs, copies (by identifier or carrier), objects, tags\n"
          "         and files, on every disc the catalogue knows (PATTERN: text, or a glob with * ? [)\n"
          "  arv list [-C CATALOG] [--in CODE] [--at PLACE] [--made DATE] [--access LEVEL] [--covers DATE]\n"
          "           [--unchecked-since AGE|DATE] [--one-place] [--json]   the discs, one a line (--in: a set or\n"
          "         category; --at: a place and the places in it; --covers: an EDTF date; AGE: 5y, 18m, 90d)\n"
          "  arv info [-C CATALOG] DISC|IMAGE|DISC-ID [--files]   a disc (--files: its listing), an image\n"
          "         (which disc and copy), or a disc id (its parts, its check character, the disc it names)\n"
          "  arv location list [-v] | add CODE [NAME] [--in PARENT] [--description TEXT] [--temperature T]\n"
          "               | move CODE [NAME] [--in PARENT] [--temperature T]\n"
          "         the places copies are kept: a tree (a box in a room in a house)\n"
          "  arv appraise [TARGET] [--importance 'LEVEL for AUDIENCE']... [--basis TEXT] [--review DATE] [--due [DATE]]\n"
          "         how much something matters and to whom (essential, important, useful); TARGET: DISC-ID,\n"
          "         DISC-ID:folder/, DISC-ID:file or set:CODE; --due: the appraisals due for review by DATE\n"
          "  arv log [COLLECTION]   a collection's editions, and what changed in each\n"
          "  arv diff REV [REV]   what changed between two revisions, or from one to the folder now\n"
          "         (REV: a revision's first digits, or CODE/N for edition N)\n"
          "  arv retire CODE|DISC-ID [--yes [--accept-loss]]   retire the editions a newer safe one replaces (not kept\n"
          "         ones), or one disc of no edition; refused while a file is on no disc that stays\n"
          "  arv sets [-v]   the set codes and categories, as a tree\n"
          "  arv tags [--namespace NS]   the tags in use, by namespace, on how many paths and discs; and the\n"
          "         paths to correct: two values of a one-valued namespace, a date that is not EDTF\n"
          "  arv keywords DISC-ID   a disc's folder tags as an exiftool argument file, to write into the files\n"
          "\n";
static const char USAGE_REST[] =
          "Verify: still good, and can be got back\n"
          "  arv todo [--overdue AGE] [--tsv]   what is owed: copies, read-backs, cold copies, places, checks\n"
          "         (--overdue: checks older than AGE, default 5y; --tsv: one copy a line, for scripts)\n"
          "  arv check [--repair | --drive] [--copy X] [--identifier ID | --barcode CODE]... [--note TEXT] [-v] SOURCE [DISC-ID]\n"
          "         a copy, against what arv made: SOURCE is a drive (every sector), an image file (--repair:\n"
          "         mend it with its RS03 data; --drive: a whole disc read, as ddrescue dumps one) or a disc's\n"
          "         files, mounted or copied (every file); logged, naming the copy when it is known\n"
          "  arv drives [--check] [DRIVE...]   what is in each optical drive: which disc, which copy\n"
          "  arv inventory [-v] PLACE [FILE]   a library's barcodes (mtx status, or a list) against the copies\n"
          "         recorded at PLACE: missing, unknown, recorded elsewhere\n"
          "  arv restore DISC DEST [--as-burned]   copy a disc's files to DEST, checking each; dates, execute bits\n"
          "         and links back; under the names the record gives them now (--as-burned: the disc's)\n"
          "  arv rebuild [--prefer-disc] [--any-archive] DISC|CATALOG   merge a disc's, or another .arv's, catalogue\n"
          "  arv audit [DISC...]   every event as written; with discs, the home against what they carry\n"
          "\n"
          "The home\n"
          "  arv init [FOLDER] [--name NAME [--default]]   make an archive: FOLDER/.arv (--name: known by name,\n"
          "         for arv --archive NAME; --default: the one used when no other is found)\n"
          "  arv where [--here]   which home arv uses, and why (--here: this copy of it may be written here too)\n"
          "  arv gui   the interface, in your web browser (*)\n"
          "  arv help [--all | --man | COMMAND]   --all: the whole manual (markdown), for people and agents alike\n"
          "\n"
          "(*) when installed: arv-assist (the local AI helpers), arv-gui (the interface)\n"
          "DISC: the root of a mounted arv disc or an extracted image (the folder with catalog.rec)\n"
          "CATALOG: a disc root, its catalog/ folder or a home (.arv); default: $ARV_HOME, or the\n"
          "first .arv folder or disc root from here up\n";
static const char *const USAGE[] = { USAGE_ARCHIVE, USAGE_RECORD, USAGE_REST, NULL };

/* arv alone: where you are, the commands to start with, and how to find the rest */
static int orientation(void)
{
    arv_home h;
    puts("arv: Archive, Record, Verify. Long-term archiving of what you choose to keep, on discs.\n");
    if (!home_try(&h, NULL, NULL)) printf("Archive here: %s (arv where says why)\n\n", h.path);
    else puts("No archive here yet: arv init FOLDER makes one, at the root of what it describes.\n");
    puts("Start with:\n"
         "  arv init FOLDER                    an archive (its record: FOLDER/.arv)\n"
         "  arv collection init FOLDER --code CODE --title TITLE\n"
         "                                     a folder you keep over time\n"
         "  arv make FOLDER                    its disc images (--no-ecc to try arv out quickly)\n"
         "  arv burned --device DRIVE          after burning one: read it back, record the copy\n"
         "  arv todo                           what is owed: copies, read-backs, checks\n"
         "\n"
         "Every command, by group: arv archive, arv record, arv verify, arv home. All of them: arv --help.\n"
         "In the browser: arv gui. The whole workflow: docs/workflow.md.");
    return 0;
}

static void usage(FILE *to)
{
    for (int part = 0; USAGE[part]; part++) fputs(USAGE[part], to);
}

/* "arv NAME" on a line of the usage, as a whole word */
static int names_command(const char *line, size_t len, const char *name)
{
    char *word = xprintf("arv %s", name);
    size_t n = strlen(word);
    int found = 0;
    for (const char *p = line; !found && p + n <= line + len; p++)
        found = !strncmp(p, word, n) && (p + n == line + len || p[n] == ' ');
    free(word);
    return found;
}

/* arv NAME --help: the lines of the usage for that command (with the lines continuing them), and the
 * words they use (DISC, CATALOG) */
static void command_usage(FILE *to, const char *name)
{
    sbuf out = { 0 };
    int in = 0;
    for (int part = 0; USAGE[part]; part++)
        for (const char *line = USAGE[part], *end; *line; line = end + 1) {
            end = strchr(line, '\n');
            size_t len = (size_t)(end - line);
            if (!strncmp(line, "  arv ", 6)) in = names_command(line, len, name);
            else if (strncmp(line, "    ", 4)) in = 0;     /* a continuation is indented further */
            if (in) sb_printf(&out, "%.*s\n", (int)len, line);
        }
    if (!out.s) {
        usage(to);
        return;
    }
    fputs(out.s, to);
    for (const char *p = out.s; (p = strstr(p, "DISC")); p += 4)
        if (p[4] != '-') {
            fputs("DISC: the root of a mounted arv disc or an extracted image (the folder with catalog.rec)\n", to);
            break;
        }
    if (strstr(out.s, "CATALOG"))
        fputs("CATALOG: a disc root, its catalog/ folder or a home (.arv); default: $ARV_HOME, or the\n"
              "first .arv folder or disc root from here up\n", to);
    fputs("All commands: arv --help. More: README.md and docs/workflow.md.\n", to);
    free(out.s);
}

/* ------------------------------------------------------------------ groups: arv archive|record|verify|home */

static const struct {
    const char *name, *header;          /* the header of its part of the usage */
} GROUPS[] = { { "archive", "Archive:" }, { "record", "Record:" }, { "verify", "Verify:" }, { "home", "The home" } };

/* a group's part of the usage (its header to the blank line after it); with cmd, only whether a
 * command line of that part names cmd */
static int group_lines(const char *header, const char *cmd, sbuf *out)
{
    int in = 0, found = 0;
    for (int part = 0; USAGE[part]; part++)
        for (const char *line = USAGE[part], *end; *line; line = end + 1) {
            end = strchr(line, '\n');
            size_t len = (size_t)(end - line);
            if (!strncmp(line, header, strlen(header))) in = 1;
            else if (!len) in = 0;
            if (!in) continue;
            if (out) sb_printf(out, "%.*s\n", (int)len, line);
            if (cmd && !strncmp(line, "  arv ", 6) && names_command(line, len, cmd)) found = 1;
        }
    return found;
}

/* ------------------------------------------------------------------ completion (arv __complete)
 * The shell scripts in completion/ ask arv what fits the word being typed, so completion follows
 * this arv's own usage and cannot drift: one candidate a line; none means "file names". */

static void candidate(const char *word, size_t len, const char *cur, strlist *seen)
{
    char *w = xprintf("%.*s", (int)len, word);
    if (!strncmp(w, cur, strlen(cur)) && !strlist_has(seen, w)) {
        strlist_add(seen, w);
        puts(w);
    }
    free(w);
}

/* the --options in a command's lines of the usage (and make's own help) */
static void option_candidates(const char *cmd, const char *cur, strlist *seen)
{
    sbuf lines = { 0 };
    int in = 0;
    for (int part = 0; USAGE[part]; part++)
        for (const char *line = USAGE[part], *end; *line; line = end + 1) {
            end = strchr(line, '\n');
            size_t len = (size_t)(end - line);
            if (!strncmp(line, "  arv ", 6)) in = names_command(line, len, cmd);
            else if (strncmp(line, "    ", 4)) in = 0;
            if (in) sb_printf(&lines, "%.*s\n", (int)len, line);
        }
    if (!strcmp(cmd, "make")) sb_puts(&lines, MAKE_HELP);
    for (const char *p = lines.s ? lines.s : ""; *p; p++) {
        if (!(p[0] == '-' && (p == lines.s || strchr(" [(|", p[-1])))) continue;
        size_t n = 1;
        while (p[n] == '-' || (p[n] >= 'a' && p[n] <= 'z') || (p[n] >= 'A' && p[n] <= 'Z') || (p[n] >= '0' && p[n] <= '9')) n++;
        if (n > 1) candidate(p, n, cur, seen);
        p += n - 1;
    }
    free(lines.s);
}

/* a command's own words: arv plan new|list|show ..., arv location list ... | add ... | move ...: the
 * first word of each part, split at | (not a placeholder in capitals, an option or [optional]) */
static void subcommand_candidates(const char *cmd, const char *cur, strlist *seen)
{
    char *word = xprintf("arv %s ", cmd);
    size_t wl = strlen(word);
    int in = 0;
    for (int part = 0; USAGE[part]; part++)
        for (const char *line = USAGE[part], *end; *line; line = end + 1) {
            end = strchr(line, '\n');
            const char *p = NULL;
            if (!strncmp(line, "  arv ", 6)) {
                in = !strncmp(line + 2, word, wl);      /* this command's own line, not one that names it later */
                p = in ? line + 2 + wl : NULL;
            } else if (strncmp(line, "    ", 4)) in = 0;
            else if (in) {                              /* a continuation: only its "| more" parts */
                p = line;
                while (p < end && *p == ' ') p++;
                if (p >= end || *p != '|') p = NULL;
            }
            for (int first = 1; p && p < end; first = 0) {
                if (!first || *p == '|') {               /* to the next part */
                    const char *bar = strstr(p, " | ");
                    if (*p == '|') bar = p - 1;
                    if (!bar || bar >= end) break;
                    p = bar + 3;
                    if (p > end) break;
                }
                const char *stop = p;
                while (stop < end && *stop != ' ') stop++;
                if (*p >= 'a' && *p <= 'z')
                    for (const char *q = p; q < stop;) {
                        const char *bar = memchr(q, '|', (size_t)(stop - q));
                        const char *e = bar ? bar : stop;
                        candidate(q, (size_t)(e - q), cur, seen);
                        q = e + 1;
                    }
                p = stop;
            }
        }
    free(word);
}

/* arv __complete WORD... CURRENT: what fits CURRENT after the words before it */
static int complete(int argc, char **argv, const char *const *names, size_t nnames)
{
    const char *cur = argc ? argv[argc - 1] : "";
    strlist seen = { 0 };
    int i = 0, n = argc - 1;                     /* the words before the current one */
    while (i < n && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home") || !strcmp(argv[i], "--archive"))) i += 2;
    if (i > n) return 0;                          /* the value of -C or --archive: file names */
    if (i == n) {                                 /* the command */
        static const char *const groups[] = { "archive", "record", "verify", "home" };
        for (size_t k = 0; k < sizeof groups / sizeof *groups; k++) candidate(groups[k], strlen(groups[k]), cur, &seen);
        for (size_t k = 0; k < nnames; k++) candidate(names[k], strlen(names[k]), cur, &seen);
        if (*cur == '-') {
            static const char *const opts[] = { "--help", "--version", "-C", "--archive" };
            for (size_t k = 0; k < sizeof opts / sizeof *opts; k++) candidate(opts[k], strlen(opts[k]), cur, &seen);
        }
        return 0;
    }
    for (size_t g = 0; g < sizeof GROUPS / sizeof *GROUPS; g++)
        if (!strcmp(argv[i], GROUPS[g].name)) {
            if (i + 1 == n) {                     /* a group's commands */
                for (size_t k = 0; k < nnames; k++)
                    if (group_lines(GROUPS[g].header, names[k], NULL)) candidate(names[k], strlen(names[k]), cur, &seen);
        
                return 0;
            }
            i++;
            break;
        }
    const char *cmd = argv[i];
    if (*cur == '-') option_candidates(cmd, cur, &seen);
    else if (i + 1 == n) subcommand_candidates(cmd, cur, &seen);
    return 0;
}

/* arv --version: the release, the commit it was built from when known, and the disc format */
static int print_version(void)
{
    int is_git;
    char *source = find_source(NULL), *v = software_version(source, &is_git);
    if (strcmp(v, "arv@unknown")) printf("%s (%s), disc format %s\n", VERSION, v, FORMAT_VERSION);
    else printf("%s, disc format %s\n", VERSION, FORMAT_VERSION);
    free(source);
    free(v);
    return 0;
}

static size_t edit_distance(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b), row[64];
    if (lb >= sizeof row / sizeof *row) return (size_t)-1;
    for (size_t j = 0; j <= lb; j++) row[j] = j;
    for (size_t i = 1; i <= la; i++) {
        size_t diag = row[0];
        row[0] = i;
        for (size_t j = 1; j <= lb; j++) {
            size_t up = row[j], best = diag + (a[i - 1] != b[j - 1]);
            if (up + 1 < best) best = up + 1;
            if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
            diag = up;
            row[j] = best;
        }
    }
    return row[lb];
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

/* arv describe|tag runs arv-assist; arv gui runs arv-gui (with the home this arv finds) */
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
        if (!home_try(&h, given, NULL)) {   /* none found: the interface offers to make one */
            args[n++] = "--home";
            args[n++] = h.path;
        }
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

/* ------------------------------------------------------------------ the whole manual (arv help --all, --man) */

/* What the usage lines do not say: for a person setting arv up, and for a script or an agent using
 * it. Markdown; arv help --man turns it, with the commands, into a man page. */
static const char MANUAL_NOTES[] =
    "## Reading and changing the record\n"
    "\n"
    "These only read: `list`, `find`, `objects`, `todo`, `status`, `log`, `diff`,\n"
    "`sets`, `tags`, `keywords`, `info`, `audit`, `inventory`, `drives` (without\n"
    "`--check`), `where` (without `--here`), `names`, `plan list|show`, `collection list|show`, `location\n"
    "list`, `appraise --due`, and `retire` without `--yes` (a preview). `restore`\n"
    "writes only the files it restores, where it is told.\n"
    "\n"
    "These change the record, each leaving an event that says who and when: `init`, `make`, `plan make`,\n"
    "`collection init|keep`, `link`, `burned`, `stored`, `identify`,\n"
    "`relabel`, `note`, `rename`, `locate`, `access`, `location add|move`, `appraise`,\n"
    "`retire --yes`, `rebuild`, `check` and `drives --check` (a check is logged), `describe` and `tag`\n"
    "(when a draft is applied). `where --here` changes only where the catalogue may be written.\n"
    "arv never deletes a file, an image or a disc.\n"
    "\n"
    "## Exit status\n"
    "\n"
    "- 0: done, and nothing found wrong.\n"
    "- 1: an error (`Error: ...` on standard error), or something found that needs a person: a check\n"
    "  that failed, `audit` or `inventory` findings, a disc id `rebuild` would not merge.\n"
    "- 2: the command was not used as it is meant to be; its usage follows on standard error.\n"
    "\n"
    "## Output for scripts\n"
    "\n"
    "- `arv objects --json`, `arv objects DISC-ID --json`: what is kept and every copy of it.\n"
    "- `arv todo --tsv`: what is owed, one copy a line, with a header line.\n"
    "- `arv keywords --format tsv|exiftool DISC-ID`: a disc's folder tags.\n"
    "- Lines that start with a fixed word: `find` (`DISC`, `OBJECT`, `COPY`, `GIT`, `TAG`, `LOST`, or a\n"
    "  disc id for a file), `audit` (`CHANGED`, `MISSING`, `DIFFERS`, `TWICE`, `TWO DISCS`, `TWO BCA`,\n"
    "  `CONFLICT`), `inventory` (`MISSING`, `UNKNOWN`, `ELSEWHERE`, `UNMARKED`).\n"
    "- The catalogue itself: `.arv/catalog/archive.rec` is a GNU recutils recfile (plain text; `recsel`\n"
    "  reads it), and each disc's file list is `catalog/volumes/DISC-ID/listing.tsv`.\n"
    "\n"
    "These are not yet a stable interface: they may change before 1.0.\n"
    "\n"
    "## For scripts and agents\n"
    "\n"
    "- Read through arv, or read the recfiles directly. Change the record only through arv: an event\n"
    "  edited by hand no longer matches its `EventId`, and `arv audit` reports it.\n"
    "- To describe discs, folders and data objects, write a draft (JSON; README.md, \"Describing from\n"
    "  anywhere\") and apply it: `arv describe ID --apply DRAFT` (`-` reads standard input). Add\n"
    "  `--suggested` when no person reviewed it, so the record says so.\n"
    "- Ask the person before what cannot be taken back or changes who may see what: `retire --yes`\n"
    "  (above all with `--accept-loss`), `access ... public|sealed`, `relabel` (a label must be\n"
    "  rewritten by hand), `where --here`.\n"
    "- Work on the archive's own catalogue, not a copy of it: a copy refuses changes (`arv where` says\n"
    "  which is which). Another catalogue of the archive is merged with `arv rebuild OTHER/.arv`.\n"
    "- `make` and the checks read and write whole disc images: minutes to an hour each. `--no-ecc`\n"
    "  makes small images for trying arv out, not for burning.\n"
    "- Every disc carries arv's source and these docs in `tools/`, and `README.txt` says how to read it\n"
    "  without arv.\n"
    "\n"
    "## Files\n"
    "\n"
    "- `.arv/`: the archive's home, at the root of the tree it describes; found by walking up from the\n"
    "  current folder, else the default home (`arv where`). `catalog/archive.rec`: every disc, copy,\n"
    "  place, collection and event. `catalog/volumes/DISC-ID/`: each disc's manifest and file list.\n"
    "  `config/`: the vocabularies, and `paths.rec`, where the catalogue may be written. `drafts/`:\n"
    "  disc plans and waiting questions. `cache/`: rebuildable.\n"
    "- `~/.config/arv/homes.rec`: named homes on this machine.\n"
    "\n"
    "## See also\n"
    "\n"
    "README.md, docs/workflow.md (the whole workflow), docs/concepts.md, docs/philosophy.md, and\n"
    "docs/spec/smart-archive-format.md (the disc format); on every disc, under `tools/`.\n";

/* the commands, group by group, from the usage text --help prints: one source for both */
static void manual_md(sbuf *out)
{
    sb_puts(out, "# arv(1): Archive, Record, Verify\n\n");
    sb_printf(out, "%s, disc format %s.\n\n", VERSION, FORMAT_VERSION);
    sbuf all = { 0 };
    for (int part = 0; USAGE[part]; part++) sb_puts(&all, USAGE[part]);
    const char *p = all.s;
    const char *nl = strchr(p, '\n');               /* the usage: line is the synopsis */
    sb_puts(out, "## Synopsis\n\n```\n");
    sb_printf(out, "%.*s\n", (int)(nl - p - 7), p + 7);
    sb_puts(out, "arv GROUP COMMAND ...   (archive, record, verify, home)\narv help [--all | --man | COMMAND]\n```\n\n");
    p = nl + 1;
    sb_puts(out, "## Description\n\n");                 /* the lines up to the first blank one */
    while (*p && *p != '\n') {
        nl = strchr(p, '\n');
        sb_printf(out, "%.*s\n", (int)(nl - p), p);
        p = nl + 1;
    }
    sb_puts(out, "\n## Commands\n");
    while (*p == '\n') p++;
    while (*p) {                                        /* each group: a header line, then its commands */
        nl = strchr(p, '\n');
        int heading = strncmp(p, "  ", 2) != 0;
        if (!strncmp(p, "(*)", 3)) {                    /* the notes after the last group: (*), DISC:, CATALOG: */
            sb_puts(out, "\n### Words\n\n```\n");
            while (*p) {
                nl = strchr(p, '\n');
                sb_printf(out, "%.*s\n", (int)(nl - p), p);
                p = nl + 1;
            }
            sb_puts(out, "```\n");
            break;
        }
        if (heading) {
            sb_printf(out, "\n### %.*s\n\n```\n", (int)(nl - p), p);
            p = nl + 1;
            while (*p && *p != '\n') {
                nl = strchr(p, '\n');
                if (strncmp(p, "  ", 2)) break;         /* the notes after the last group */
                sb_printf(out, "%.*s\n", (int)(nl - p), p);
                p = nl + 1;
            }
            sb_puts(out, "```\n");
            if (*p && *p != '\n' && strncmp(p, "  ", 2)) {          /* (*), DISC:, CATALOG: */
                sb_puts(out, "\n### Words\n\n");
                while (*p) {
                    nl = strchr(p, '\n');
                    sb_printf(out, "%.*s\n", (int)(nl - p), p);
                    p = nl + 1;
                }
            }
        }
        while (*p == '\n') p++;
    }
    sb_puts(out, "\n### arv make in full\n\n```\n");
    sb_puts(out, MAKE_HELP);
    sb_puts(out, "```\n\n");
    sb_puts(out, MANUAL_NOTES);
    free(all.s);
}

/* roff text: backslashes escaped, a leading . or ' kept from being a request, `code` bold */
static void roff_text(FILE *to, const char *s, size_t n, int code_spans)
{
    if (n && (s[0] == '.' || s[0] == '\'')) fputs("\\&", to);
    int bold = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\') fputs("\\e", to);
        else if (s[i] == '-') fputs("\\-", to);
        else if (code_spans && s[i] == '`') fputs((bold = !bold) ? "\\fB" : "\\fR", to);
        else fputc(s[i], to);
    }
    if (bold) fputs("\\fR", to);
}

/* the manual as a man page: the same markdown, turned to roff line by line */
static void manual_man(FILE *to)
{
    sbuf md = { 0 };
    manual_md(&md);
    char date[11];
    today_iso(date);
    fprintf(to, ".TH ARV 1 \"%s\" \"%s\" \"arv manual\"\n.SH NAME\narv \\- Archive, Record, Verify: long-term archiving on "
                "self-describing discs\n", date, VERSION);
    int code = 0, para = 0;
    for (const char *p = md.s, *nl; *p; p = nl + 1) {
        nl = strchr(p, '\n');
        size_t n = (size_t)(nl - p);
        if (!strncmp(p, "```", 3)) {
            fputs(code ? ".fi\n.RE\n" : ".PP\n.RS 2\n.nf\n", to);
            code = !code;
        } else if (code) {
            roff_text(to, p, n, 0);
            fputc('\n', to);
        } else if (!strncmp(p, "# ", 2)) {
            continue;                                   /* the title: .TH and NAME say it */
        } else if (!strncmp(p, "## ", 3)) {
            fputs(".SH ", to);
            for (size_t i = 3; i < n; i++) fputc(toupper((unsigned char)p[i]), to);
            fputc('\n', to);
            para = 0;
        } else if (!strncmp(p, "### ", 4)) {
            fputs(".SS ", to);
            roff_text(to, p + 4, n - 4, 0);
            fputc('\n', to);
            para = 0;
        } else if (!n) {
            para = 1;
        } else if (!strncmp(p, "- ", 2)) {
            fputs(".IP \\(bu 2\n", to);
            roff_text(to, p + 2, n - 2, 1);
            fputc('\n', to);
            para = 0;
        } else {
            if (para) fputs(".PP\n", to);
            para = 0;
            while (n && *p == ' ') p++, n--;            /* a list item's continuation */
            roff_text(to, p, n, 1);
            fputc('\n', to);
        }
    }
    free(md.s);
}

/* arv help [--all | --man | COMMAND] */
static int cmd_help(int argc, char **argv)
{
    if (argc == 0) {
        usage(stdout);
        return 0;
    }
    if (argc == 1 && !strcmp(argv[0], "--all")) {
        sbuf md = { 0 };
        manual_md(&md);
        fputs(md.s, stdout);
        free(md.s);
        return 0;
    }
    if (argc == 1 && !strcmp(argv[0], "--man")) {
        manual_man(stdout);
        return 0;
    }
    if (argc == 1 && argv[0][0] != '-') {
        if (!strcmp(argv[0], "make")) fputs(MAKE_HELP, stdout);
        else command_usage(stdout, argv[0]);
        return 0;
    }
    return 2;
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        int (*fn)(int, char **);
    } cmds[] = { { "info", cmd_info }, { "restore", cmd_restore },
                 { "find", cmd_find }, { "list", cmd_list },
                 { "init", cmd_init }, { "make", cmd_make }, { "check", cmd_check },
                 { "burned", cmd_burned }, { "note", cmd_note }, { "rename", cmd_rename }, { "locate", cmd_locate },
                 { "access", cmd_access }, { "location", cmd_location }, { "collection", cmd_collection },
                 { "status", cmd_status }, { "log", cmd_log }, { "diff", cmd_diff },
                 { "link", cmd_link }, { "todo", cmd_todo }, { "objects", cmd_objects }, { "retire", cmd_retire }, { "stored", cmd_stored },
                 { "appraise", cmd_appraise }, { "sets", cmd_sets }, { "names", cmd_names },
                 { "where", cmd_where }, { "rebuild", cmd_rebuild }, { "audit", cmd_audit }, { "drives", cmd_drives }, { "relabel", cmd_relabel }, { "inventory", cmd_inventory }, { "identify", cmd_identify },
                 { "tags", cmd_tags }, { "keywords", cmd_keywords }, { "plan", cmd_plan } };
    arv_argv0 = argv[0];
    if (argc >= 2 && !strcmp(argv[1], "__complete")) {     /* for the shell's completion (completion/) */
        const char *names[sizeof cmds / sizeof *cmds + 5];
        size_t nnames = 0;
        for (size_t i = 0; i < sizeof cmds / sizeof *cmds; i++) names[nnames++] = cmds[i].name;
        names[nnames++] = "describe", names[nnames++] = "tag", names[nnames++] = "gui";
        names[nnames++] = "help";
        return complete(argc - 2, argv + 2, names, nnames);
    }
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        return print_version();
    }
    /* a group first: arv record burned ... is arv burned ...; arv record alone lists the group */
    int at = 1;
    while (at + 1 < argc && (!strcmp(argv[at], "-C") || !strcmp(argv[at], "--home") || !strcmp(argv[at], "--archive"))) at += 2;
    for (size_t g = 0; at < argc && g < sizeof GROUPS / sizeof *GROUPS; g++) {
        if (strcmp(argv[at], GROUPS[g].name)) continue;
        const char *sub = at + 1 < argc ? argv[at + 1] : NULL, *group = GROUPS[g].name;
        int known = 0;
        if (!sub || !strcmp(sub, "-h") || !strcmp(sub, "--help")) {
            sbuf out = { 0 };
            group_lines(GROUPS[g].header, NULL, &out);
            printf("%s(arv %s COMMAND or arv COMMAND; arv COMMAND --help says more; arv --help lists every group)\n",
                   out.s, group);
            free(out.s);
            return 0;
        }
        for (size_t i = 0; i < sizeof cmds / sizeof *cmds; i++) known |= !strcmp(sub, cmds[i].name);
        known |= !strcmp(sub, "describe") || !strcmp(sub, "tag") || !strcmp(sub, "gui");
        if (group_lines(GROUPS[g].header, sub, NULL)) {
        } else if (known) {
            for (size_t h = 0; h < sizeof GROUPS / sizeof *GROUPS; h++)
                if (group_lines(GROUPS[h].header, sub, NULL)) {
                    fprintf(stderr, "arv %s %s: %s is in %s (arv %s, or arv %s %s)\n", group, sub, sub, GROUPS[h].name, sub,
                            GROUPS[h].name, sub);
                    return 2;
                }
        } else {
            fprintf(stderr, "arv %s: no command %s (arv %s lists them)\n", group, sub, group);
            return 2;
        }
        for (int k = at; k + 1 < argc; k++) argv[k] = argv[k + 1];       /* the group's name goes */
        argv[--argc] = NULL;
        break;
    }
    /* the local AI helpers and the interface are programs of their own */
    const char *word = command_word(argc, argv);
    if (word && (!strcmp(word, "describe") || !strcmp(word, "tag") || !strcmp(word, "gui")))
        run_helper(argc, argv, word);
    if (argc < 2) return orientation();
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(stdout);
        return 0;
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
                        command_usage(stdout, cmds[i].name);
                        return 0;
                    }
            char **args = argv + 2;
            int n = argc - 2;
            if (home) {                     /* the command reads -C HOME first (restore: for moves in its record) */
                args = xmalloc(((size_t)n + 3) * sizeof *args);
                args[0] = "-C";
                args[1] = (char *)home;
                for (int k = 0; k < n; k++) args[k + 2] = argv[k + 2];
                args[n + 2] = NULL;
                n += 2;
            }
            int rc = cmds[i].fn(n, args);
            if (rc == 2) {          /* the command did not take its arguments: say so, and how it is used */
                fprintf(stderr, "arv %s: unexpected or missing arguments:", cmds[i].name);
                for (int k = 2; k < argc; k++) fprintf(stderr, " %s", argv[k]);
                fputs(argc > 2 ? "\n" : " (none given)\n", stderr);
                command_usage(stderr, cmds[i].name);
            }
            return rc;
        }
    if (argc >= 2 && !strcmp(argv[1], "version")) {
        return print_version();
    }
    if (argc >= 2 && !strcmp(argv[1], "help")) {
        int rc = cmd_help(argc - 2, argv + 2);
        if (rc == 2) fputs("usage: arv help [--all | --man | COMMAND]\n", stderr);
        return rc;
    }
    /* an unknown command: say which, and the nearest ones */
    static const char *const helpers[] = { "describe", "tag", "gui" };
    const char *near[3];
    size_t nnear = 0, cutoff = strlen(argv[1]) <= 4 ? 1 : 2;
    for (size_t i = 0; i < sizeof cmds / sizeof *cmds + sizeof helpers / sizeof *helpers && nnear < 3; i++) {
        const char *name = i < sizeof cmds / sizeof *cmds ? cmds[i].name : helpers[i - sizeof cmds / sizeof *cmds];
        if (edit_distance(argv[1], name) <= cutoff || (strlen(argv[1]) >= 3 && !strncmp(argv[1], name, strlen(argv[1]))))
            near[nnear++] = name;
    }
    fprintf(stderr, "arv: unknown %s '%s'", argv[1][0] == '-' ? "option" : "command", argv[1]);
    for (size_t k = 0; k < nnear; k++) fprintf(stderr, "%s%s", k ? ", " : " - did you mean ", near[k]);
    fputs(nnear ? "?\n" : "\n", stderr);
    fputs("arv --help lists every command.\n", stderr);
    return 2;
}
