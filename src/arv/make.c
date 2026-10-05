/*
 * arv make: disc images from a folder, without prompts (src/arv/cli.py cmd_make and
 * src/arv/make.py Maker, UDF 2.50; --split spreads a folder over as many discs as needed). The disc it writes is the same as the Python arv's: BagIt
 * tag files, the catalogue snapshot, catalog.rec, README.txt, index.html and tools/, the image
 * written by udfwrite (linked in), and RS03 error correction (src/rs03, linked in: dvdisaster's format).
 *
 * Not ported (use the Python arv): drafts and the local AI helpers, Siegfried format identification, --ro-crate, --tools-history.
 */
#define _XOPEN_SOURCE 700
#include "arv.h"
#include "data.h"
#include "../rs03/rs03.h"
#include "../udfwrite/udfwrite.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FORMAT_NAME "smart-archive"
#define FORMAT_VERSION "0.5"
#define URL "https://github.com/mofosyne/arv"
#define SECTOR 2048
#define GF_FIELDMAX 255

static void say(const char *fmt, const char *arg)
{
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------------ options */

typedef struct {
    const char *source, *home, *output, *output_dir, *id, *set, *coverage, *title, *label, *description;
    const char *creator, *location, *access, *rights, *links, *medium, *media, *snapshot, *tools;
    const char *basis, *review, *formats, *sf_home, *extra_tools, *draft, *message;
    strlist categories, subjects, notes, importance;
    long medium_sectors;
    double min_redundancy;
    int no_rules, no_ecc, no_verify, no_defect_management, keep_stage, ignore_names, label_given, redundancy_given, split, tools_history, ro_crate, yes;
    int final, access_given;
} options;

static const char HELP[] =
    "usage: arv make [options] FOLDER\n"
    "Makes archive disc images of FOLDER and records them in the home catalogue.\n"
    "  -y, --yes              ask nothing (in a terminal, make asks what the options leave open)\n"
    "  -C, --home HOME        the home (default: $ARV_HOME, a .arv above FOLDER or here, ...)\n"
    "  -o, --output PATH      the image (default: <disc-id>.iso in --output-dir)\n"
    "  --output-dir DIR       where the image goes (default: the current folder)\n"
    "  --set CODE             set code or alias (default: from the folder name and Match rules)\n"
    "  --category CODE        extra category (repeatable; default: from Match rules)\n"
    "  --no-rules             no categories from Match rules\n"
    "  --id ID                the disc id (default: SET-SEQ_COVERAGE_CHECK)\n"
    "  --coverage EDTF        dates covered (default: the files' years)\n"
    "  --title, --description, --creator, --rights TEXT\n"
    "  --subject, --note TEXT (repeatable)\n"
    "  --label TEXT           volume label text after the id (default: the title)\n"
    "  --location PLACE       where the disc will be kept: a location code or text\n"
    "  --access LEVEL         private (default), public or sealed\n"
    "  --links POLICY         default, record or copy (docs/spec/smart-archive-format.md, Links)\n"
    "  --importance 'LEVEL for AUDIENCE' (repeatable), --basis TEXT, --review DATE|5y\n"
    "                         appraise the disc (the archivist log)\n"
    "  --medium bd25|bd50|bd100|bd128|auto, --medium-sectors N, --no-defect-management\n"
    "  --min-redundancy PERCENT  RS03 minimum (default: 20)\n"
    "  --media TEXT           media description (default: M-DISC <medium>)\n"
    "  --snapshot full|set|disc  the catalogue the disc carries (default: full)\n"
    "  --tools DIR            arv's source for tools/ (default: found next to this program)\n"
    "  --tools-history        also arv's whole git history in tools/ (a git bundle)\n"
    "  --ro-crate             RO-Crate 1.2 metadata in data/ (ro-crate-metadata.json and a preview)\n"
    "  --draft FILE           title, description, subjects, notes and folder tags from a draft (the JSON\n"
    "                         arv describe --save and arv tag --save write); options given win\n"
    "  --extra-tools DIR      a folder copied to tools/extra/ (dvdisaster binaries, say)\n"
    "  --split                spread the folder over as many discs as needed\n"
    "In a collection's workflow folder (arv collection init), the collection gives the title, set,\n"
    "categories and access, its code starts the disc ids, and each make is its next edition:\n"
    "  --final                a final edition (default: provisional)\n"
    "  --message TEXT         what this edition is, for the collection's history\n"
    "  --formats auto|yes|no  PRONOM format ids with Siegfried (auto: when sf is on PATH)\n"
    "  --sf-home DIR          Siegfried signature folder (sf -home)\n"
    "  --no-ecc, --no-verify  skip RS03, or skip testing the image afterwards\n"
    "  --ignore-names, --keep-stage\n"
    "Not here: the local AI helpers (arv describe, arv tag) make drafts; this takes them (--draft).\n";

static int parse_options(int argc, char **argv, options *o)
{
    memset(o, 0, sizeof *o);
    o->access = "private";
    o->links = "default";
    o->medium = "bd25";
    o->snapshot = "full";
    o->formats = "auto";
    o->min_redundancy = 20;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        const char **str = NULL;
        strlist *list = NULL;
        if (a[0] != '-') {
            if (o->source) return 2;
            o->source = a;
            continue;
        }
        if (!strcmp(a, "--no-rules")) { o->no_rules = 1; continue; }
        if (!strcmp(a, "--no-ecc")) { o->no_ecc = 1; continue; }
        if (!strcmp(a, "--no-verify")) { o->no_verify = 1; continue; }
        if (!strcmp(a, "--no-defect-management")) { o->no_defect_management = 1; continue; }
        if (!strcmp(a, "--keep-stage")) { o->keep_stage = 1; continue; }
        if (!strcmp(a, "--ignore-names")) { o->ignore_names = 1; continue; }
        if (!strcmp(a, "--split")) { o->split = 1; continue; }
        if (!strcmp(a, "--tools-history")) { o->tools_history = 1; continue; }
        if (!strcmp(a, "--ro-crate")) { o->ro_crate = 1; continue; }
        if (!strcmp(a, "--final")) { o->final = 1; continue; }
        if (!strcmp(a, "-y") || !strcmp(a, "--yes")) { o->yes = 1; continue; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(HELP, stdout);
            exit(0);
        }
        if (!strncmp(a, "--llm", 5) || !strncmp(a, "--vision", 8))
            die("make %s: the local AI helpers are separate steps now: arv describe FOLDER --save d.json (or arv tag), "
                "then arv make --draft d.json FOLDER", a);
        if (i + 1 >= argc) return 2;
        const char *v = argv[++i];
        if (!strcmp(a, "-C") || !strcmp(a, "--home")) str = &o->home;
        else if (!strcmp(a, "-o") || !strcmp(a, "--output")) str = &o->output;
        else if (!strcmp(a, "--output-dir")) str = &o->output_dir;
        else if (!strcmp(a, "--id")) str = &o->id;
        else if (!strcmp(a, "--set")) str = &o->set;
        else if (!strcmp(a, "--coverage")) str = &o->coverage;
        else if (!strcmp(a, "--title")) str = &o->title;
        else if (!strcmp(a, "--label")) { str = &o->label; o->label_given = 1; }
        else if (!strcmp(a, "--description")) str = &o->description;
        else if (!strcmp(a, "--creator")) str = &o->creator;
        else if (!strcmp(a, "--location")) str = &o->location;
        else if (!strcmp(a, "--access")) { str = &o->access; o->access_given = 1; }
        else if (!strcmp(a, "--message")) str = &o->message;
        else if (!strcmp(a, "--rights")) str = &o->rights;
        else if (!strcmp(a, "--links")) str = &o->links;
        else if (!strcmp(a, "--medium")) str = &o->medium;
        else if (!strcmp(a, "--media")) str = &o->media;
        else if (!strcmp(a, "--snapshot")) str = &o->snapshot;
        else if (!strcmp(a, "--tools")) str = &o->tools;
        else if (!strcmp(a, "--basis")) str = &o->basis;
        else if (!strcmp(a, "--review")) str = &o->review;
        else if (!strcmp(a, "--formats")) str = &o->formats;
        else if (!strcmp(a, "--sf-home")) str = &o->sf_home;
        else if (!strcmp(a, "--extra-tools")) str = &o->extra_tools;
        else if (!strcmp(a, "--draft")) str = &o->draft;
        else if (!strcmp(a, "--category")) list = &o->categories;
        else if (!strcmp(a, "--subject")) list = &o->subjects;
        else if (!strcmp(a, "--note")) list = &o->notes;
        else if (!strcmp(a, "--importance")) list = &o->importance;
        else if (!strcmp(a, "--medium-sectors")) { o->medium_sectors = atol(v); continue; }
        else if (!strcmp(a, "--min-redundancy")) { o->min_redundancy = atof(v); o->redundancy_given = 1; continue; }
        else {
            fprintf(stderr, "arv make: unknown option %s\n", a);
            return 2;
        }
        if (str) *str = v;
        else strlist_add(list, v);
    }
    if (!o->source) return 2;
    const char *ok[][5] = { { "private", "public", "sealed", NULL }, { "default", "record", "copy", NULL },
                            { "full", "set", "disc", NULL }, { "bd25", "bd50", "bd100", "bd128", "auto" },
                            { "auto", "yes", "no", NULL } };
    const char *val[] = { o->access, o->links, o->snapshot, o->medium, o->formats };
    const char *what[] = { "--access", "--links", "--snapshot", "--medium", "--formats" };
    for (int k = 0; k < 5; k++) {
        int good = 0;
        for (int j = 0; j < 5 && ok[k][j]; j++) good |= !strcmp(val[k], ok[k][j]);
        if (!good) {
            fprintf(stderr, "arv make: %s %s is not one of the choices\n", what[k], val[k]);
            return 2;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ drafts (describe.load_draft) */

static int by_cstr(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* the folder tags and captions for the folders on one disc ("." and every folder above a file), as
 * tags.tsv (catalog.write_tags); 0 when there are none */
static int write_plan_tags(const char *path, const draft *d, const entries *files)
{
    strlist folders = { 0 };
    strlist_add(&folders, ".");
    for (size_t i = 0; i < files->n; i++)
        for (const char *s = strchr(files->v[i].path, '/'); s; s = strchr(s + 1, '/')) {
            char *f = xprintf("%.*s", (int)(s - files->v[i].path), files->v[i].path);
            if (!strlist_has(&folders, f)) strlist_add(&folders, f);
            free(f);
        }
    strlist keys = { 0 };
    for (size_t i = 0; i < d->nft; i++)
        if (strlist_has(&folders, d->ft_folder[i]) && !strlist_has(&keys, d->ft_folder[i])) strlist_add(&keys, d->ft_folder[i]);
    for (size_t i = 0; i < d->ncap; i++)
        if (strlist_has(&folders, d->cap_folder[i]) && !strlist_has(&keys, d->cap_folder[i])) strlist_add(&keys, d->cap_folder[i]);
    strlist_free(&folders);
    if (!keys.n) return 0;
    qsort(keys.v, keys.n, sizeof *keys.v, by_cstr);
    sbuf b = { 0 };
    sb_puts(&b, "# folder (relative to data/)\ttags\tcaption (what sampled images show, if analysed)\n");
    for (size_t k = 0; k < keys.n; k++) {
        sb_puts(&b, keys.v[k]);
        sb_puts(&b, "\t");
        for (size_t i = d->nft; i-- > 0;)                /* the last entry for a folder, as a dict keeps */
            if (!strcmp(d->ft_folder[i], keys.v[k])) {
                for (size_t t = 0; t < d->ft_tags[i].n; t++) sb_printf(&b, "%s%s", t ? ", " : "", d->ft_tags[i].v[t]);
                break;
            }
        for (size_t i = d->ncap; i-- > 0;)
            if (!strcmp(d->cap_folder[i], keys.v[k])) {
                if (*d->cap_text[i]) {
                    sb_puts(&b, "\t");
                    for (const char *c = d->cap_text[i]; *c; c++) sb_add(&b, *c == '\t' || *c == '\n' ? " " : c, 1);
                }
                break;
            }
        sb_puts(&b, "\n");
    }
    write_text(path, b.s);
    free(b.s);
    strlist_free(&keys);
    return 1;
}

/* ------------------------------------------------------------------ questions (cli.ask) */

static int interactive;         /* a terminal, and no -y */

/* asks on the terminal; the answer, else the default (NULL when there is none) */
static char *ask(const char *prompt, const char *dflt)
{
    if (!interactive) return dflt ? xstrdup(dflt) : NULL;
    printf("%s%s%s%s: ", prompt, dflt && *dflt ? " [" : "", dflt && *dflt ? dflt : "", dflt && *dflt ? "]" : "");
    fflush(stdout);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, stdin);
    if (n < 0) {
        free(line);
        line = NULL;
    }
    char *a = line ? line : NULL, *e;
    if (a) {
        while (*a == ' ' || *a == '\t') a++;
        e = a + strlen(a);
        while (e > a && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    }
    char *out = a && *a ? xstrdup(a) : dflt && *dflt ? xstrdup(dflt) : NULL;
    free(line);
    return out;
}

/* the non-blank parts of "a, b ,c" between commas; strip: without their spaces (Python keeps them for
 * categories, which pick_code trims) */
static void split_commas(const char *text, strlist *out, int strip)
{
    if (!text) return;
    char *copy = xstrdup(text), *p = copy;
    for (;;) {
        char *comma = strchr(p, ',');
        if (comma) *comma = 0;
        char *a = p, *e = p + strlen(p);
        while (*a == ' ' || *a == '\t') a++;
        while (e > a && (e[-1] == ' ' || e[-1] == '\t')) e--;
        if (e > a) {
            if (strip) {
                char *one = xprintf("%.*s", (int)(e - a), a);
                strlist_add(out, one);
                free(one);
            } else {
                strlist_add(out, p);
            }
        }
        if (!comma) break;
        p = comma + 1;
    }
    free(copy);
}

/* ------------------------------------------------------------------ media (src/arv/media.py) */

static const struct {
    const char *name, *label;
    long dm, nodm;
} MEDIA[] = { { "bd25", "BD-R 25GB", 11826176, 12219392 }, { "bd50", "BD-R DL 50GB", 23652352, 24438784 },
              { "bd100", "BD-R XL 100GB", 47305728, 48878592 }, { "bd128", "BD-R XL 128GB", 60403712, 62500864 } };

static long data_budget(long medium_sectors, double min_redundancy)
{
    long per_layer = medium_sectors / GF_FIELDMAX;
    long max_ndata = (long)(GF_FIELDMAX / (1 + min_redundancy / 100.0)) - 1;   /* floor: positive */
    if (max_ndata > GF_FIELDMAX - 1 - 8) max_ndata = GF_FIELDMAX - 1 - 8;
    if (max_ndata < 1) die("redundancy %s%% is not possible", "requested");
    return max_ndata * per_layer - 2;
}

/* ------------------------------------------------------------------ text */

/* textwrap.fill(text, width, indent): words, greedily, as Python does for these texts */
static void fill(sbuf *out, const char *text, size_t width, const char *indent)
{
    size_t col = 0;
    char *copy = xstrdup(text);
    int first = 1;
    for (char *w = strtok(copy, " \t\n"); w; w = strtok(NULL, " \t\n")) {
        size_t n = utf8_chars(w);
        if (!first && col + 1 + n > width) {
            sb_puts(out, "\n");
            first = 1;
        }
        if (first) {
            sb_puts(out, indent);
            col = strlen(indent);
            first = 0;
        } else {
            sb_puts(out, " ");
            col++;
        }
        sb_puts(out, w);
        col += n;
    }
    free(copy);
}

/* str.format with named fields: {name} -> value, {{ and }} -> { and } */
static char *format(const char *tmpl, const char *const *names, const char *const *values)
{
    sbuf out = { 0 };
    for (const char *s = tmpl; *s; s++) {
        if ((s[0] == '{' && s[1] == '{') || (s[0] == '}' && s[1] == '}')) {
            sb_add(&out, s, 1);
            s++;
            continue;
        }
        if (*s == '{') {
            const char *end = strchr(s, '}');
            int found = 0;
            for (int i = 0; end && names[i]; i++)
                if (strlen(names[i]) == (size_t)(end - s - 1) && !strncmp(s + 1, names[i], (size_t)(end - s - 1))) {
                    sb_puts(&out, values[i]);
                    found = 1;
                    break;
                }
            if (found) {
                s = end;
                continue;
            }
        }
        sb_add(&out, s, 1);
    }
    return out.s;
}

/* ------------------------------------------------------------------ the tool on the disc */

static int has(const char *dir, const char *rel)
{
    char *p = join(dir, rel);
    int r = !access(p, F_OK);
    free(p);
    return r;
}

/* the arv source tree to put in tools/: --tools, $ARV_SOURCE, an installed share/arv, arv/ next to
 * this program (tools/arv.com on a disc), or the checkout it was built in (a few folders up) */
static char *find_source(const char *given)
{
    if (given) return xstrdup(given);
    if (getenv("ARV_SOURCE") && *getenv("ARV_SOURCE")) return xstrdup(getenv("ARV_SOURCE"));
    char *exe = exe_dir(), *found = NULL;
    if (!exe) return NULL;
    char *installed = xprintf("%s/../share/arv", exe), *beside = join(exe, "arv");
    if (has(installed, "src/arv/arv.c")) found = realpath(installed, NULL);
    else if (has(beside, "src/arv/arv.c")) found = realpath(beside, NULL);
    for (int up = 1; !found && up <= 4; up++) {
        sbuf dir = { 0 };
        sb_puts(&dir, exe);
        for (int k = 0; k < up; k++) sb_puts(&dir, "/..");
        if (has(dir.s, "src/arv/arv.c")) found = realpath(dir.s, NULL);
        free(dir.s);
    }
    free(installed);
    free(beside);
    free(exe);
    return found;
}

/* "arv@<commit>" (+uncommitted), from git in a checkout or VERSION in an installed tree */
static char *software_version(const char *source, int *is_git)
{
    *is_git = 0;
    if (source && has(source, ".git") && on_path("git")) {
        char *out = NULL, *dirty = NULL;
        char *a[] = { "git", "-C", (char *)source, "rev-parse", "--short=12", "HEAD", NULL };
        char *b[] = { "git", "-C", (char *)source, "status", "--porcelain", "--untracked-files=no", NULL };
        if (run(a, &out) == 0 && run(b, &dirty) == 0) {
            out[strcspn(out, "\n")] = 0;
            char *v = xprintf("arv@%s%s", out, *dirty ? "+uncommitted" : "");
            free(out);
            free(dirty);
            *is_git = 1;
            return v;
        }
        free(out);
        free(dirty);
    }
    if (source && has(source, "VERSION")) {
        char *p = join(source, "VERSION"), *t = read_text(p);
        free(p);
        if (t) {
            t[strcspn(t, "\n")] = 0;
            char *v = xprintf("arv@%s", strncmp(t, "arv@", 4) ? t : t + 4);
            free(t);
            return v;
        }
    }
    return xstrdup("arv@unknown");
}

static const char APE_USE[] =
    "It is also ready to run, as\n"
    "  tools/arv.com: one file for Linux, macOS, Windows and the BSDs, on x86-64\n"
    "  and ARM64 (Cosmopolitan). Copy it off the disc (on Windows as arv.exe):\n"
    "    ~/arv.com verify .                 (or: ~/arv.com restore . ~/restored)\n"
    "  If a Linux shell will not start it: sh ~/arv.com verify .\n"
    "  ";
static const char APE_LINE[] = "  tools/arv.com           the reader, ready to run (Linux, macOS, Windows, BSD)\n";

/* arv as an Actually Portable Executable for tools/arv.com (cli.find_ape): $ARV_APE, arv.com in the
 * source tree (an installed one), the C port's build in a checkout, or this program when it is
 * one (arv.com run from a disc); NULL when there is none, or ARV_APE=none */
static int reg_file(const char *path)
{
    struct stat st;
    return !stat(path, &st) && S_ISREG(st.st_mode);
}

static char *find_ape(const char *source)
{
    const char *env = getenv("ARV_APE");
    if (env && !strcmp(env, "none")) return NULL;      /* a disc without it */
    if (env && *env && reg_file(env)) return xstrdup(env);
    if (source) {
        char *a = join(source, "arv.com"), *b = join(source, "src/arv/build/arv.com");
        char *found = reg_file(a) ? a : reg_file(b) ? b : NULL;
        if (found) {
            char *out = xstrdup(found);
            free(a);
            free(b);
            return out;
        }
        free(a);
        free(b);
    }
    char *dir = exe_dir();
    if (dir && arv_argv0) {
        const char *name = strrchr(arv_argv0, '/') ? strrchr(arv_argv0, '/') + 1 : arv_argv0;
        size_t n = strlen(name);
        char *self = join(dir, name);
        if (n > 4 && !strcmp(name + n - 4, ".com") && reg_file(self)) {
            free(dir);
            return self;
        }
        free(self);
    }
    free(dir);
    return NULL;
}

/* arv's last commit (and with history, a git bundle of every branch), arv.com and any
 * extra tools (cli.stage_tools) */
static void stage_tools(const char *tools, const char *source, int is_git, const char *workdir, const options *o)
{
    char *tree = join(tools, "arv");
    if (mkdirs(tree)) die("cannot create %s", tree);
    if (!source) {
        say("Warning: arv's source was not found (--tools DIR or $ARV_SOURCE): tools/ holds only what the "
            "disc needs to explain itself", "");
    } else if (is_git) {                 /* the last commit, as git archive gives it */
        char *tar = join(workdir, "tools.tar"), *out = NULL;
        char *a[] = { "git", "-C", (char *)source, "archive", "--format=tar", "-o", tar, "HEAD", NULL };
        char *b[] = { "tar", "-x", "-f", tar, "-C", tree, NULL };
        if (run(a, &out) || run(b, &out)) die("could not copy arv's source into tools/: %s", out ? out : "");
        unlink(tar);
        free(tar);
        free(out);
    } else {
        static const char *const skip[] = { "__pycache__", "*.pyc", ".git", "*.iso", NULL };
        copy_tree(source, tree, skip);
    }
    if (source && is_git && o->tools_history) {
        char *bundle = join(tools, "arv.bundle"), *out = NULL;
        char *a[] = { "git", "-C", (char *)source, "bundle", "create", bundle, "--all", NULL };
        if (run(a, &out)) {
            char *e = out + strlen(out);
            while (e > out && (e[-1] == '\n' || e[-1] == ' ')) *--e = 0;
            fprintf(stderr, "Warning: git bundle failed, disc gets the plain tree only:\n%s\n", out);
            unlink(bundle);
        }
        free(out);
        free(bundle);
    }
    char *ape = find_ape(source);
    if (ape) {
        char *to = join(tools, "arv.com");
        copy_file(ape, to);
        chmod(to, 0755);
        free(to);
        free(ape);
    }
    if (o->extra_tools) {
        char *extra = join(tools, "extra");
        if (mkdirs(extra)) die("cannot create %s", extra);
        copy_tree(o->extra_tools, extra, NULL);
        free(extra);
    }
    free(tree);
}

/* ------------------------------------------------------------------ the catalogue */

static char *review_date(const char *text)
{
    char unit;
    int n;
    if (sscanf(text, " %d %c", &n, &unit) == 2 && (unit == 'y' || unit == 'm' || unit == 'Y' || unit == 'M') && n >= 0) {
        struct tm tm;
        today_tm(&tm);
        int months = n * (tolower((unsigned char)unit) == 'y' ? 12 : 1) + tm.tm_mon;
        return xprintf("%04d-%02d-%02d", tm.tm_year + 1900 + months / 12, months % 12 + 1, tm.tm_mday < 28 ? tm.tm_mday : 28);
    }
    long a, b;
    if (strlen(text) == 10 && edtf_span(text, &a, &b) == 1 && a == b) return xstrdup(text);
    die("review date: YYYY-MM-DD or a span such as 5y or 18m, not %s", text);
    return NULL;
}

/* ------------------------------------------------------------------ the image (udfwrite) */

typedef struct {
    char **paths;               /* disk path of each file, by index */
    size_t n;
    FILE *open_fp;
    size_t open_index;
    FILE *out, *extents;
    uint64_t written;
} image_ctx;

static image_ctx *ictx;

static long read_cb(void *file, uint64_t offset, void *buf, size_t len)
{
    size_t i = (size_t)(uintptr_t)file;
    if (!ictx->open_fp || ictx->open_index != i) {
        if (ictx->open_fp) fclose(ictx->open_fp);
        ictx->open_index = i;
        if (!(ictx->open_fp = fopen(ictx->paths[i], "rb"))) return -1;
    }
    if (fseeko(ictx->open_fp, (off_t)offset, SEEK_SET)) return -1;
    return (long)fread(buf, 1, len, ictx->open_fp);
}

static int write_cb(void *ctx, const void *buf, size_t count)
{
    (void)ctx;
    ictx->written += count;
    return fwrite(buf, UDFW_SECTOR, count, ictx->out) == count ? 0 : -1;
}

static void extent_cb(void *ctx, const char *path, uint64_t sector, uint64_t size)
{
    (void)ctx;
    fprintf(ictx->extents, "%llu\t%llu\t%s\n", (unsigned long long)sector, (unsigned long long)size, path);
}

static size_t add_source(image_ctx *c, const char *path)
{
    c->paths = xrealloc(c->paths, (c->n + 1) * sizeof *c->paths);
    c->paths[c->n] = xstrdup(path);
    return c->n++;
}

/* adds everything under disk folder dir as image path rel */
static void add_tree(udfw *w, image_ctx *c, const char *dir, const char *rel)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) die("cannot read %s", dir);
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *disk = join(dir, e->d_name), *path = *rel ? join(rel, e->d_name) : xstrdup(e->d_name);
        struct stat st;
        if (stat(disk, &st)) die("cannot read %s", disk);
        if (S_ISDIR(st.st_mode)) {
            if (udfw_add_dir(w, path, (int64_t)st.st_mtime)) die("udfwrite: %s", udfw_error(w));
            add_tree(w, c, disk, path);
        } else if (udfw_add_file(w, path, (uint64_t)st.st_size, (int64_t)st.st_mtime, (unsigned)st.st_mode, read_cb,
                                 (void *)(uintptr_t)add_source(c, disk))) {
            die("udfwrite: %s", udfw_error(w));
        }
        free(disk);
        free(path);
    }
    closedir(d);
}

/* the image: the stage at the top, the payload under data/. Returns its sectors. */
static uint64_t build_image(const char *stage, const char *src, const entries *files, const entries *extras, int whole_folder,
                            const char *out, const char *extents, const char *label, const char *disc_id,
                            const char *volume_set, int64_t when)
{
    image_ctx c;
    memset(&c, 0, sizeof c);
    ictx = &c;
    udfw_options opt = { disc_id, label, volume_set, when };
    udfw *w = udfw_new(&opt);
    if (!w) die("%s", "out of memory");
    add_tree(w, &c, stage, "");
    struct stat st;
    if (stat(src, &st)) die("cannot read %s", src);
    if (udfw_add_dir(w, "data", (int64_t)st.st_mtime)) die("udfwrite: %s", udfw_error(w));
    if (whole_folder) add_tree(w, &c, src, "data");     /* empty folders included */
    for (int pass = whole_folder; pass < 2; pass++) {   /* a folder with links: exactly the listed files */
        const entries *list = pass ? extras : files;    /* then the files added to data/ */
        for (size_t i = 0; i < list->n; i++) {
            char *path = join("data", list->v[i].path);
            if (stat(list->v[i].source, &st)) die("cannot read %s", list->v[i].source);
            if (udfw_add_file(w, path, list->v[i].size, (int64_t)st.st_mtime, (unsigned)st.st_mode, read_cb,
                              (void *)(uintptr_t)add_source(&c, list->v[i].source)))
                die("udfwrite: %s", udfw_error(w));
            free(path);
        }
    }
    if (!(c.out = fopen(out, "wb"))) die("cannot write %s", out);
    if (!(c.extents = fopen(extents, "w"))) die("cannot write %s", extents);
    fputs("# arv extents 1\tstart sector\tsize (bytes)\tpath\n", c.extents);
    int rc = udfw_write(w, write_cb, NULL, extent_cb, NULL);
    if (c.open_fp) fclose(c.open_fp);
    if (rc) die("udfwrite: %s", udfw_error(w));
    if (fclose(c.out) || fclose(c.extents)) die("cannot write %s", out);
    udfw_free(w);
    for (size_t i = 0; i < c.n; i++) free(c.paths[i]);
    free(c.paths);
    return c.written;
}

/* ------------------------------------------------------------------ make */

static char *folder_default_title(const char *src, char **set_out)
{
    char *copy = xstrdup(src);
    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/') copy[--len] = 0;
    char *name = strrchr(copy, '/') ? strrchr(copy, '/') + 1 : copy;
    /* a leading YYYY-MM-DD_ is dropped */
    if (strlen(name) > 11 && isdigit((unsigned char)name[0]) && name[4] == '-' && name[7] == '-' && name[10] == '_') {
        int digits = 1;
        for (int i = 0; i < 10; i++) if (i != 4 && i != 7 && !isdigit((unsigned char)name[i])) digits = 0;
        if (digits) name += 11;
    }
    sbuf title = { 0 };
    char *words = xstrdup(name);
    for (char *p = words; *p; p++) if (*p == '_') *p = ' ';
    for (char *w = strtok(words, " \t"); w; w = strtok(NULL, " \t")) {
        if (title.len) sb_puts(&title, " ");
        char first = (char)toupper((unsigned char)w[0]);
        sb_add(&title, &first, 1);
        sb_puts(&title, w + 1);
    }
    free(words);
    char *first_part = xstrdup(name), *u = strchr(first_part, '_');
    if (u) *u = 0;
    sbuf set = { 0 };
    for (char *p = first_part; *p; p++)
        if (isalnum((unsigned char)*p) && !((unsigned char)*p & 0x80)) {
            char ch = (char)toupper((unsigned char)*p);
            sb_add(&set, &ch, 1);
        }
    *set_out = set.len ? set.s : (free(set.s), xstrdup("ARCHIVE"));
    free(first_part);
    char *t = title.len ? title.s : (free(title.s), xstrdup(name));
    free(copy);
    return t;
}

/* a vocabulary code for a typed code or alias; otherwise the text as a code */
static char *pick_code(const vocab *v, const char *text)
{
    const char *code = vocab_resolve(v, text);
    if (code) {
        char *w = xmalloc(strlen(text) + 1);
        vocab_word(text, w);
        if (strcmp(w, code)) fprintf(stderr, "%s -> %s\n", text, code);
        free(w);
        return xstrdup(code);
    }
    char s[9];
    size_t k = 0, all = 0;
    for (const char *p = text; *p; p++)
        if (isalnum((unsigned char)*p) && !((unsigned char)*p & 0x80)) {
            all++;
            if (k < 8) s[k++] = (char)toupper((unsigned char)*p);
        }
    s[k] = 0;
    return xstrdup(all >= 2 ? s : "ARCHIVE");
}



/* ------------------------------------------------------------------ discs (make.Maker) */

typedef struct {
    entries files, noted;           /* this disc's share of the folder */
    entries extras, payload;        /* files added to data/ (--ro-crate); files + extras */
    char disc_id[64], uuid[37];
    long sequence;
    int part, parts;
    char *out, *label, *stage, *built, *extents, *b_manifest, *b_listing, *b_formats, *b_tags;
    rec_record *disc, *binding;
    recs events, appraisals;
    uint64_t sectors;
} plan;

typedef struct {
    options *o;
    arv_home *h;
    archive *cat;
    const char *src, *title, *creator, *set_code, *medium_label;
    char *location, *source, *software, *review, *workdir;
    entries *files, *noted;
    strlist *categories, *paths;
    char coverage[64], today[11];
    long capacity, budget;
    int is_git, links_chosen, have_formats;
    formats fmt;
    const draft *draft;         /* --draft, or NULL */
    rec_record *collection;     /* the workflow folder's collection, or NULL */
    const char *id_prefix;      /* the collection's code, else the set code */
    char *tree_text;            /* the collection's revision manifest */
    rec_record *revision;       /* this edition, once the discs are assigned */
    size_t left_out;            /* files arv keeps off the disc (its own .arv) */
    plan *plans;
    size_t nplans;
} maker;

static char *redundancy_text(const options *o)
{
    char red[40];          /* as Python prints it: the default is the int 20, a given value a float */
    snprintf(red, sizeof red, "%.15g", o->min_redundancy);
    if (o->redundancy_given && !strchr(red, '.') && !strchr(red, 'e')) strcat(red, ".0");
    return xstrdup(red);
}

static void sub_entries(entries *out, const entry *v, size_t n)
{
    out->v = xmalloc((n + 1) * sizeof *out->v);
    memcpy(out->v, v, n * sizeof *v);
    out->n = n;
}

static int by_entry_path(const void *a, const void *b)
{
    return strcmp(((const entry *)a)->path, ((const entry *)b)->path);
}

/* rough image cost of one file: its data, a file entry and directory records (make.estimate_sectors) */
static long estimate_sectors(const entry *e)
{
    return (long)((e->size + SECTOR - 1) / SECTOR) + 1 + (long)(3 * (utf8_chars(e->path) + 64)) / SECTOR + 1;
}

/* files in path order, a disc filled before the next; bins[i] holds counts */
static size_t greedy_split(const entries *all, long limit, size_t **counts)
{
    size_t nbins = 0, current = 0;
    long used = 0;
    *counts = xmalloc((all->n + 1) * sizeof **counts);
    for (size_t i = 0; i < all->n; i++) {
        long cost = estimate_sectors(&all->v[i]);
        if (cost > limit) {
            fprintf(stderr, "Error: %s (%llu bytes) is larger than one disc can hold\n", all->v[i].path,
                    (unsigned long long)all->v[i].size);
            exit(1);
        }
        if (current && used + cost > limit) {
            (*counts)[nbins++] = current;
            current = 0;
            used = 0;
        }
        current++;
        used += cost;
    }
    if (current || !nbins) (*counts)[nbins++] = current;
    return nbins;
}

static long dir_sectors(const char *path)
{
    long total = 0;
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *p = join(path, e->d_name);
        struct stat st;
        if (!lstat(p, &st)) total += S_ISDIR(st.st_mode) ? dir_sectors(p) : (long)((st.st_size + SECTOR - 1) / SECTOR) + 1;
        free(p);
    }
    if (d) closedir(d);
    return total;
}

