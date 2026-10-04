/*
 * arvc make: one disc from a folder, without prompts (src/arv/cli.py cmd_make and src/arv/make.py
 * Maker, for a single UDF 2.50 disc). The disc it writes is the same as the Python arv's: BagIt
 * tag files, the catalogue snapshot, catalog.rec, README.txt, index.html and tools/, the image
 * written by udfwrite (linked in), and dvdisaster RS03 error correction.
 *
 * Not ported (use the Python arv): --split, --filesystem hybrid, --udf-writer udfmake, drafts
 * and the local AI helpers, Siegfried format identification, --ro-crate, --tools-history.
 */
#define _XOPEN_SOURCE 700
#include "arvc.h"
#include "data.h"
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
#define FORMAT_VERSION "0.4"
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
    const char *basis, *review;
    strlist categories, subjects, notes, importance;
    long medium_sectors;
    double min_redundancy;
    int no_rules, no_ecc, no_verify, no_defect_management, keep_stage, ignore_names, label_given, redundancy_given;
} options;

static const char HELP[] =
    "usage: arvc make [options] FOLDER\n"
    "Makes one archive disc image of FOLDER and records it in the home catalogue (no prompts).\n"
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
    "  --links POLICY         default, record or copy (docs/smart-archive-format.md, Links)\n"
    "  --importance 'LEVEL for AUDIENCE' (repeatable), --basis TEXT, --review DATE|5y\n"
    "                         appraise the disc (the archivist log)\n"
    "  --medium bd25|bd50|bd100|bd128|auto, --medium-sectors N, --no-defect-management\n"
    "  --min-redundancy PERCENT  RS03 minimum (default: 20)\n"
    "  --media TEXT           media description (default: M-DISC <medium>)\n"
    "  --snapshot full|set|disc  the catalogue the disc carries (default: full)\n"
    "  --tools DIR            arv's source for tools/ (default: found next to this program)\n"
    "  --no-ecc, --no-verify  skip RS03, or skip dvdisaster -t afterwards\n"
    "  --ignore-names, --keep-stage\n"
    "Not here (use the Python arv): --split, --filesystem hybrid, drafts and AI help,\n"
    "format identification, --ro-crate.\n";

static int parse_options(int argc, char **argv, options *o)
{
    memset(o, 0, sizeof *o);
    o->access = "private";
    o->links = "default";
    o->medium = "bd25";
    o->snapshot = "full";
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
        if (!strcmp(a, "-y") || !strcmp(a, "--yes")) continue;      /* arvc never asks */
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(HELP, stdout);
            exit(0);
        }
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
        else if (!strcmp(a, "--access")) str = &o->access;
        else if (!strcmp(a, "--rights")) str = &o->rights;
        else if (!strcmp(a, "--links")) str = &o->links;
        else if (!strcmp(a, "--medium")) str = &o->medium;
        else if (!strcmp(a, "--media")) str = &o->media;
        else if (!strcmp(a, "--snapshot")) str = &o->snapshot;
        else if (!strcmp(a, "--tools")) str = &o->tools;
        else if (!strcmp(a, "--basis")) str = &o->basis;
        else if (!strcmp(a, "--review")) str = &o->review;
        else if (!strcmp(a, "--category")) list = &o->categories;
        else if (!strcmp(a, "--subject")) list = &o->subjects;
        else if (!strcmp(a, "--note")) list = &o->notes;
        else if (!strcmp(a, "--importance")) list = &o->importance;
        else if (!strcmp(a, "--medium-sectors")) { o->medium_sectors = atol(v); continue; }
        else if (!strcmp(a, "--min-redundancy")) { o->min_redundancy = atof(v); o->redundancy_given = 1; continue; }
        else {
            fprintf(stderr, "arvc make: unknown option %s\n", a);
            return 2;
        }
        if (str) *str = v;
        else strlist_add(list, v);
    }
    if (!o->source) return 2;
    const char *ok[][5] = { { "private", "public", "sealed", NULL }, { "default", "record", "copy", NULL },
                            { "full", "set", "disc", NULL }, { "bd25", "bd50", "bd100", "bd128", "auto" } };
    const char *val[] = { o->access, o->links, o->snapshot, o->medium };
    const char *what[] = { "--access", "--links", "--snapshot", "--medium" };
    for (int k = 0; k < 4; k++) {
        int good = 0;
        for (int j = 0; j < 5 && ok[k][j]; j++) good |= !strcmp(val[k], ok[k][j]);
        if (!good) {
            fprintf(stderr, "arvc make: %s %s is not one of the choices\n", what[k], val[k]);
            return 2;
        }
    }
    return 0;
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

