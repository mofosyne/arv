/* Finding the home catalogue, in the order src/arv/homes.py uses (see its docstring):
 * --home (-C), $ARV_HOME, a .arv folder / .arv pointer file / disc root from the folder being
 * archived or the current folder up, the machine config, then $XDG_DATA_HOME/arv. */
#define _XOPEN_SOURCE 700
#include "arv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int is_dir(const char *p)
{
    struct stat st;
    return !stat(p, &st) && S_ISDIR(st.st_mode);
}

static int is_reg(const char *p)
{
    struct stat st;
    return !stat(p, &st) && S_ISREG(st.st_mode);
}

static char *absolute(const char *p)
{
    if (p[0] == '/') return xstrdup(p);
    char *cwd = getcwd(NULL, 4096), *r = join(cwd ? cwd : ".", p);
    free(cwd);
    return r;
}

/* the home a .arv pointer file names ("Home: PATH", relative to the file's folder) */
static char *read_pointer(const char *file)
{
    rec_file f;
    int bad = 0;
    if (rec_read(file, &f, &bad)) die("cannot read the pointer file %s", file);
    const char *target = NULL;
    for (size_t i = 0; i < f.nrecords && !target; i++) target = rec_get(&f.records[i], "Home");
    if (!target) die("%s is a .arv pointer file without a 'Home: PATH' line", file);
    char *dir = xstrdup(file), *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    char *out;
    if (target[0] == '/') out = xstrdup(target);
    else if (target[0] == '~' && getenv("HOME")) out = xprintf("%s%s", getenv("HOME"), target + 1);
    else out = join(dir, target);
    free(dir);
    rec_free(&f);
    return out;
}

static char *walk_up(const char *start, char **how)
{
    char *folder = absolute(start);
    size_t len = strlen(folder);
    while (len > 1 && folder[len - 1] == '/') folder[--len] = 0;
    if (len > 2 && !strcmp(folder + len - 2, "/.")) folder[len - 2] = 0;    /* "x/." -> "x" */
    for (;;) {
        char *cand = join(folder, ".arv"), *found = NULL;
        if (!strcmp(folder, "/")) { free(cand); cand = xstrdup("/.arv"); }
        if (is_dir(cand)) {
            found = xstrdup(cand);
            *how = xprintf("the .arv folder in %s", folder);
        } else if (is_reg(cand)) {
            found = read_pointer(cand);
            *how = xprintf("the pointer file %s", cand);
        } else {
            char *rec = join(folder, "catalog.rec"), *cat = join(folder, "catalog/archive.rec");
            if (is_reg(rec) && is_reg(cat)) {
                found = join(folder, "catalog");
                *how = xprintf("the archive disc at %s", folder);
            }
            free(rec);
            free(cat);
        }
        free(cand);
        char *slash = strrchr(folder, '/');
        if (found || !slash || slash == folder) {
            free(folder);
            return found;
        }
        *slash = 0;
    }
}

static char *config_path(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    return xdg && *xdg ? join(xdg, "arv/homes.rec") : xprintf("%s/.config/arv/homes.rec", home ? home : "");
}

static char *configured_home(void)
{
    char *path = config_path();
    rec_file f;
    int bad = 0;
    char *out = NULL;
    if (path && is_reg(path) && !rec_read(path, &f, &bad)) {
        const rec_record *only = NULL, *def = NULL;
        size_t n = 0;
        for (size_t i = 0; i < f.nrecords; i++) {
            const rec_record *r = &f.records[i];
            if (r->descriptor || !r->type || strcmp(r->type, "Home")) continue;
            n++;
            only = r;
            const char *d = rec_get(r, "Default");
            if (d && (!strcmp(d, "yes") || !strcmp(d, "Yes") || !strcmp(d, "YES")) && !def) def = r;
        }
        const rec_record *pick = def ? def : n == 1 ? only : NULL;
        if (pick && rec_get(pick, "Path")) out = xstrdup(rec_get(pick, "Path"));
        rec_free(&f);
    }
    free(path);
    return out;
}

static char *named_home(const char *name, char **how);

void home_at(arv_home *h, const char *path)
{
    h->path = xstrdup(path);
    char *vol = join(path, "volumes"), *rec = join(path, "archive.rec");
    h->catalog_dir = is_dir(vol) && is_reg(rec) ? xstrdup(path) : join(path, "catalog");   /* a bare catalog/ */
    free(vol);
    free(rec);
    h->config_dir = join(path, "config");
    h->drafts_dir = join(path, "drafts");
    h->cache_dir = join(path, "cache");
    h->rec_path = join(h->catalog_dir, "archive.rec");
}