static int prior_disc(const maker *mk, const rec_record *d)
{
    if (!strcmp(mk->o->snapshot, "full")) return 1;
    return !strcmp(mk->o->snapshot, "set") && rec_get(d, "Set") && !strcmp(rec_get(d, "Set"), mk->set_code) &&
           !strcmp(disc_access(d), "public");
}

static long snapshot_estimate(const maker *mk)
{
    static const char *const KINDS[] = { "manifest.sha256", "listing.tsv", "formats.csv", "tags.tsv", "extents.tsv", NULL };
    long total = 0;
    for (size_t i = 0; i < mk->cat->discs.n; i++) {
        const rec_record *d = mk->cat->discs.v[i];
        if (!prior_disc(mk, d) || !rec_get(d, "Id")) continue;
        for (int k = 0; KINDS[k]; k++) {
            char *p = home_volume_file(mk->h, rec_get(d, "Id"), KINDS[k]);
            struct stat st;
            if (!stat(p, &st)) total += 2 * (long)st.st_size;
            free(p);
        }
    }
    for (size_t i = 0; i < mk->files->n; i++) total += 4 * (long)(utf8_chars(mk->files->v[i].path) + 200);
    return total / SECTOR + 256;
}

/* the first bins: one, or (--split) as many as the estimates need */
static size_t initial_bins(maker *mk, size_t **counts)
{
    if (!mk->o->split || !mk->budget) {
        if (mk->o->split) die("%s", "--split needs a target --medium (not auto)");
        *counts = xmalloc(sizeof **counts);
        (*counts)[0] = mk->files->n;
        return 1;
    }
    char *probe = join(mk->workdir, "tools-probe");
    stage_tools(probe, mk->source, mk->is_git, mk->workdir, mk->o);
    long reserve = dir_sectors(probe) + snapshot_estimate(mk) + mk->budget / 200 + (1024 < mk->budget / 20 ? 1024 : mk->budget / 20);
    remove_tree(probe);
    free(probe);
    return greedy_split(mk->files, mk->budget - reserve, counts);
}

