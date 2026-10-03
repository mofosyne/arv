/* Finding the home catalogue, in the order src/arv/homes.py uses (see its docstring):
 * --home (-C), $ARV_HOME, a .arv folder / .arv pointer file / disc root from the folder being
 * archived or the current folder up, the machine config, then $XDG_DATA_HOME/arv. */
#define _XOPEN_SOURCE 700
#include "arvc.h"

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

static char *walk_up(const char *start, const char **how)
{
    char *folder = absolute(start);
    for (;;) {
        char *cand = join(folder, ".arv"), *found = NULL;
        if (is_dir(cand)) {
            found = xstrdup(cand);
            *how = "a .arv folder";
        } else if (is_reg(cand)) {
            found = read_pointer(cand);
            *how = "a .arv pointer file";
        } else {
            char *rec = join(folder, "catalog.rec"), *cat = join(folder, "catalog/archive.rec");
            if (is_reg(rec) && is_reg(cat)) {
                found = join(folder, "catalog");
                *how = "an archive disc";
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

static char *configured_home(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char *path = xdg && *xdg ? join(xdg, "arv/homes.rec") : home ? xprintf("%s/.config/arv/homes.rec", home) : NULL;
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
    const char *how = NULL;
    char *found = NULL;
    memset(h, 0, sizeof *h);
    if (given) {
        found = xstrdup(given);
        how = "-C";
    }
    for (const char *const *var = (const char *const[]){ "ARV_HOME", "BLURAY_ARCHIVE_HOME", NULL }; !found && *var; var++)
        if (getenv(*var) && *getenv(*var)) {
            found = xstrdup(getenv(*var));
            how = *var;
        }
    if (!found && source && is_dir(source)) found = walk_up(source, &how);
    if (!found) found = walk_up(".", &how);
    if (!found && (found = configured_home())) how = "the machine config";
    if (!found) {
        const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
        char *base = xdg && *xdg ? xstrdup(xdg) : xprintf("%s/.local/share", home ? home : ".");
        char *old = join(base, "bluray-archive");
        found = is_dir(old) ? old : join(base, "arv");
        if (found != old) free(old);
        free(base);
        how = "the fallback home";
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

/* arvc init [FOLDER]: a .arv home in FOLDER (default: the current folder) */
int cmd_init(int argc, char **argv)
{
    if (argc > 1) return 2;
    char *folder = absolute(argc ? argv[0] : ".");
    char *target = join(folder, ".arv"), *git = join(folder, ".git");
    struct stat st;
    if (!lstat(target, &st)) die("%s already exists", target);
    if (is_dir(git))
        fprintf(stderr, "Note: %s is a git repository; a .arv in the folder above it can cover several "
                        "repositories and stays out of git\n", folder);
    arv_home h;
    memset(&h, 0, sizeof h);
    h.path = target;
    h.catalog_dir = join(target, "catalog");
    h.config_dir = join(target, "config");
    h.drafts_dir = join(target, "drafts");
    h.cache_dir = join(target, "cache");
    home_ensure(&h);
    printf("Created %s\n", target);
    return 0;
}