void home_find(arv_home *h, const char *given, const char *source)
{
    char *how = NULL;
    char *found = NULL;
    memset(h, 0, sizeof *h);
    if (given) {
        found = xstrdup(given);
        how = xstrdup("--home");
    } else if (home_archive_name) {
        found = named_home(home_archive_name, &how);
    }
    for (const char *const *var = (const char *const[]){ "ARV_HOME", "BLURAY_ARCHIVE_HOME", NULL }; !found && *var; var++)
        if (getenv(*var) && *getenv(*var)) {
            found = xstrdup(getenv(*var));
            how = xprintf("$%s", *var);
        }
    if (!found && source && is_dir(source)) found = walk_up(source, &how);
    if (!found) found = walk_up(".", &how);
    if (!found && (found = configured_home())) {
        char *cp = config_path();
        how = xprintf("the default home in %s", cp);
        free(cp);
    }
    if (!found) {
        const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
        char *base = xdg && *xdg ? xstrdup(xdg) : xprintf("%s/.local/share", home ? home : ".");
        char *old = join(base, "bluray-archive");
        found = is_dir(old) ? old : join(base, "arv");
        if (found != old) free(old);
        free(base);
        char *cwd = getcwd(NULL, 4096);
        how = xprintf("the fallback home (no .arv found above %s)", cwd ? cwd : ".");
        free(cwd);
    }
    home_at(h, found);
    h->how = how;
    free(found);
}

char *home_volume_file(const arv_home *h, const char *disc_id, const char *name)
{
    return xprintf("%s/volumes/%s/%s", h->catalog_dir, disc_id, name);
}

static const char *const CACHEDIR_TAG =
    "Signature: 8a477f597d28d172789f06886806bc55\n"
    "# This folder holds caches made by arv (Archive, Record, Verify).\n"
    "# Everything here can be rebuilt; backup tools may skip it.\n"
    "# See https://bford.info/cachedir/\n";

/* Creates the home's folders; cache/ gets its CACHEDIR.TAG and .gitignore (as catalog.Home.ensure). */
void home_ensure(const arv_home *h)
{
    const char *dirs[] = { h->config_dir, h->catalog_dir, h->drafts_dir, h->cache_dir };
    for (int i = 0; i < 4; i++)
        if (mkdirs(dirs[i])) die("cannot create %s", dirs[i]);
    char *tag = join(h->cache_dir, "CACHEDIR.TAG"), *ign = join(h->cache_dir, ".gitignore");
    if (!is_reg(tag)) write_text(tag, CACHEDIR_TAG);
    if (!is_reg(ign)) write_text(ign, "*\n");
    free(tag);
    free(ign);
}

/* adds or updates a home in the machine config (homes.register) */
static void register_home(const char *name, const char *path, int is_default)
{
    char *cp = config_path();
    rec_file old, out;
    int bad = 0;
    memset(&old, 0, sizeof old);
    memset(&out, 0, sizeof out);
    if (is_reg(cp) && rec_read(cp, &old, &bad)) die("cannot read %s", cp);
    rec_record *d = rec_new(&out, "Home");
    d->descriptor = 1;
    rec_add(d, "%rec", "Home");
    rec_add(d, "%doc", "Archive homes on this machine (paths are local; this file never goes on a disc).\n"
                       "Name is what --archive takes; Default: yes picks the home used outside any .arv tree.");
    rec_add(d, "%key", "Name");
    rec_add(d, "%mandatory", "Name Path");
    rec_add(d, "%type", "Default enum yes no");
    size_t mine = (size_t)-1;
    for (size_t i = 0; i < old.nrecords; i++) {
        const rec_record *r = &old.records[i];
        if (r->descriptor || !r->type || strcmp(r->type, "Home")) continue;
        rec_record *c = rec_new(&out, "Home");
        rec_copy(c, r);
        const char *n = rec_get(r, "Name");
        if (n && !strcmp(n, name) && mine == (size_t)-1) mine = out.nrecords - 1;
    }
    if (mine == (size_t)-1) {
        rec_record *c = rec_new(&out, "Home");
        rec_add(c, "Name", name);
        rec_add(c, "Path", path);
        mine = out.nrecords - 1;
    }
    rec_set(&out.records[mine], "Path", path);
    if (is_default) {
        for (size_t i = 1; i < out.nrecords; i++) {      /* every Default field goes */
            rec_record *r = &out.records[i];
            size_t k = 0;
            for (size_t j = 0; j < r->nfields; j++) {
                if (!strcmp(r->fields[j].name, "Default")) {
                    free(r->fields[j].name);
                    free(r->fields[j].value);
                } else {
                    r->fields[k++] = r->fields[j];
                }
            }
            r->nfields = k;
        }
        rec_add(&out.records[mine], "Default", "yes");
    }
    char *dir = xstrdup(cp), *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    if (mkdirs(dir)) die("cannot create %s", dir);
    rec_record **v = xmalloc(out.nrecords * sizeof *v);
    for (size_t i = 0; i < out.nrecords; i++) v[i] = &out.records[i];
    if (rec_write(cp, v, out.nrecords)) die("cannot write %s", cp);
    free(v);
    free(dir);
    rec_free(&old);
    rec_free(&out);
    free(cp);
}