/* the noted links that go into disc i's listing: with the disc holding the file just before each */
static void links_for(const maker *mk, const size_t *counts, size_t nbins, size_t i, entries *out)
{
    out->v = xmalloc((mk->noted->n + 1) * sizeof *out->v);
    out->n = 0;
    for (size_t k = 0; k < mk->noted->n; k++) {
        size_t at = 0, first = 0;
        if (nbins > 1)
            for (size_t j = 0; j < nbins; j++) {
                if (counts[j] && strcmp(mk->files->v[first].path, mk->noted->v[k].path) <= 0) at = j;
                first += counts[j];
            }
        if (at == i) out->v[out->n++] = mk->noted->v[k];
    }
}

static rec_record *disc_record(const maker *mk, const plan *p)
{
    const options *o = mk->o;
    rec_record *d = rec_alloc("Disc");
    uint64_t bytes = 0;
    for (size_t i = 0; i < p->files.n; i++) bytes += p->files.v[i].size;
    rec_add(d, "Id", p->disc_id);
    rec_add(d, "Uuid", p->uuid);
    if (!o->id) rec_add(d, "IdScheme", DISCID_SCHEME);
    if (strcmp(p->label, p->disc_id)) rec_add(d, "Label", p->label);
    rec_add(d, "Title", mk->title);
    if (mk->collection) rec_add(d, "Collection", rec_get(mk->collection, "Code"));
    rec_add(d, "Set", mk->set_code);
    for (size_t i = 0; i < mk->categories->n; i++) rec_add(d, "Category", mk->categories->v[i]);
    for (size_t i = 0; i < mk->paths->n; i++) rec_add(d, "Path", mk->paths->v[i]);
    char *seq = xprintf("%ld", p->sequence);
    rec_add(d, "Sequence", seq);
    free(seq);
    rec_add(d, "Coverage", mk->coverage);
    rec_add(d, "Date", mk->today);
    if (p->parts > 1) {
        char *part = xprintf("%d of %d", p->part, p->parts);
        rec_add(d, "Part", part);
        free(part);
    }
    if (mk->creator && *mk->creator) rec_add(d, "Creator", mk->creator);
    if (o->description && *o->description) rec_add(d, "Description", o->description);
    for (size_t i = 0; i < o->subjects.n; i++) rec_add(d, "Subject", o->subjects.v[i]);
    for (size_t i = 0; i < o->notes.n; i++) rec_add(d, "Note", o->notes.v[i]);
    if (mk->location) rec_add(d, "Location", mk->location);
    rec_add(d, "Access", o->access);
    if (o->rights) rec_add(d, "Rights", o->rights);
    char *nfiles = xprintf("%zu", p->files.n), *nbytes = xprintf("%llu", (unsigned long long)bytes);
    rec_add(d, "Files", nfiles);
    rec_add(d, "Bytes", nbytes);
    rec_add(d, "Software", mk->software);
    free(nfiles);
    free(nbytes);
    return d;
}