static int dvdisaster_sets_medium_size(void)
{
    char *out = NULL;
    char *argv[] = { "dvdisaster", "--help", NULL };
    run(argv, &out);
    int yes = out && strstr(out, "no-bdr-defect-management") != NULL;
    free(out);
    return yes;
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

/* the arv source tree to put in tools/: --tools, $ARV_SOURCE, an installed share/arv, or the
 * checkout this program was built in */
static char *find_source(const char *given)
{
    if (given) return xstrdup(given);
    if (getenv("ARV_SOURCE") && *getenv("ARV_SOURCE")) return xstrdup(getenv("ARV_SOURCE"));
    char *exe = exe_dir(), *found = NULL;
    if (exe) {
        char *installed = xprintf("%s/../share/arv", exe), *checkout = xprintf("%s/../../..", exe);
        if (has(installed, "src/arv/vendor/bagit.py")) found = realpath(installed, NULL);
        else if (has(checkout, "src/arvc/arvc.c")) found = realpath(checkout, NULL);
        free(installed);
        free(checkout);
        free(exe);
    }
    return found;
}

/* "arvc@<commit>" (+uncommitted), from git in a checkout or VERSION in an installed tree */
static char *software_version(const char *source, int *is_git)
{
    *is_git = 0;
    if (source && has(source, ".git") && on_path("git")) {
        char *out = NULL, *dirty = NULL;
        char *a[] = { "git", "-C", (char *)source, "rev-parse", "--short=12", "HEAD", NULL };
        char *b[] = { "git", "-C", (char *)source, "status", "--porcelain", "--untracked-files=no", NULL };
        if (run(a, &out) == 0 && run(b, &dirty) == 0) {
            out[strcspn(out, "\n")] = 0;
            char *v = xprintf("arvc@%s%s", out, *dirty ? "+uncommitted" : "");
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
            char *v = xprintf("arvc@%s", strncmp(t, "arv@", 4) ? t : t + 4);
            free(t);
            return v;
        }
    }
    return xstrdup("arvc@unknown");
}

static void stage_tools(const char *tools, const char *source, int is_git, const char *workdir)
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
    if (source) {
        char *from = join(source, "src/arv/vendor/bagit.py"), *to = join(tools, "bagit.py");
        if (!access(from, F_OK)) copy_file(from, to);
        free(from);
        free(to);
    }
    free(tree);
}

/* ------------------------------------------------------------------ the catalogue */