/* the home registered under NAME, and how it was found (homes.find with --archive) */
static char *named_home(const char *name, char **how)
{
    char *cp = config_path(), *out = NULL;
    rec_file f;
    int bad = 0;
    if (is_reg(cp) && !rec_read(cp, &f, &bad)) {
        for (size_t i = 0; i < f.nrecords && !out; i++) {
            const rec_record *r = &f.records[i];
            const char *n = rec_get(r, "Name");
            if (!r->descriptor && r->type && !strcmp(r->type, "Home") && n && !strcmp(n, name) && rec_get(r, "Path"))
                out = xstrdup(rec_get(r, "Path"));
        }
        rec_free(&f);
    }
    if (!out) {
        fprintf(stderr, "Error: no home named %s in %s (`arv init --name %s` adds one)\n", name, cp, name);
        exit(1);
    }
    *how = xprintf("--archive %s (%s)", name, cp);
    free(cp);
    return out;
}

const char *home_archive_name;      /* --archive NAME, before the command */

/* arv init [FOLDER] [--pointer HOME] [--name NAME [--default]]: a .arv home in FOLDER (default:
 * the current folder), or a .arv pointer file to an existing home */
int cmd_init(int argc, char **argv)
{
    const char *given = NULL, *pointer = NULL, *name = NULL;
    int is_default = 0;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && !strcmp(argv[i], "--pointer")) pointer = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--name")) name = argv[++i];
        else if (!strcmp(argv[i], "--default")) is_default = 1;
        else if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) i++;
        else if (!given && argv[i][0] != '-') given = argv[i];
        else return 2;
    }
    char *folder = realpath(given ? given : ".", NULL);       /* os.path.abspath: no "./" left */
    if (!folder) die("%s is not a folder", given ? given : ".");
    char *target = join(folder, ".arv"), *git = join(folder, ".git");
    struct stat st;
    if (!lstat(target, &st)) die("%s already exists", target);
    if (is_dir(git))
        fprintf(stderr, "Note: %s is a git repository; a .arv in the folder above it can cover several "
                        "repositories and stays out of git\n", folder);
    if (pointer) {
        char *abs = absolute(pointer), *home = abs_path(abs);
        if (!is_dir(home)) die("%s is not a folder", home);
        char *text = xprintf("# This tree belongs to the archive whose catalogue is here (see `arv where`):\n"
                             "Home: %s\n", home);
        write_text(target, text);
        printf("Wrote %s -> %s\n", target, home);
        free(text);
        free(abs);
        free(home);
        return 0;
    }
    arv_home h;
    memset(&h, 0, sizeof h);
    h.path = target;
    h.catalog_dir = join(target, "catalog");
    h.config_dir = join(target, "config");
    h.drafts_dir = join(target, "drafts");
    h.cache_dir = join(target, "cache");
    home_ensure(&h);
    h.rec_path = join(h.catalog_dir, "archive.rec");
    archive cat;                        /* the home's identity, from its first day */
    archive_load(&cat, h.rec_path);
    archive_home_uuid(&cat);
    if (name) rec_add(cat.homes.v[0], "Name", name);
    archive_save(&cat, h.rec_path);
    printf("Created %s\n", target);
    if (name) {
        register_home(name, target, is_default);
        char *cp = config_path();
        printf("Registered as %s in %s%s\n", name, cp, is_default ? " (default)" : "");
        free(cp);
    }
    return 0;
}