static rec_record *binding_record(const maker *mk, const plan *p)
{
    const options *o = mk->o;
    rec_record *b = rec_alloc("Binding");
    rec_add(b, "Volume", p->disc_id);
    rec_add(b, "Container", "udf-2.50");
    rec_add(b, "Protection", o->no_ecc ? "none" : "rs03");
    char *media = o->media ? xstrdup(o->media) : xprintf("M-DISC %s", mk->capacity ? mk->medium_label : "BD-R");
    rec_add(b, "Media", media);
    rec_add(b, "Filesystem", "UDF 2.50, BD-ROM layout with metadata partition and a real mirror (arv udfwrite)");
    char *ecc, *red = redundancy_text(o);
    if (o->no_ecc) ecc = xstrdup("none");
    else if (mk->capacity)
        ecc = xprintf("dvdisaster RS03 augmented image, %s (%ld sectors), minimum %s%% redundancy", mk->medium_label,
                      mk->capacity, red);
    else ecc = xstrdup("dvdisaster RS03 augmented image");
    rec_add(b, "Ecc", ecc);
    if (mk->capacity && !o->no_ecc) {
        char *ms = xprintf("%ld", mk->capacity);
        rec_add(b, "MediumSectors", ms);
        free(ms);
    }
    free(media);
    free(ecc);
    free(red);
    return b;
}

/* this edition of the collection: a Revision naming its discs */
static rec_record *revision_record(const maker *mk)
{
    const options *o = mk->o;
    const char *uuid = rec_get(mk->collection, "Uuid");
    const rec_record *head = collection_head(mk->cat, uuid);
    const char *parent = head ? rec_get(head, "Node") : NULL;
    char tree[65], node[65];
    revision_hashes(mk->tree_text, parent, mk->today, o->message, tree, node);
    rec_record *r = rec_alloc("Revision");
    rec_add(r, "Node", node);
    rec_add(r, "Collection", uuid);
    rec_add(r, "Tree", tree);
    if (parent) rec_add(r, "Parent", parent);
    rec_add(r, "Date", mk->today);
    rec_add(r, "Stage", o->final ? "final" : "provisional");
    char *n = xprintf("%d", collection_editions(mk->cat, uuid) + 1);
    rec_add(r, "Edition", n);
    free(n);
    for (size_t i = 0; i < mk->nplans; i++) rec_add(r, "Volume", mk->plans[i].disc_id);
    char *before = NULL;
    if (parent) {
        char *path = revision_manifest_path(mk->h, parent);
        before = access(path, F_OK) ? NULL : read_text(path);
        free(path);
    }
    char *changes = manifest_changes(before ? before : "", mk->tree_text);
    rec_add(r, "Changes", changes);
    free(changes);
    free(before);
    if (o->message && *o->message) rec_add(r, "Message", o->message);
    return r;
}