static char *review_date(const char *text)
{
    char unit;
    int n;
    if (sscanf(text, " %d %c", &n, &unit) == 2 && (unit == 'y' || unit == 'm' || unit == 'Y' || unit == 'M') && n >= 0) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
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
static uint64_t build_image(const char *stage, const char *src, const entries *files, int whole_folder,
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
    if (whole_folder) {
        add_tree(w, &c, src, "data");                   /* empty folders included */
    } else {
        for (size_t i = 0; i < files->n; i++) {         /* a folder with links: exactly the listed files */
            char *path = join("data", files->v[i].path);
            if (stat(files->v[i].source, &st)) die("cannot read %s", files->v[i].source);
            if (udfw_add_file(w, path, files->v[i].size, (int64_t)st.st_mtime, (unsigned)st.st_mode, read_cb,
                              (void *)(uintptr_t)add_source(&c, files->v[i].source)))
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

static void add_volume_files(strlist *kinds, strlist *paths, const char *kind, const char *path)
{
    if (access(path, F_OK)) return;
    strlist_add(kinds, kind);
    strlist_add(paths, path);
}

int cmd_make(int argc, char **argv)
{
    options o;
    if (parse_options(argc, argv, &o)) return 2;
    char *src = realpath(o.source, NULL);
    struct stat st;
    if (!src || stat(src, &st) || !S_ISDIR(st.st_mode)) die("%s is not a directory", o.source);
    if (o.output && o.output_dir) die("%s", "use either --output or --output-dir");
    if (!o.no_ecc && !on_path("dvdisaster")) die("%s", "missing required tool(s): dvdisaster (or --no-ecc for a test image)");
    char *review = o.review ? review_date(o.review) : NULL;
    if (o.importance.n || o.basis) (void)new_appraisal("X", &o.importance, o.basis, review);   /* checked early */
    else if (o.review) die("%s", "--review needs --importance or --basis");

    arv_home h;
    home_find(&h, o.home, src);
    archive cat;
    archive_load(&cat, h.rec_path);

    fprintf(stderr, "Scanning and hashing %s ...\n", src);
    entries files, noted;
    scan_payload(src, o.links, &files, &noted);
    char *summary = link_summary(&files, &noted, o.links);
    if (summary) fprintf(stderr, "Links: %s\n", summary + strlen("links: "));
    {
        char **paths = xmalloc((files.n + 1) * sizeof *paths);
        for (size_t i = 0; i < files.n; i++) paths[i] = files.v[i].path;
        name_issues issues = { 0 };
        names_check(paths, files.n, 1, &issues);
        size_t errors = 0;
        for (size_t i = 0; i < issues.n; i++) errors += issues.v[i].error;
        if (errors) {
            fprintf(stderr, "arvc: %zu file name(s) cannot be stored in a udf250 image (rename them):\n", errors);
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
                time_t t = time(NULL);
                struct tm tm;
                localtime_r(&t, &tm);
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
        const char *set_text = o.set ? o.set : guessed ? guessed : rules.n ? rules.v[0] : default_set;
        char *set_code = pick_code(&v, set_text);
        strlist categories = { 0 }, typed = { 0 };
        if (o.categories.n) {
            for (size_t i = 0; i < o.categories.n; i++) strlist_add(&typed, o.categories.v[i]);
        } else {
            for (size_t i = 0; i < rules.n && typed.n < 3; i++)
                if (strcmp(rules.v[i], set_code) && !vocab_is_ancestor(&v, rules.v[i], set_code))
                    strlist_add(&typed, rules.v[i]);
            if (typed.n) {
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

        /* --------------------------------------------------------------- the plan */
        const char *title = o.title ? o.title : default_title;
        const char *creator = o.creator ? o.creator : getenv("USER");
        char *location = NULL;
        if (o.location && *o.location) {
            rec_record *l = archive_location(&cat, o.location);
            if (l) location = xstrdup(rec_get(l, "Code"));
            else {
                const char *s = o.location;
                while (isspace((unsigned char)*s)) s++;
                location = xstrdup(s);
                size_t n = strlen(location);
                while (n && isspace((unsigned char)location[n - 1])) location[--n] = 0;
            }
        }
        long capacity = 0;
        const char *medium_label = "auto";
        if (o.medium_sectors) {
            capacity = o.medium_sectors;
            medium_label = "custom medium";
        } else if (strcmp(o.medium, "auto")) {
            for (int i = 0; i < 4; i++)
                if (!strcmp(MEDIA[i].name, o.medium)) {
                    capacity = o.no_defect_management ? MEDIA[i].nodm : MEDIA[i].dm;
                    medium_label = MEDIA[i].label;
                }
        }
        long budget = capacity ? data_budget(capacity, o.min_redundancy) : 0;

        long sequence = archive_next_number(&cat, set_code);
        char disc_id[64];
        if (o.id) snprintf(disc_id, sizeof disc_id, "%s", o.id);
        else if (discid_compose(set_code, sequence, coverage, disc_id, sizeof disc_id))
            die("cannot make a disc id from set %s and coverage %s", set_code);
        int id_ok = isalnum((unsigned char)disc_id[0]) && strlen(disc_id) <= 32;
        for (const char *p = disc_id; *p; p++) id_ok &= isalnum((unsigned char)*p) || *p == '_' || *p == '-';
        if (!id_ok) die("invalid disc id %s (letters, digits, _ and -; at most 32 characters)", disc_id);
        if (archive_disc(&cat, disc_id)) die("disc id %s already exists in the catalogue", disc_id);
        char *out;
        if (o.output) out = realpath(o.output, NULL) ? realpath(o.output, NULL) : xstrdup(o.output);
        else {
            const char *dir = o.output_dir ? o.output_dir : ".";
            if (mkdirs(dir)) die("cannot create %s", dir);
            char *absdir = realpath(dir, NULL);
            out = xprintf("%s/%s%s.iso", absdir, disc_id, o.no_ecc ? ".noecc" : "");
            free(absdir);
        }
        if (!access(out, F_OK)) die("%s already exists", out);
        char *label = volume_label(disc_id, o.label_given ? o.label : title, 1);

        char *software, *source = find_source(o.tools);
        int is_git;
        software = software_version(source, &is_git);
        char today[11], uuid[37];
        today_iso(today);
        uuid4(uuid);
        uint64_t bytes = 0;
        for (size_t i = 0; i < files.n; i++) bytes += files.v[i].size;

        rec_record *disc = rec_alloc("Disc");
        rec_add(disc, "Id", disc_id);
        rec_add(disc, "Uuid", uuid);
        if (!o.id) rec_add(disc, "IdScheme", DISCID_SCHEME);
        if (strcmp(label, disc_id)) rec_add(disc, "Label", label);
        rec_add(disc, "Title", title);
        rec_add(disc, "Set", set_code);
        for (size_t i = 0; i < categories.n; i++) rec_add(disc, "Category", categories.v[i]);
        for (size_t i = 0; i < all_paths.n; i++) rec_add(disc, "Path", all_paths.v[i]);
        char *seq = xprintf("%ld", sequence);
        rec_add(disc, "Sequence", seq);
        rec_add(disc, "Coverage", coverage);
        rec_add(disc, "Date", today);
        if (creator && *creator) rec_add(disc, "Creator", creator);
        if (o.description && *o.description) rec_add(disc, "Description", o.description);
        for (size_t i = 0; i < o.subjects.n; i++) rec_add(disc, "Subject", o.subjects.v[i]);
        for (size_t i = 0; i < o.notes.n; i++) rec_add(disc, "Note", o.notes.v[i]);
        if (location) rec_add(disc, "Location", location);
        rec_add(disc, "Access", o.access);
        if (o.rights) rec_add(disc, "Rights", o.rights);
        char *nfiles = xprintf("%zu", files.n), *nbytes = xprintf("%llu", (unsigned long long)bytes);
        rec_add(disc, "Files", nfiles);
        rec_add(disc, "Bytes", nbytes);
        rec_add(disc, "Software", software);

        rec_record *binding = rec_alloc("Binding");
        rec_add(binding, "Volume", disc_id);
        rec_add(binding, "Container", "udf-2.50");
        rec_add(binding, "Protection", o.no_ecc ? "none" : "rs03");
        char *media = o.media ? xstrdup(o.media) : xprintf("M-DISC %s", capacity ? medium_label : "BD-R");
        rec_add(binding, "Media", media);
        rec_add(binding, "Filesystem", "UDF 2.50, BD-ROM layout with metadata partition and a real mirror (arv udfwrite)");
        char *ecc;
        if (o.no_ecc) ecc = xstrdup("none");
        else if (capacity) {
            char red[40];          /* as Python prints it: the default is the int 20, a given value a float */
            snprintf(red, sizeof red, "%.15g", o.min_redundancy);
            if (o.redundancy_given && !strchr(red, '.') && !strchr(red, 'e')) strcat(red, ".0");
            ecc = xprintf("dvdisaster RS03 augmented image, %s (%ld sectors), minimum %s%% redundancy", medium_label,
                          capacity, red);
        } else ecc = xstrdup("dvdisaster RS03 augmented image");
        rec_add(binding, "Ecc", ecc);
        if (capacity && !o.no_ecc) {
            char *ms = xprintf("%ld", capacity);
            rec_add(binding, "MediumSectors", ms);
            free(ms);
        }

        recs events = { 0 }, appraisals = { 0 };
        char *digest_note = xprintf("sha256 and sha512 manifests of %zu files", files.n);
        recs_add(&events, new_event(disc_id, "message digest calculation", "success", software, "automatic", digest_note));
        if (summary) {
            int chosen = 0;
            for (int i = 0; i < argc; i++) chosen |= !strcmp(argv[i], "--links");
            char *who = chosen ? person() : xstrdup(software);
            recs_add(&events, new_event(disc_id, "ingestion", "success", who, chosen ? "human" : "automatic", summary));
            free(who);
        }
        if (o.importance.n || o.basis) recs_add(&appraisals, new_appraisal(disc_id, &o.importance, o.basis, review));

        /* --------------------------------------------------------------- staging */
        char *out_dir = xstrdup(out), *slash = strrchr(out_dir, '/');
        if (slash) *slash = 0;
        char *workdir = xprintf("%s/.archive-make-XXXXXX", out_dir);
        if (!mkdtemp(workdir)) die("cannot create a work folder in %s", out_dir);
        char *batch = join(workdir, "batch"), *stage = xprintf("%s/stage-%s", workdir, disc_id);
        if (mkdirs(batch) || mkdirs(stage)) die("cannot create %s", stage);
        char *b_manifest = xprintf("%s/%s.sha256", batch, disc_id), *b_listing = xprintf("%s/%s.tsv", batch, disc_id);
        write_manifest(b_manifest, &files, 0);
        write_listing(b_listing, &files, &noted);

        /* BagIt */
        strlist info = { 0 };
        char *ext_desc = o.description && *o.description ? xprintf("%s - %s", title, o.description) : xstrdup(title);
        char *oxum = xprintf("%llu.%zu", (unsigned long long)bytes, files.n), *agent = xprintf("%s <%s>", software, URL);
        const char *pairs[] = { "Bagging-Date", today, "External-Identifier", disc_id, "External-Description", ext_desc,
                                "Bag-Group-Identifier", set_code, "Payload-Oxum", oxum, "Bag-Software-Agent", agent };
        for (int i = 0; i < 12; i++) strlist_add(&info, pairs[i]);
        write_bag_tags(stage, &files, &info);

        /* the catalogue snapshot */
        strlist prior = { 0 };
        for (size_t i = 0; i < cat.discs.n; i++) {
            const rec_record *d = cat.discs.v[i];
            if (!strcmp(o.snapshot, "full") ||
                (!strcmp(o.snapshot, "set") && rec_get(d, "Set") && !strcmp(rec_get(d, "Set"), set_code) &&
                 !strcmp(disc_access(d), "public")))
                strlist_add(&prior, rec_get(d, "Id"));
        }
        archive snap;
        archive_shared_subset(&cat, &prior, &snap);
        recs_add(&snap.discs, disc);
        recs_add(&snap.bindings, binding);
        for (size_t i = 0; i < events.n; i++) recs_add(&snap.events, events.v[i]);
        for (size_t i = 0; i < appraisals.n; i++) recs_add(&snap.appraisals, appraisals.v[i]);
        if (!strcmp(o.snapshot, "full")) for (size_t i = 0; i < cat.locations.n; i++) recs_add(&snap.locations, cat.locations.v[i]);
        else archive_locations_for(&cat, &snap.discs, &snap.locations);
        strlist snap_ids = { 0 };
        for (size_t i = 0; i < snap.discs.n; i++) strlist_add(&snap_ids, rec_get(snap.discs.v[i], "Id"));
        archive_collections_for(&cat, &snap_ids, &snap.collections);
        if (!strcmp(o.snapshot, "full")) {          /* the history of the places and collections it carries */
            strlist carried = { 0 };
            for (size_t i = 0; i < snap.locations.n; i++) {
                char *k = xprintf("location:%s", rec_get(snap.locations.v[i], "Code"));
                strlist_add(&carried, k);
                free(k);
            }
            for (size_t i = 0; i < snap.collections.n; i++) {
                char *k = xprintf("collection:%s", rec_get(snap.collections.v[i], "Code"));
                strlist_add(&carried, k);
                free(k);
            }
            for (size_t i = 0; i < cat.events.n; i++)
                if (rec_get(cat.events.v[i], "Object") && strlist_has(&carried, rec_get(cat.events.v[i], "Object")))
                    recs_add(&snap.events, cat.events.v[i]);
            for (size_t i = 0; i < snap.discs.n; i++)
                if (rec_get(snap.discs.v[i], "Set")) {
                    char *k = xprintf("set:%s", rec_get(snap.discs.v[i], "Set"));
                    if (!strlist_has(&carried, k)) strlist_add(&carried, k);
                    free(k);
                }
            for (size_t i = 0; i < cat.appraisals.n; i++)
                if (rec_get(cat.appraisals.v[i], "Target") && strlist_has(&carried, rec_get(cat.appraisals.v[i], "Target")))
                    recs_add(&snap.appraisals, cat.appraisals.v[i]);
            strlist_free(&carried);
        }
        char *cat_dir = join(stage, "catalog"), *vol_dir = join(cat_dir, "volumes");
        if (mkdirs(vol_dir)) die("cannot create %s", vol_dir);
        {
            recs all = { 0 };
            rec_record *info_rec = rec_alloc("Snapshot");
            char *count = xprintf("%zu", snap.discs.n);
            rec_add(info_rec, "Date", today);
            rec_add(info_rec, "Scope", o.snapshot);
            rec_add(info_rec, "Discs", count);
            recs_add(&all, descriptor("Snapshot"));
            recs_add(&all, info_rec);
            archive_records(&snap, &all);
            char *p = join(cat_dir, "archive.rec");
            write_records(p, &all);
            free(p);
            free(count);
            free(all.v);
        }
        static const char *const KINDS[] = { "manifest.sha256", "listing.tsv", "formats.csv", "tags.tsv", "extents.tsv", NULL };
        for (size_t i = 0; i < prior.n; i++) {               /* earlier discs' file lists (not sealed ones) */
            rec_record *d = archive_disc(&cat, prior.v[i]);
            if (!d || !strcmp(disc_access(d), "sealed")) continue;
            for (int k = 0; KINDS[k]; k++) {
                char *from = home_volume_file(&h, prior.v[i], KINDS[k]);
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
        char *own_dir = xprintf("%s/%s", vol_dir, disc_id), *own_manifest = join(own_dir, "manifest.sha256"),
             *own_listing = join(own_dir, "listing.tsv");
        if (mkdirs(own_dir)) die("cannot create %s", own_dir);
        copy_file(b_manifest, own_manifest);
        copy_file(b_listing, own_listing);

        /* tools/, README.txt, index.html */
        char *tools = join(stage, "tools");
        stage_tools(tools, source, is_git, workdir);
        {
            long image_sectors = !o.no_ecc && capacity && dvdisaster_sets_medium_size() ? capacity / GF_FIELDMAX * GF_FIELDMAX : 0;
            sbuf plain = { 0 }, size_check = { 0 }, underline = { 0 };
            char *plain_text = xprintf("This is an archive disc%s%s, made on %s: %s. Its files are ordinary files in the "
                                       "data/ folder, and any computer can open them.",
                                       creator && *creator ? " by " : "", creator && *creator ? creator : "", today, title);
            fill(&plain, plain_text, 76, "");
            char *size_text = image_sectors
                ? xprintf("The image must be %ld sectors (%ld bytes). If it comes out smaller, the error correction "
                          "was not found; read again with --ignore-iso-size.", image_sectors, image_sectors * SECTOR)
                : xstrdup("The image is larger than the filesystem. If dvdisaster does not mention RS03 error "
                          "correction while reading, read again with --ignore-iso-size.");
            fill(&size_check, size_text, 76, "     ");
            sb_puts(&size_check, "\n");
            for (size_t i = utf8_chars(title); i > 0; i--) sb_puts(&underline, "=");
            int disc_only = !strcmp(o.snapshot, "disc");
            char *other = disc_only ? xstrdup(" its notes") : xprintf(" lists the discs made before it (%s catalogue)", o.snapshot);
            const char *cat_lines = disc_only
                ? "  catalog/volumes/<id>/   this disc's file list (listing.tsv) and checksums\n"
                : "  catalog/archive.rec     all discs in the archive as of the burn date\n"
                  "  catalog/volumes/<id>/   per disc: manifest.sha256, listing.tsv, formats.csv\n";
            const char *names[] = { "plain", "size_check", "title", "underline", "id", "set", "part", "date", "files",
                                    "bytes", "software", "other_discs", "catalog_lines", "repo", "bundle_line", NULL };
            const char *values[] = { plain.s, size_check.s, title, underline.s ? underline.s : "", disc_id, set_code, "",
                                     today, nfiles, nbytes, software, other, cat_lines, "arv", "" };
            char *text = format(DATA_README, names, values), *p = join(stage, "README.txt");
            write_text(p, text);
            free(p);
            free(text);
            free(other);
            free(plain.s);
            free(size_check.s);
            free(underline.s);
            free(plain_text);
            free(size_text);
        }
        {
            char *html = render_index(disc, binding, &files, &snap, archive_where), *p = join(stage, "index.html");
            write_text(p, html);
            free(p);
            free(html);
        }

        /* catalog.rec: the Archive entry record, then this disc's own records */
        {
            recs all = { 0 };
            rec_record *arc = rec_alloc("Archive");
            rec_add(arc, "Format", FORMAT_NAME);
            rec_add(arc, "Version", FORMAT_VERSION);
            rec_add(arc, "Disc", disc_id);
            rec_add(arc, "Uuid", uuid);
            const char *pointers[][2] = { { "Manifest", "manifest-sha256.txt" }, { "Listing", "catalog/volumes/%s/listing.tsv" },
                                          { "Tags", "catalog/volumes/%s/tags.tsv" }, { "Formats", "catalog/volumes/%s/formats.csv" },
                                          { "Snapshot", "catalog/archive.rec" }, { "Viewer", "index.html" }, { "Payload", "data/" } };
            for (int i = 0; i < 7; i++) {
                char *rel = strstr(pointers[i][1], "%s") ? xprintf(pointers[i][1], disc_id) : xstrdup(pointers[i][1]);
                if (!strcmp(pointers[i][0], "Payload") || has(stage, rel)) rec_add(arc, pointers[i][0], rel);
                free(rel);
            }
            recs_add(&all, descriptor("Archive"));
            recs_add(&all, arc);
            archive own;
            memset(&own, 0, sizeof own);
            recs_add(&own.discs, disc);
            recs_add(&own.bindings, binding);
            for (size_t i = 0; i < events.n; i++) recs_add(&own.events, events.v[i]);
            for (size_t i = 0; i < appraisals.n; i++) recs_add(&own.appraisals, appraisals.v[i]);
            archive_locations_for(&cat, &own.discs, &own.locations);
            archive_records(&own, &all);
            char *p = join(stage, "catalog.rec");
            write_records(p, &all);
            free(p);
            free(all.v);
        }
        write_tagmanifests(stage);

        /* --------------------------------------------------------------- the image */
        char *built = xprintf("%s.udf", stage), *extents = xprintf("%s.extents.tsv", stage);
        int whole = !noted.n;
        for (size_t i = 0; i < files.n && whole; i++) whole = !*files.v[i].link && !files.v[i].via_folder;
        char volume_set[17];
        size_t k = 0;
        for (const char *p = uuid; *p && k < 16; p++) if (*p != '-') volume_set[k++] = *p;
        volume_set[k] = 0;
        char stamp[32];            /* the recording time: the record's Date at midnight UTC */
        snprintf(stamp, sizeof stamp, "%sT00:00:00Z", today);
        int64_t when = (int64_t)parse_utc(stamp);
        uint64_t sectors = build_image(stage, src, &files, whole, built, extents, label, disc_id, volume_set, when);
        if (budget && (long)sectors > budget) {
            char need[32], room[32];
            human_size(sectors * SECTOR, need);
            human_size((uint64_t)budget * SECTOR, room);
            fprintf(stderr, "arvc: this folder needs %s but a %s holds %s at %g%% minimum redundancy.\n"
                            "Use a larger --medium, a lower --min-redundancy, or the Python arv's --split.\n",
                    need, medium_label, room, o.min_redundancy);
            if (!o.keep_stage) remove_tree(workdir);
            exit(1);
        }
        char size[32];
        human_size(sectors * SECTOR, size);
        fprintf(stderr, "Building %s (1 of 1, %s) ...\n", out, size);
        if (rename(built, out)) {               /* another file system: copy */
            copy_file(built, out);
            unlink(built);
        }
        char *note = xprintf("image %s, %llu sectors", strrchr(out, '/') ? strrchr(out, '/') + 1 : out,
                             (unsigned long long)sectors);
        rec_record *creation = new_event(disc_id, "creation", "success", software, "automatic", note);
        recs_add(&events, creation);
        int failed = 0;
        if (!o.no_ecc) {
            fprintf(stderr, "Adding dvdisaster RS03 error correction ...\n");
            char threads[16], ms[32], *output = NULL;
            snprintf(threads, sizeof threads, "%ld", sysconf(_SC_NPROCESSORS_ONLN) > 0 ? sysconf(_SC_NPROCESSORS_ONLN) : 1);
            snprintf(ms, sizeof ms, "%ld", capacity);
            char *a[] = { "dvdisaster", "-i", out, "-mRS03", "-o", "image", "-c", "--no-progress", "-x", threads,
                          capacity && dvdisaster_sets_medium_size() ? "-n" : NULL, ms, NULL };
            if (run(a, &output)) die("dvdisaster failed:\n%s", output ? output : "");
            for (char *l = strtok(output, "\n"); l; l = strtok(NULL, "\n"))
                if (strstr(l, "redundancy")) {
                    while (isspace((unsigned char)*l)) l++;
                    char *n2 = xprintf("%s; RS03: %s", note, l);
                    size_t e = strlen(n2);
                    while (e && isspace((unsigned char)n2[e - 1])) n2[--e] = 0;
                    rec_set(creation, "Note", n2);
                    free(n2);
                }
            free(output);
            if (!o.no_verify) {
                fprintf(stderr, "Verifying with dvdisaster -t ...\n");
                char *t[] = { "dvdisaster", "-i", out, "-t", "--no-progress", NULL };
                int rc = run(t, &output);
                char *lower_out = xstrdup(output ? output : "");
                for (char *p = lower_out; *p; p++) *p = (char)tolower((unsigned char)*p);
                int ok = rc == 0 && strstr(output, "all sectors present") && !strstr(lower_out, "fail");
                recs_add(&events, new_event(disc_id, "fixity check", ok ? "success" : "failure", "dvdisaster", "automatic",
                                            "image test after creation"));
                if (!ok) {
                    fprintf(stderr, "%s\narvc: dvdisaster verification failed for %s\n", output, disc_id);
                    failed = 1;
                }
                free(output);
                free(lower_out);
            }
        }

        /* --------------------------------------------------------------- record it at home */
        recs_add(&cat.discs, disc);
        recs_add(&cat.bindings, binding);
        for (size_t i = 0; i < events.n; i++) recs_add(&cat.events, events.v[i]);
        for (size_t i = 0; i < appraisals.n; i++) recs_add(&cat.appraisals, appraisals.v[i]);
        strlist kinds = { 0 }, from = { 0 };
        add_volume_files(&kinds, &from, "manifest.sha256", own_manifest);
        add_volume_files(&kinds, &from, "listing.tsv", own_listing);
        add_volume_files(&kinds, &from, "extents.tsv", extents);    /* kept at home and on later discs only */
        char *home_vol = xprintf("%s/volumes/%s", h.catalog_dir, disc_id);
        if (mkdirs(home_vol)) die("cannot create %s", home_vol);
        for (size_t i = 0; i < kinds.n; i++) {
            char *to = join(home_vol, kinds.v[i]);
            copy_file(from.v[i], to);
            free(to);
        }
        archive_save(&cat, h.rec_path);
        if (o.keep_stage) fprintf(stderr, "Kept staging directory %s\n", workdir);
        else remove_tree(workdir);
        printf("%s\t%s\t%s\n", disc_id, out, title);
        return failed;
    }
}