/* plans for these bins: ids, records and their first events (make.Maker.assign) */
static void assign(maker *mk, const size_t *counts, size_t nbins)
{
    const options *o = mk->o;
    if (o->id && nbins > 1) die("--id cannot be used when the folder is split across several discs%s", "");
    long first = archive_next_number(mk->cat, mk->id_prefix);
    mk->plans = xmalloc(nbins * sizeof *mk->plans);
    memset(mk->plans, 0, nbins * sizeof *mk->plans);
    mk->nplans = nbins;
    size_t offset = 0;
    for (size_t i = 0; i < nbins; i++) {
        plan *p = &mk->plans[i];
        sub_entries(&p->files, mk->files->v + offset, counts[i]);
        offset += counts[i];
        links_for(mk, counts, nbins, i, &p->noted);
        p->part = (int)i + 1;
        p->parts = (int)nbins;
        p->sequence = first + (long)i;
        if (o->id) snprintf(p->disc_id, sizeof p->disc_id, "%s", o->id);
        else if (discid_compose(mk->id_prefix, p->sequence, mk->coverage, p->disc_id, sizeof p->disc_id))
            die("cannot make a disc id from %s and this coverage", mk->id_prefix);
        int id_ok = isalnum((unsigned char)p->disc_id[0]) && strlen(p->disc_id) <= 32;
        for (const char *c = p->disc_id; *c; c++) id_ok &= isalnum((unsigned char)*c) || *c == '_' || *c == '-';
        if (!id_ok) die("invalid disc id %s (letters, digits, _ and -; at most 32 characters)", p->disc_id);
        if (archive_disc(mk->cat, p->disc_id)) die("disc id %s already exists in the catalogue", p->disc_id);
        if (o->output && nbins == 1) p->out = realpath(o->output, NULL) ? realpath(o->output, NULL) : xstrdup(o->output);
        else {
            const char *dir = o->output_dir ? o->output_dir : ".";
            if (mkdirs(dir)) die("cannot create %s", dir);
            char *absdir = realpath(dir, NULL);
            p->out = xprintf("%s/%s%s.iso", absdir, p->disc_id, o->no_ecc ? ".noecc" : "");
            free(absdir);
        }
        if (!access(p->out, F_OK)) die("%s already exists", p->out);
        p->label = volume_label(p->disc_id, o->label_given ? o->label : mk->title);
        uuid4(p->uuid);
        p->disc = disc_record(mk, p);
        p->binding = binding_record(mk, p);
        char *note = xprintf("sha256 and sha512 manifests of %zu files", p->files.n);
        recs_add(&p->events, new_event(p->disc_id, "message digest calculation", "success", mk->software, "automatic", note));
        free(note);
        char *summary = link_summary(&p->files, &p->noted, o->links);
        if (summary) {   /* how links were treated: an ingest decision */
            char *who = mk->links_chosen ? person() : xstrdup(mk->software);
            recs_add(&p->events, new_event(p->disc_id, "ingestion", "success", who, mk->links_chosen ? "human" : "automatic", summary));
            free(who);
            free(summary);
        }
        if (mk->draft) {          /* the draft's suggestions, and who saw them (catalog.reviewed_agents) */
            const char *how = mk->draft->authorship;
            char *dnote = xprintf("title, description, subjects and folder tags taken from a draft (made by: %s)",
                                  mk->draft->agent);
            rec_record *e = reviewed_event(p->disc_id, mk->draft->agent, how, dnote);
            free(dnote);
            recs_add(&p->events, e);
        }
        if (mk->have_formats) {
            size_t unknown = formats_unknown(&mk->fmt, &p->files);
            char *agent = formats_agent(&mk->fmt), *fnote = xprintf("PRONOM ids for %zu files, %zu unidentified", p->files.n, unknown);
            recs_add(&p->events, new_event(p->disc_id, "format identification", unknown ? "warning" : "success", agent,
                                           "automatic", fnote));
            free(agent);
            free(fnote);
        }
        if (o->importance.n || o->basis) recs_add(&p->appraisals, new_appraisal(p->disc_id, &o->importance, o->basis, mk->review));
    }
    if (mk->collection) mk->revision = revision_record(mk);
}

static void batch_files(maker *mk)
{
    char *batch = join(mk->workdir, "batch");
    if (mkdirs(batch)) die("cannot create %s", batch);
    for (size_t i = 0; i < mk->nplans; i++) {
        plan *p = &mk->plans[i];
        free(p->extras.v);
        free(p->payload.v);
        memset(&p->extras, 0, sizeof p->extras);
        if (mk->o->ro_crate) {                         /* the crate files, added to data/ */
            char *dir = xprintf("%s/%s-rocrate", batch, p->disc_id);
            if (mkdirs(dir)) die("cannot create %s", dir);
            char *text[2] = { rocrate_metadata(p->disc, &p->files, mk->have_formats ? &mk->fmt : NULL),
                              rocrate_preview(p->disc, &p->files) };
            const char *names[2] = { "ro-crate-metadata.json", "ro-crate-preview.html" };
            p->extras.v = xmalloc(3 * sizeof *p->extras.v);
            for (int k = 0; k < 2; k++) {
                char *path = join(dir, names[k]);
                write_text(path, text[k]);
                struct stat st;
                if (stat(path, &st)) die("cannot read %s", path);
                entry *e = &p->extras.v[p->extras.n++];
                memset(e, 0, sizeof *e);
                e->path = xstrdup(names[k]);
                e->kind = "file";
                e->link = "";
                e->source = path;
                e->size = (uint64_t)st.st_size;
                e->mtime = st.st_mtime;
                hash_both(path, e->sha256, e->sha512);
                free(text[k]);
            }
            free(dir);
        }
        p->payload.v = xmalloc((p->files.n + p->extras.n + 1) * sizeof *p->payload.v);
        memcpy(p->payload.v, p->files.v, p->files.n * sizeof *p->files.v);
        if (p->extras.n) memcpy(p->payload.v + p->files.n, p->extras.v, p->extras.n * sizeof *p->extras.v);
        p->payload.n = p->files.n + p->extras.n;
        p->b_manifest = xprintf("%s/%s.sha256", batch, p->disc_id);
        p->b_listing = xprintf("%s/%s.tsv", batch, p->disc_id);
        write_manifest(p->b_manifest, &p->payload, 0);
        entries sorted;                                /* the listing is by path: extras slot in */
        sub_entries(&sorted, p->payload.v, p->payload.n);
        if (sorted.n) qsort(sorted.v, sorted.n, sizeof *sorted.v, by_entry_path);
        write_listing(p->b_listing, &sorted, &p->noted);
        free(sorted.v);
        p->b_tags = NULL;
        if (mk->draft) {
            char *t = xprintf("%s/%s.tags", batch, p->disc_id);
            if (write_plan_tags(t, mk->draft, &p->files)) p->b_tags = t;
            else free(t);
        }
        p->b_formats = NULL;
        if (mk->have_formats) {
            p->b_formats = xprintf("%s/%s.csv", batch, p->disc_id);
            formats_write(p->b_formats, &mk->fmt, &p->files);
        }
    }
    free(batch);
}

static char *group_id(const maker *mk)
{
    if (mk->nplans < 2) return NULL;
    return xprintf("%s-%02ld-%02ld", mk->id_prefix, mk->plans[0].sequence, mk->plans[mk->nplans - 1].sequence);
}

/* everything on disc i but the payload, in a stage folder (make.Maker.stage) */
static void stage_plan(maker *mk, size_t idx)
{
    const options *o = mk->o;
    plan *p = &mk->plans[idx];
    archive *cat = mk->cat;
    char *tmpl = xprintf("%s/stage-%s-XXXXXX", mk->workdir, p->disc_id);
    if (!mkdtemp(tmpl)) die("cannot create %s", tmpl);
    p->stage = tmpl;
    uint64_t bytes = 0;
    for (size_t i = 0; i < p->payload.n; i++) bytes += p->payload.v[i].size;

    /* BagIt */
    strlist info = { 0 };
    char *ext_desc = o->description && *o->description ? xprintf("%s - %s", mk->title, o->description) : xstrdup(mk->title);
    char *group = group_id(mk), *count = xprintf("%d of %d", p->part, p->parts);
    char *oxum = xprintf("%llu.%zu", (unsigned long long)bytes, p->payload.n), *agent = xprintf("%s <%s>", mk->software, URL);
    const char *pairs[] = { "Bagging-Date", mk->today, "External-Identifier", p->disc_id, "External-Description", ext_desc,
                            "Bag-Group-Identifier", group ? group : mk->id_prefix };
    for (int i = 0; i < 8; i++) strlist_add(&info, pairs[i]);
    if (p->parts > 1) { strlist_add(&info, "Bag-Count"); strlist_add(&info, count); }
    strlist_add(&info, "Payload-Oxum"); strlist_add(&info, oxum);
    strlist_add(&info, "Bag-Software-Agent"); strlist_add(&info, agent);
    write_bag_tags(p->stage, &p->payload, &info);
    strlist_free(&info);
    free(ext_desc); free(group); free(count); free(oxum); free(agent);

    /* the catalogue snapshot: earlier discs as this disc may carry them, and this batch */
    size_t b0 = !strcmp(o->snapshot, "disc") ? idx : 0, b1 = !strcmp(o->snapshot, "disc") ? idx + 1 : mk->nplans;
    strlist prior = { 0 };
    for (size_t i = 0; i < cat->discs.n; i++)
        if (prior_disc(mk, cat->discs.v[i])) strlist_add(&prior, rec_get(cat->discs.v[i], "Id"));
    archive snap;
    archive_shared_subset(cat, &prior, &snap);
    for (size_t b = b0; b < b1; b++) recs_add(&snap.discs, mk->plans[b].disc);
    for (size_t b = b0; b < b1; b++) recs_add(&snap.bindings, mk->plans[b].binding);
    for (size_t b = b0; b < b1; b++) for (size_t i = 0; i < mk->plans[b].events.n; i++) recs_add(&snap.events, mk->plans[b].events.v[i]);
    for (size_t b = b0; b < b1; b++) for (size_t i = 0; i < mk->plans[b].appraisals.n; i++) recs_add(&snap.appraisals, mk->plans[b].appraisals.v[i]);
    if (!strcmp(o->snapshot, "full")) for (size_t i = 0; i < cat->locations.n; i++) recs_add(&snap.locations, cat->locations.v[i]);
    else archive_locations_for(cat, &snap.discs, &snap.locations);
    strlist snap_ids = { 0 };
    for (size_t i = 0; i < snap.discs.n; i++) strlist_add(&snap_ids, rec_get(snap.discs.v[i], "Id"));
    archive_selections_for(cat, &snap_ids, &snap.selections);
    for (size_t i = 0; i < cat->collections.n; i++) {    /* collections and their histories: all of them in a
                                                           full snapshot, else this disc's own */
        rec_record *c = cat->collections.v[i];
        if (strcmp(o->snapshot, "full") && c != mk->collection) continue;
        recs_add(&snap.collections, c);
        for (size_t k = 0; k < cat->revisions.n; k++)
            if (rec_get(cat->revisions.v[k], "Collection") && rec_get(c, "Uuid")
                && !strcmp(rec_get(cat->revisions.v[k], "Collection"), rec_get(c, "Uuid")))
                recs_add(&snap.revisions, cat->revisions.v[k]);
    }
    if (mk->revision) recs_add(&snap.revisions, mk->revision);
    if (!strcmp(o->snapshot, "full")) {          /* the history of the places and selections it carries */
        strlist carried = { 0 };
        for (size_t i = 0; i < snap.locations.n; i++) {
            char *k = xprintf("location:%s", rec_get(snap.locations.v[i], "Code"));
            strlist_add(&carried, k);
            free(k);
        }
        for (size_t i = 0; i < snap.selections.n; i++) {
            char *k = xprintf("selection:%s", rec_get(snap.selections.v[i], "Code"));
            strlist_add(&carried, k);
            free(k);
        }
        for (size_t i = 0; i < cat->events.n; i++)
            if (rec_get(cat->events.v[i], "Object") && strlist_has(&carried, rec_get(cat->events.v[i], "Object")))
                recs_add(&snap.events, cat->events.v[i]);
        for (size_t i = 0; i < snap.discs.n; i++)
            if (rec_get(snap.discs.v[i], "Set")) {
                char *k = xprintf("set:%s", rec_get(snap.discs.v[i], "Set"));
                if (!strlist_has(&carried, k)) strlist_add(&carried, k);
                free(k);
            }
        for (size_t i = 0; i < cat->appraisals.n; i++)
            if (rec_get(cat->appraisals.v[i], "Target") && strlist_has(&carried, rec_get(cat->appraisals.v[i], "Target")))
                recs_add(&snap.appraisals, cat->appraisals.v[i]);
        strlist_free(&carried);
    }
    char *cat_dir = join(p->stage, "catalog"), *vol_dir = join(cat_dir, "volumes");
    if (mkdirs(vol_dir)) die("cannot create %s", vol_dir);
    {
        recs all = { 0 };
        rec_record *info_rec = rec_alloc("Snapshot");
        char *n = xprintf("%zu", snap.discs.n);
        rec_add(info_rec, "Date", mk->today);
        rec_add(info_rec, "Scope", o->snapshot);
        rec_add(info_rec, "Discs", n);
        recs_add(&all, descriptor("Snapshot"));
        recs_add(&all, info_rec);
        archive_records(&snap, &all);
        char *path = join(cat_dir, "archive.rec");
        write_records(path, &all);
        free(path);
        free(n);
        free(all.v);
    }
    static const char *const KINDS[] = { "manifest.sha256", "listing.tsv", "formats.csv", "tags.tsv", "extents.tsv", NULL };
    for (size_t i = 0; i < prior.n; i++) {               /* earlier discs' file lists (not sealed ones) */
        rec_record *d = archive_disc(cat, prior.v[i]);
        if (!d || !strcmp(disc_access(d), "sealed")) continue;
        for (int k = 0; KINDS[k]; k++) {
            char *from = home_volume_file(mk->h, prior.v[i], KINDS[k]);
            if (!access(from, F_OK)) {
                char *dir = xprintf("%s/%s", vol_dir, prior.v[i]), *to = join(dir, KINDS[k]);
                if (mkdirs(dir)) die("cannot create %s", dir);
                copy_file(from, to);
                free(dir);
                free(to);
            }
            free(from);
        }
    }
    for (size_t b = b0; b < b1; b++) {                   /* this batch's discs */
        char *dir = xprintf("%s/%s", vol_dir, mk->plans[b].disc_id), *m = join(dir, "manifest.sha256"), *l = join(dir, "listing.tsv");
        if (mkdirs(dir)) die("cannot create %s", dir);
        copy_file(mk->plans[b].b_manifest, m);
        copy_file(mk->plans[b].b_listing, l);
        if (mk->plans[b].b_formats) {
            char *fm = join(dir, "formats.csv");
            copy_file(mk->plans[b].b_formats, fm);
            free(fm);
        }
        if (mk->plans[b].b_tags) {
            char *tg = join(dir, "tags.tsv");
            copy_file(mk->plans[b].b_tags, tg);
            free(tg);
        }
        free(dir); free(m); free(l);
    }

    /* tools/, README.txt, index.html */
    char *tools = join(p->stage, "tools");
    stage_tools(tools, mk->source, mk->is_git, mk->workdir, o);
    free(tools);
    {
        long image_sectors = !o->no_ecc && mk->capacity ? mk->capacity / GF_FIELDMAX * GF_FIELDMAX : 0;
        sbuf plain = { 0 }, size_check = { 0 }, underline = { 0 };
        const char *creator = mk->creator;
        char *plain_text = xprintf("This is an archive disc%s%s, made on %s: %s. Its files are ordinary files in the "
                                   "data/ folder, and any computer can open them.",
                                   creator && *creator ? " by " : "", creator && *creator ? creator : "", mk->today, mk->title);
        fill(&plain, plain_text, 76, "");
        char *size_text = image_sectors
            ? xprintf("The image must be %ld sectors (%ld bytes). If dvdisaster gives a smaller one, read again "
                      "with --ignore-iso-size; an end that cannot be read at all, the repair fills in.",
                      image_sectors, image_sectors * SECTOR)
            : xstrdup("The image is larger than the filesystem. If dvdisaster does not mention RS03 error "
                      "correction while reading, read again with --ignore-iso-size.");
        fill(&size_check, size_text, 76, "     ");
        sb_puts(&size_check, "\n");
        for (size_t i = utf8_chars(mk->title); i > 0; i--) sb_puts(&underline, "=");
        int disc_only = !strcmp(o->snapshot, "disc");
        char *other = disc_only ? xstrdup(" its notes") : xprintf(" lists the discs made before it (%s catalogue)", o->snapshot);
        const char *cat_lines = disc_only
            ? "  catalog/volumes/<id>/   this disc's file list (listing.tsv) and checksums\n"
            : "  catalog/archive.rec     all discs in the archive as of the burn date\n"
              "  catalog/volumes/<id>/   per disc: manifest.sha256, listing.tsv, formats.csv\n";
        char *part = p->parts > 1 ? xprintf("  (part %d of %d)", p->part, p->parts) : xstrdup("");
        char *nfiles = xprintf("%zu", p->files.n), *nbytes = xstrdup(rec_get(p->disc, "Bytes"));
        char *ape = find_ape(mk->source);
        /* the medium size dvdisaster needs (-n) when the error correction's own copies of it are lost */
        char *dv_n = mk->capacity && !mk->o->no_ecc ? xprintf(" -n %ld", mk->capacity) : xstrdup("");
        char *n_note = mk->capacity && !mk->o->no_ecc ? xprintf(",\n     and -n %ld (this disc's medium size) tells it where to look", mk->capacity) : xstrdup("");
        const char *names[] = { "plain", "size_check", "title", "underline", "id", "set", "part", "date", "files",
                                "bytes", "software", "other_discs", "catalog_lines", "repo", "bundle_line", "ape_use",
                                "ape_line", "dvdisaster_n", "n_note", NULL };
        const char *values[] = { plain.s, size_check.s, mk->title, underline.s ? underline.s : "", p->disc_id, mk->set_code,
                                 part, mk->today, nfiles, nbytes, mk->software, other, cat_lines, "arv",
                                 o->tools_history ? "  tools/arv.bundle     the same with full history: git clone <bundle>\n" : "",
                                 ape ? APE_USE : "", ape ? APE_LINE : "", dv_n, n_note };
        char *text = format(DATA_README, names, values), *path = join(p->stage, "README.txt");
        free(ape); free(dv_n); free(n_note);
        write_text(path, text);
        free(path); free(text); free(other); free(part); free(nfiles); free(nbytes);
        free(plain.s); free(size_check.s); free(underline.s); free(plain_text); free(size_text);
    }
    {
        char *html = render_index(p->disc, p->binding, &p->payload, &snap, archive_where), *path = join(p->stage, "index.html");
        write_text(path, html);
        free(path);
        free(html);
    }

    /* catalog.rec: the Archive entry record, then this disc's own records */
    {
        recs all = { 0 };
        rec_record *arc = rec_alloc("Archive");
        rec_add(arc, "Format", FORMAT_NAME);
        rec_add(arc, "Version", FORMAT_VERSION);
        rec_add(arc, "Disc", p->disc_id);
        rec_add(arc, "Uuid", p->uuid);
        const char *pointers[][2] = { { "Manifest", "manifest-sha256.txt" }, { "Listing", "catalog/volumes/%s/listing.tsv" },
                                      { "Tags", "catalog/volumes/%s/tags.tsv" }, { "Formats", "catalog/volumes/%s/formats.csv" },
                                      { "Snapshot", "catalog/archive.rec" }, { "Viewer", "index.html" }, { "Payload", "data/" } };
        for (int i = 0; i < 7; i++) {
            char *rel = strstr(pointers[i][1], "%s") ? xprintf(pointers[i][1], p->disc_id) : xstrdup(pointers[i][1]);
            if (!strcmp(pointers[i][0], "Payload") || has(p->stage, rel)) rec_add(arc, pointers[i][0], rel);
            free(rel);
        }
        recs_add(&all, descriptor("Archive"));
        recs_add(&all, arc);
        archive own;
        memset(&own, 0, sizeof own);
        recs_add(&own.discs, p->disc);
        recs_add(&own.bindings, p->binding);
        for (size_t i = 0; i < p->events.n; i++) recs_add(&own.events, p->events.v[i]);
        for (size_t i = 0; i < p->appraisals.n; i++) recs_add(&own.appraisals, p->appraisals.v[i]);
        if (mk->collection) {                   /* the collection, and the edition this disc is part of */
            recs_add(&own.collections, mk->collection);
            recs_add(&own.revisions, mk->revision);
        }
        archive_locations_for(cat, &own.discs, &own.locations);
        archive_records(&own, &all);
        char *path = join(p->stage, "catalog.rec");
        write_records(path, &all);
        free(path);
        free(all.v);
    }
    write_tagmanifests(p->stage);
    strlist_free(&prior);
    strlist_free(&snap_ids);
    free(cat_dir);
    free(vol_dir);
}

/* the exact size, by building the image (kept for building the disc) */
static void measure(maker *mk, plan *p)
{
    p->built = xprintf("%s.udf", p->stage);
    p->extents = xprintf("%s.extents.tsv", p->stage);
    int whole = p->parts == 1 && !mk->noted->n && !mk->left_out;     /* the whole folder: empty folders too */
    for (size_t i = 0; i < p->files.n && whole; i++) whole = !*p->files.v[i].link && !p->files.v[i].via_folder;
    char volume_set[17];
    size_t k = 0;
    for (const char *c = p->uuid; *c && k < 16; c++) if (*c != '-') volume_set[k++] = *c;
    volume_set[k] = 0;
    char stamp[32];            /* the recording time: the record's Date at midnight UTC */
    snprintf(stamp, sizeof stamp, "%sT00:00:00Z", mk->today);
    p->sectors = build_image(p->stage, mk->src, &p->files, &p->extras, whole, p->built, p->extents, p->label, p->disc_id, volume_set,
                             (int64_t)parse_utc(stamp));
}

static void free_plans(maker *mk)
{
    for (size_t i = 0; i < mk->nplans; i++) {
        plan *p = &mk->plans[i];
        if (p->stage) remove_tree(p->stage);
        if (p->built) unlink(p->built);
        if (p->extents) unlink(p->extents);
        free(p->files.v);
        free(p->noted.v);
        for (size_t k = 0; k < p->extras.n; k++) {
            free(p->extras.v[k].path);
            free(p->extras.v[k].source);
        }
        free(p->extras.v);
        free(p->payload.v);
    }
    free(mk->plans);
    mk->plans = NULL;
    mk->nplans = 0;
    char *batch = join(mk->workdir, "batch");
    remove_tree(batch);
    free(batch);
}

/* stage and measure every disc; move files forward until every disc fits (make.Maker.fit) */
static void fit(maker *mk)
{
    size_t *counts, nbins = initial_bins(mk, &counts);
    size_t attempts = mk->files->n + 10;
    for (size_t attempt = 0; attempt < attempts; attempt++) {
        assign(mk, counts, nbins);
        batch_files(mk);
        size_t over = (size_t)-1;
        for (size_t i = 0; i < mk->nplans; i++) {
            stage_plan(mk, i);
            measure(mk, &mk->plans[i]);
            if (mk->budget && (long)mk->plans[i].sectors > mk->budget && over == (size_t)-1) over = i;
        }
        if (over == (size_t)-1) {
            free(counts);
            return;
        }
        plan *p = &mk->plans[over];
        if (p->files.n == 1) die("%s does not fit on one disc together with the catalogue and tools", p->files.v[0].path);
        if (!mk->o->split) {
            char need[32], room[32], *red = redundancy_text(mk->o);
            human_size(p->sectors * SECTOR, need);
            human_size((uint64_t)mk->budget * SECTOR, room);
            fprintf(stderr, "Error: this folder needs %s but a %s holds %s at %s%% minimum redundancy.\n"
                            "Use --split (about %ld discs), a larger --medium, or a lower --min-redundancy.\n",
                    need, mk->medium_label, room, red, ((long)p->sectors + mk->budget - 1) / mk->budget);
            if (!mk->o->keep_stage) remove_tree(mk->workdir);
            exit(1);
        }
        long excess = (long)p->sectors - mk->budget + 64, cost = 0;
        size_t moved = 0;
        while (counts[over] - moved > 1 && cost < excess) {     /* from the end of the disc that is over */
            const entry *e = &p->files.v[counts[over] - moved - 1];
            cost += (long)((e->size + SECTOR - 1) / SECTOR) + 1;
            moved++;
        }
        long over_by = (long)p->sectors - mk->budget;
        counts[over] -= moved;
        if (over + 1 < nbins) counts[over + 1] += moved;
        else {
            counts = xrealloc(counts, (nbins + 1) * sizeof *counts);
            counts[nbins++] = moved;
        }
        size_t kept = 0;
        for (size_t i = 0; i < nbins; i++) if (counts[i]) counts[kept++] = counts[i];
        nbins = kept;
        free_plans(mk);
        fprintf(stderr, "Rebalancing: disc %zu was %ld sectors over budget\n", over + 1, over_by);
    }
    die("could not fit the files onto discs after %s attempts", "several");
}

static int make_discs(maker *mk)
{
    const options *o = mk->o;
    char *out_dir;
    if (o->output) {
        out_dir = realpath(o->output, NULL) ? realpath(o->output, NULL) : xstrdup(o->output);
        char *slash = strrchr(out_dir, '/');
        if (slash) *slash = 0;
        else { free(out_dir); out_dir = xstrdup("."); }
    } else {
        out_dir = xstrdup(o->output_dir ? o->output_dir : ".");
    }
    if (mkdirs(out_dir)) die("cannot create %s", out_dir);
    char *abs_out = realpath(out_dir, NULL);      /* absolute: tools are copied with git -C elsewhere */
    if (!abs_out) die("cannot read %s", out_dir);
    free(out_dir);
    out_dir = abs_out;
    mk->workdir = xprintf("%s/.archive-make-XXXXXX", out_dir);
    if (!mkdtemp(mk->workdir)) die("cannot create a work folder in %s", out_dir);
    if (o->ro_crate) {
        sbuf clash = { 0 };
        for (size_t i = 0; i < mk->files->n; i++)
            if (!strcmp(mk->files->v[i].path, "ro-crate-metadata.json") || !strcmp(mk->files->v[i].path, "ro-crate-preview.html"))
                sb_printf(&clash, "%s%s", clash.s ? ", " : "", mk->files->v[i].path);
        if (clash.s) {
            remove_tree(mk->workdir);
            die("--ro-crate would overwrite %s in the source folder", clash.s);
        }
    }
    if (!strcmp(o->formats, "yes") || (!strcmp(o->formats, "auto") && on_path("sf"))) {
        if (!on_path("sf")) die("%s", "--formats yes needs Siegfried (sf) on PATH");
        fputs("Identifying file formats with Siegfried ...\n", stderr);
        char *error = NULL;
        if (!formats_identify(mk->src, o->sf_home, mk->workdir, &mk->fmt, &error)) mk->have_formats = 1;
        else if (!strcmp(o->formats, "yes")) die("Siegfried failed: %s", error);
        else fprintf(stderr, "Warning: skipping format identification, Siegfried failed: %s\n", error);
        free(error);
    }
    fit(mk);
    int failed = 0;
    for (size_t i = 0; i < mk->nplans; i++) {          /* build: move the measured image into place, then RS03 */
        plan *p = &mk->plans[i];
        char size[32];
        human_size(p->sectors * SECTOR, size);
        fprintf(stderr, "Building %s (%d of %d, %s) ...\n", p->out, p->part, p->parts, size);
        if (rename(p->built, p->out)) {
            copy_file(p->built, p->out);
            unlink(p->built);
        }
        char *note = xprintf("image %s, %llu sectors", strrchr(p->out, '/') ? strrchr(p->out, '/') + 1 : p->out,
                             (unsigned long long)p->sectors);
        rec_record *creation = new_event(p->disc_id, "creation", "success", mk->software, "automatic", note);
        recs_add(&p->events, creation);
        if (o->no_ecc) { free(note); continue; }
        fprintf(stderr, "Adding RS03 error correction ...\n");
        rs03_layout lay;
        char err[512], line[200];
        if (rs03_augment(p->out, (uint64_t)mk->capacity, o->no_defect_management, &lay, err, sizeof err)) die("RS03: %s", err);
        rs03_describe(&lay, line, sizeof line);
        if (!rec_get(p->binding, "MediumSectors")) {   /* --medium auto: the one RS03 chose (dvdisaster -n needs it) */
            char *ms = xprintf("%llu", (unsigned long long)lay.medium_sectors);
            rec_add(p->binding, "MediumSectors", ms);
            free(ms);
        }
        char *n2 = xprintf("%s; RS03: %s", note, line);
        rec_set(creation, "Note", n2);
        free(n2);
        free(note);
        if (!o->no_verify) {         /* every sector read back: data against its CRC, parity against the data */
            fprintf(stderr, "Testing the image ...\n");
            rs03_report r;
            int ok = !rs03_verify(p->out, &r, err, sizeof err) && r.header_ok && !r.bad_data && !r.bad_crc && !r.bad_ecc
                     && r.lay.total_sectors == lay.total_sectors;
            recs_add(&p->events, new_event(p->disc_id, "fixity check", ok ? "success" : "failure", mk->software, "automatic",
                                           "image test after creation"));
            if (!ok) {
                fprintf(stderr, "Error: the image test failed for %s\n", p->disc_id);
                failed = 1;
            }
        }
    }
    for (size_t i = 0; i < mk->nplans; i++) {          /* the finished image, as it is to be burned: a disc read back */
        plan *p = &mk->plans[i];                        /* whole gives the same (home and later discs only) */
        char hex[65], *n;
        uint64_t bytes;
        fprintf(stderr, "Hashing %s ...\n", p->out);
        if (hash_file(p->out, hex, -1, &bytes)) die("cannot read %s", p->out);
        rec_add(p->binding, "ImageSectors", n = xprintf("%llu", (unsigned long long)(bytes / SECTOR)));
        rec_add(p->binding, "ImageSha256", hex);
        free(n);
    }
    for (size_t i = 0; i < mk->nplans; i++) {          /* record them at home */
        plan *p = &mk->plans[i];
        recs_add(&mk->cat->discs, p->disc);
        recs_add(&mk->cat->bindings, p->binding);
        for (size_t k = 0; k < p->events.n; k++) recs_add(&mk->cat->events, p->events.v[k]);
        for (size_t k = 0; k < p->appraisals.n; k++) recs_add(&mk->cat->appraisals, p->appraisals.v[k]);
        char *home_vol = xprintf("%s/volumes/%s", mk->h->catalog_dir, p->disc_id);
        if (mkdirs(home_vol)) die("cannot create %s", home_vol);
        const char *kinds[] = { "manifest.sha256", "listing.tsv", "formats.csv", "tags.tsv", "extents.tsv" };
        char *own = xprintf("%s/catalog/volumes/%s", p->stage, p->disc_id);
        char *from[] = { join(own, "manifest.sha256"), join(own, "listing.tsv"), join(own, "formats.csv"), join(own, "tags.tsv"),
                         xstrdup(p->extents) };
        for (int k = 0; k < 5; k++) {       /* extents: kept at home and on later discs, never on this one */
            if (!access(from[k], F_OK)) {
                char *to = join(home_vol, kinds[k]);
                copy_file(from[k], to);
                free(to);
            }
            free(from[k]);
        }
        free(own);
        free(home_vol);
    }
    if (mk->revision) {         /* the edition in the collection's history, and its manifest */
        recs_add(&mk->cat->revisions, mk->revision);
        char *path = revision_manifest_path(mk->h, rec_get(mk->revision, "Node")), *dir = xstrdup(path);
        *strrchr(dir, '/') = 0;
        if (mkdirs(dir)) die("cannot create %s", dir);
        write_text(path, mk->tree_text);
        fprintf(stderr, "%s: edition %s (%s), revision %.12s, %s\n", rec_get(mk->collection, "Code"),
                rec_get(mk->revision, "Edition"), rec_get(mk->revision, "Stage"), rec_get(mk->revision, "Node"),
                rec_get(mk->revision, "Changes"));
        free(path);
        free(dir);
    }
    archive_save(mk->cat, mk->h->rec_path);
    if (mk->collection) hash_cache_note(mk->h, mk->files);
    if (o->keep_stage) fprintf(stderr, "Kept staging directory %s\n", mk->workdir);
    else remove_tree(mk->workdir);
    for (size_t i = 0; i < mk->nplans; i++) printf("%s\t%s\t%s\n", mk->plans[i].disc_id, mk->plans[i].out, mk->title);
    return failed;
}

int cmd_make(int argc, char **argv)
{
    options o;
    if (parse_options(argc, argv, &o)) return 2;
    interactive = isatty(0) && !o.yes;
    char *src = realpath(o.source, NULL);
    struct stat st;
    if (!src || stat(src, &st) || !S_ISDIR(st.st_mode)) die("%s is not a directory", abs_path(o.source));
    if (o.output && o.output_dir) die("%s", "use either --output or --output-dir");
    char *review = o.review ? review_date(o.review) : NULL;
    if (o.importance.n || o.basis) (void)new_appraisal("X", &o.importance, o.basis, review);   /* checked early */
    else if (o.review) die("%s", "--review needs --importance or --basis");

    arv_home h;
    home_find(&h, o.home, src);
    archive cat;
    archive_load(&cat, h.rec_path);

    /* a collection's workflow folder: the collection gives what the options leave open */
    char *coll_uuid = marker_collection(src);
    rec_record *coll = folder_collection(&cat, src, NULL);   /* its marker, or a declaration (arv link) */
    if (coll_uuid && !coll) die("this folder's .arv marker names collection %s, which is not in the home catalogue", coll_uuid);
    if (coll) {
        if (!o.title || !*o.title) o.title = rec_get(coll, "Title");
        if ((!o.description || !*o.description) && rec_get(coll, "Description")) o.description = rec_get(coll, "Description");
        if ((!o.set || !*o.set) && rec_get(coll, "Set")) o.set = rec_get(coll, "Set");
        if (!o.categories.n)
            for (size_t i = 0; i < coll->nfields; i++)
                if (!strcmp(coll->fields[i].name, "Category")) strlist_add(&o.categories, coll->fields[i].value);
        if (!o.access_given && rec_get(coll, "Access")) o.access = rec_get(coll, "Access");
        fprintf(stderr, "Collection %s (%s): its next edition\n", rec_get(coll, "Code"), rec_get(coll, "Title"));
    } else if (o.final || o.message) {
        die("%s", "--final and --message are for a collection's workflow folder (arv collection init)");
    }

    fprintf(stderr, "Scanning and hashing %s ...\n", src);
    size_t left_out = 0;
    entries files, noted;
    scan_payload(src, o.links, &files, &noted);
    {   /* the .arv at the folder's root is arv's own (a marker, pointer or home), never content */
        size_t kept = 0;
        for (size_t i = 0; i < files.n; i++)
            if (strcmp(files.v[i].path, ".arv") && strncmp(files.v[i].path, ".arv/", 5)) files.v[kept++] = files.v[i];
        left_out = files.n - kept;
        files.n = kept;
    }
    char *summary = link_summary(&files, &noted, o.links);
    if (summary) fprintf(stderr, "Links: %s\n", summary + strlen("links: "));
    {
        char **paths = xmalloc((files.n + 1) * sizeof *paths);
        for (size_t i = 0; i < files.n; i++) paths[i] = files.v[i].path;
        name_issues issues = { 0 };
        names_check(paths, files.n, &issues);
        size_t errors = 0;
        for (size_t i = 0; i < issues.n; i++) errors += issues.v[i].error;
        if (errors) {
            fprintf(stderr, "Error: %zu file name(s) cannot be stored in a udf250 image (rename them):\n", errors);
            for (size_t i = 0, shown = 0; i < issues.n && shown < 10; i++)
                if (issues.v[i].error) {
                    fprintf(stderr, "  error: %s: %s\n", issues.v[i].path, issues.v[i].problem);
                    shown++;
                }
            exit(1);
        }
        if (issues.n && !o.ignore_names) {
            fprintf(stderr, "Note: %zu file name(s) will look different on Windows/macOS (the manifests and Linux "
                            "keep them exactly; --ignore-names hides this):\n", issues.n);
            for (size_t i = 0; i < issues.n && i < 10; i++)
                fprintf(stderr, "  warning: %s: %s\n", issues.v[i].path, issues.v[i].problem);
        }
        names_free(&issues);

        /* a draft fills what the options leave empty (cli.cmd_make): no questions for those */
        draft dr;
        memset(&dr, 0, sizeof dr);
        if (o.draft) {
            draft_load(o.draft, &dr, 1);
            if (!strcmp(dr.authorship, "suggested") || !strcmp(dr.authorship, "accepted")) {
                int same = o.subjects.n == dr.subjects.n;      /* a model's draft overridden: edited by a person */
                for (size_t i = 0; same && i < o.subjects.n; i++) same = !strcmp(o.subjects.v[i], dr.subjects.v[i]);
                if ((o.title && (!dr.title || strcmp(o.title, dr.title)))
                    || (o.description && (!dr.description || strcmp(o.description, dr.description)))
                    || (o.subjects.n && !same)) {
                    free(dr.authorship);
                    dr.authorship = xstrdup("edited");
                }
            }
            if (!o.title || !*o.title) o.title = dr.title;
            if (!o.description || !*o.description) o.description = dr.description;
            if (!o.subjects.n)
                for (size_t i = 0; i < dr.subjects.n; i++) strlist_add(&o.subjects, dr.subjects.v[i]);
            for (size_t i = 0; i < dr.notes.n; i++) strlist_add(&o.notes, dr.notes.v[i]);
            for (size_t i = 0; i < dr.nft; i++) tags_canonical(&h, &dr.ft_tags[i]);   /* aliases become names */
        }

        /* coverage: the files' years, or --coverage */
        char coverage[64];
        if (o.coverage) {
            if (discid_to_edtf(o.coverage, coverage, sizeof coverage))
                die("--coverage: %s is not a date or range (examples: 2019, 2015/2024, 2019-07/2019-08, 199X, 1995~)", o.coverage);
        } else {
            int lo = 9999, hi = 0;
            for (size_t i = 0; i < files.n; i++) {
                struct tm tm;
                localtime_r(&files.v[i].mtime, &tm);
                if (tm.tm_year + 1900 < lo) lo = tm.tm_year + 1900;
                if (tm.tm_year + 1900 > hi) hi = tm.tm_year + 1900;
            }
            if (!files.n) {
                struct tm tm;
                today_tm(&tm);
                lo = hi = tm.tm_year + 1900;
            }
            if (lo == hi) snprintf(coverage, sizeof coverage, "%d", lo);
            else snprintf(coverage, sizeof coverage, "%d/%d", lo, hi);
        }

        /* the set and categories, from the vocabulary */
        vocab v;
        char err[512];
        char *sets_path = join(h.config_dir, "sets.rec");
        if (access(sets_path, F_OK)) {
            if (mkdirs(h.config_dir)) die("cannot create %s", h.config_dir);
            write_text(sets_path, DATA_DEFAULT_SETS);
        }
        if (vocab_load(&v, sets_path, NULL, err, sizeof err)) die("%s", err);
        strlist rules = { 0 };
        if (!o.no_rules) vocab_rule_suggestions(&v, paths, files.n, &rules);
        char *default_set, *default_title = folder_default_title(src, &default_set);
        const char *guessed = vocab_guess(&v, default_set);
        const char *set_default = guessed ? guessed : rules.n ? rules.v[0] : default_set;
        char *set_asked = o.set && *o.set ? NULL : ask("Set code (see 'arv sets')", set_default);
        char *set_code = pick_code(&v, o.set && *o.set ? o.set : set_asked ? set_asked : "");
        strlist categories = { 0 }, typed = { 0 };
        if (o.categories.n) {
            for (size_t i = 0; i < o.categories.n; i++) strlist_add(&typed, o.categories.v[i]);
        } else {
            for (size_t i = 0; i < rules.n && typed.n < 3; i++)
                if (strcmp(rules.v[i], set_code) && !vocab_is_ancestor(&v, rules.v[i], set_code))
                    strlist_add(&typed, rules.v[i]);
            if (interactive) {
                sbuf l = { 0 };
                for (size_t i = 0; i < typed.n; i++) sb_printf(&l, "%s%s", i ? ", " : "", typed.v[i]);
                char *answer = ask("Extra categories, comma separated (optional)", l.s);
                strlist_free(&typed);
                split_commas(answer, &typed, 0);
                free(answer);
                free(l.s);
            } else if (typed.n) {
                sbuf l = { 0 };
                for (size_t i = 0; i < typed.n; i++) sb_printf(&l, "%s%s", i ? ", " : "", typed.v[i]);
                fprintf(stderr, "Categories from Match rules in %s: %s (--category to choose, --no-rules to skip)\n",
                        sets_path, l.s);
                free(l.s);
            }
        }
        for (size_t i = 0; i < typed.n; i++) {
            if (!*typed.v[i] || strspn(typed.v[i], " ") == strlen(typed.v[i])) continue;
            char *c = pick_code(&v, typed.v[i]);
            if (strcmp(c, set_code) && !strlist_has(&categories, c)) strlist_add(&categories, c);
            free(c);
        }
        strlist all_paths = { 0 };
        for (size_t i = 0; i <= categories.n; i++) {
            const char *code = i ? categories.v[i - 1] : set_code;
            strlist p = { 0 };
            vocab_paths(&v, code, &p);
            if (p.n) {
                for (size_t k = 0; k < p.n; k++) strlist_add(&all_paths, p.v[k]);
            } else {
                strlist near = { 0 };
                vocab_near(&v, code, &near);
                sbuf hint = { 0 };
                for (size_t k = 0; k < near.n; k++) sb_printf(&hint, "%s%s", k ? ", " : " (did you mean ", near.v[k]);
                if (near.n) sb_puts(&hint, "?)");
                fprintf(stderr, "Warning: %s is not in %s%s. It is used anyway, without a place in the vocabulary.\n",
                        code, sets_path, hint.s ? hint.s : "");
                free(hint.s);
                strlist_free(&near);
            }
            strlist_free(&p);
        }
        if (all_paths.n) {
            sbuf l = { 0 };
            for (size_t i = 0; i < all_paths.n; i++) sb_printf(&l, "%s%s", i ? ", " : "", all_paths.v[i]);
            fprintf(stderr, "Classified as: %s\n", l.s);
            free(l.s);
        }

        /* the questions after classifying (cli.cmd_make's meta), when asking */
        if (interactive) {
            if (!o.title || !*o.title) o.title = ask("Title", default_title);
            if (!o.description || !*o.description) o.description = ask("Description (optional)", NULL);
            if (!o.creator || !*o.creator) o.creator = ask("Creator", getenv("USER"));
            if (!o.location || !*o.location) o.location = ask("Physical location (optional; see 'arv location list')", NULL);
            if (!o.subjects.n) {
                char *answer = ask("Subjects, comma separated (optional)", NULL);
                split_commas(answer, &o.subjects, 1);
                free(answer);
            }
            if (!o.notes.n) {
                char *answer = ask("Note (optional)", NULL);
                if (answer) strlist_add(&o.notes, answer);
                free(answer);
            }
        }

        /* --------------------------------------------------------------- the plan */
        maker mk;
        memset(&mk, 0, sizeof mk);
        mk.o = &o;
        mk.h = &h;
        mk.cat = &cat;
        mk.src = src;
        mk.files = &files;
        mk.noted = &noted;
        mk.draft = o.draft ? &dr : NULL;
        mk.title = o.title ? o.title : default_title;
        mk.creator = o.creator ? o.creator : getenv("USER");
        if (o.location && *o.location) mk.location = place(&cat, o.location);
        mk.set_code = set_code;
        mk.collection = coll;
        mk.left_out = left_out;
        mk.id_prefix = coll ? rec_get(coll, "Code") : set_code;
        if (coll) mk.tree_text = tree_manifest(&files);
        mk.categories = &categories;
        mk.paths = &all_paths;
        snprintf(mk.coverage, sizeof mk.coverage, "%s", coverage);
        mk.medium_label = "auto";
        if (o.medium_sectors) {
            mk.capacity = o.medium_sectors;
            mk.medium_label = "custom medium";
        } else if (strcmp(o.medium, "auto")) {
            for (int i = 0; i < 4; i++)
                if (!strcmp(MEDIA[i].name, o.medium)) {
                    mk.capacity = o.no_defect_management ? MEDIA[i].nodm : MEDIA[i].dm;
                    mk.medium_label = MEDIA[i].label;
                }
        }
        mk.budget = mk.capacity ? data_budget(mk.capacity, o.min_redundancy) : 0;
        mk.source = find_source(o.tools);
        mk.software = software_version(mk.source, &mk.is_git);
        today_iso(mk.today);
        mk.review = review;
        for (int i = 0; i < argc; i++) mk.links_chosen |= !strcmp(argv[i], "--links");
        return make_discs(&mk);
    }
}
