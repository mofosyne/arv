/* Git repositories in a folder being archived (research/plan.md, "Git repositories in a
 * collection"). A disc holds a repository's working tree as plain files and its history as a
 * compacted .git in the same place: HEAD, config, refs, the index and one pack, made with the git
 * program on a copy (the source is never touched). Left out: hooks (code that would run on
 * restore), reflogs, and credentials in remote URLs. --git-history since DATE makes the history
 * shallow (git's own shallow file marks where it stops; hashes are unchanged).
 *
 * Each disc's catalog/volumes/<id>/git.tsv lists, per repository: its root commits (from the
 * full history), heads, shallow boundary and every commit it holds, so a repository anywhere can
 * be recognised later by its history alone (arv status, arv find COMMIT).
 *
 * Without git on PATH a .git is copied as it is, and the ingestion event says so. */
#define _XOPEN_SOURCE 700
#include "arv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int git_ok(char **argv, char **out)
{
    char *o = NULL;
    int rc = run(argv, &o);
    if (out) *out = o;
    else free(o);
    return rc == 0;
}

/* a repository's folder (relative to the source) from the path of its .git/HEAD, or NULL */
static char *repo_of(const char *path)
{
    size_t n = strlen(path);
    const char *tail = ".git/HEAD";
    size_t t = strlen(tail);
    if (n < t || strcmp(path + n - t, tail) || (n > t && path[n - t - 1] != '/')) return NULL;
    char *repo = xmalloc(n - t + 1);
    memcpy(repo, path, n - t);
    repo[n - t] = 0;
    if (n > t) repo[n - t - 1] = 0;                 /* "sub/proj/" -> "sub/proj" */
    if (strstr(repo, ".git/") || !strcmp(repo, ".git") || (strlen(repo) > 4 && !strcmp(repo + strlen(repo) - 5, "/.git"))) {
        free(repo);                                 /* inside another .git (its modules/, say) */
        return NULL;
    }
    return repo;
}

static int set_has(const strlist *sorted, const char *s)
{
    size_t lo = 0, hi = sorted->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(sorted->v[mid], s);
        if (!c) return 1;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

static int by_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void lines(const char *text, strlist *out)
{
    for (const char *l = text; l && *l;) {
        const char *nl = strchr(l, '\n');
        size_t n = nl ? (size_t)(nl - l) : strlen(l);
        if (n) {
            char *s = xmalloc(n + 1);
            memcpy(s, l, n);
            s[n] = 0;
            strlist_add(out, s);
            free(s);
        }
        l = nl ? nl + 1 : l + n;
    }
}

/* "https://user:secret@host/x" -> "https://host/x" */
static char *strip_userinfo(const char *url)
{
    const char *scheme = strstr(url, "://");
    if (!scheme) return NULL;
    const char *host = scheme + 3, *at = strchr(host, '@'), *slash = strchr(host, '/');
    if (!at || (slash && at > slash)) return NULL;
    return xprintf("%.*s%s", (int)(host - url), url, at + 1);
}

/* compacts one repository into dir/.git; fills the git.tsv rows and a note; 0 or -1 */
static int compact(const char *src_repo, const char *dir, const char *rel, const char *since, sbuf *tsv, char **note)
{
    char *from = join(src_repo, ".git"), *gd = join(dir, ".git");
    if (mkdirs(dir)) die("cannot create %s", dir);
    char *cp[] = { "cp", "-Rp", from, gd, NULL };
    if (!git_ok(cp, NULL)) return -1;
    char *gdarg = xprintf("--git-dir=%s", gd);
    const char *name = *rel ? rel : ".";

    /* the true roots, before any trimming */
    char *roots_out = NULL;
    char *roots[] = { "git", gdarg, "rev-list", "--max-parents=0", "--all", NULL };
    if (!git_ok(roots, &roots_out)) { free(roots_out); return -1; }
    strlist rootl = { 0 };
    lines(roots_out, &rootl);
    free(roots_out);

    /* hooks, reflogs and credentials stay behind */
    char *hooks = join(gd, "hooks"), *logs = join(gd, "logs");
    remove_tree(hooks);
    mkdirs(hooks);
    char *stash_log = join(gd, "logs/refs/stash"), *stash = access(stash_log, F_OK) ? NULL : read_text(stash_log);
    remove_tree(logs);                          /* the stash's list is a reflog too: it stays */
    if (stash) {
        char *logdir = join(gd, "logs/refs");
        mkdirs(logdir);
        write_text(stash_log, stash);
        free(logdir);
        free(stash);
    }
    free(stash_log);
    char *urls_out = NULL, *cfg = join(gd, "config");
    char *urls[] = { "git", "config", "--file", cfg, "--get-regexp", "^remote\\..*\\.url$", NULL };
    int scrubbed = 0;
    if (git_ok(urls, &urls_out)) {
        strlist ul = { 0 };
        lines(urls_out, &ul);
        for (size_t i = 0; i < ul.n; i++) {
            char *sp = strchr(ul.v[i], ' ');
            if (!sp) continue;
            *sp = 0;
            char *clean = strip_userinfo(sp + 1);
            if (clean) {
                char *set[] = { "git", "config", "--file", cfg, ul.v[i], clean, NULL };
                git_ok(set, NULL);
                scrubbed++;
                free(clean);
            }
        }
        strlist_free(&ul);
    }
    free(urls_out);
    char *rmcred[] = { "git", "config", "--file", cfg, "--remove-section", "credential", NULL };
    git_ok(rmcred, NULL);

    /* --git-history since DATE: keep the commits since then, and each ref's tip */
    int shallow = 0;
    if (since) {
        char *sincearg = xprintf("--since=%s", since), *out = NULL;
        char *rl[] = { "git", gdarg, "rev-list", "--all", "--parents", sincearg, NULL };
        if (!git_ok(rl, &out)) { free(out); free(sincearg); return -1; }
        strlist rows = { 0 }, keep = { 0 };
        lines(out, &rows);
        free(out);
        for (size_t i = 0; i < rows.n; i++) {
            char c[41];
            if (sscanf(rows.v[i], "%40s", c) == 1) strlist_add(&keep, c);
        }
        char *tips_out = NULL;                       /* every ref's commit stays, even an old one */
        char *tips[] = { "git", gdarg, "for-each-ref", "--format=%(objectname) %(*objectname)", NULL };
        git_ok(tips, &tips_out);
        strlist tipl = { 0 };
        lines(tips_out, &tipl);
        free(tips_out);
        for (size_t i = 0; i < tipl.n; i++) {
            char a[41] = "", b[41] = "";
            int k = sscanf(tipl.v[i], "%40s %40s", a, b);
            const char *commit = k == 2 ? b : a;
            char *t[] = { "git", gdarg, "cat-file", "-t", (char *)commit, NULL }, *type = NULL;
            if (git_ok(t, &type) && !strncmp(type, "commit", 6) && !strlist_has(&keep, commit)) {
                char *pl = NULL, *pr[] = { "git", gdarg, "rev-list", "--parents", "-n", "1", (char *)commit, NULL };
                if (git_ok(pr, &pl)) {
                    lines(pl, &rows);
                    strlist_add(&keep, commit);
                }
                free(pl);
            }
            free(type);
        }
        strlist_free(&tipl);
        qsort(keep.v, keep.n, sizeof *keep.v, by_str);
        sbuf sh = { 0 };
        sb_puts(&sh, "");
        strlist cut = { 0 };
        for (size_t i = 0; i < rows.n; i++) {       /* a kept commit with a parent not kept: the boundary */
            char *save = NULL, *row = xstrdup(rows.v[i]), *c = strtok_r(row, " ", &save);
            int boundary = 0;
            for (char *p = strtok_r(NULL, " ", &save); p; p = strtok_r(NULL, " ", &save)) boundary |= !set_has(&keep, p);
            if (c && boundary && !strlist_has(&cut, c)) {
                strlist_add(&cut, c);
                sb_printf(&sh, "%s\n", c);
            }
            free(row);
        }
        if (cut.n) {
            char *shf = join(gd, "shallow");
            write_text(shf, sh.s);
            free(shf);
            shallow = (int)cut.n;
            for (size_t i = 0; i < cut.n; i++) sb_printf(tsv, "%s\tshallow\t%s\n", name, cut.v[i]);
        }
        strlist_free(&cut);
        strlist_free(&rows);
        strlist_free(&keep);
        free(sh.s);
        free(sincearg);
    }
    char *gc[] = { "git", gdarg, "-c", "gc.reflogExpire=now", "-c", "gc.reflogExpireUnreachable=now",
                   "-c", "gc.refs/stash.reflogExpire=never", "-c", "gc.refs/stash.reflogExpireUnreachable=never",
                   "gc", "--prune=now", "--quiet", NULL };
    char *gc_out = NULL;
    if (!git_ok(gc, &gc_out)) {
        fprintf(stderr, "git gc failed for %s:\n%s", name, gc_out ? gc_out : "");
        free(gc_out);
        return -1;
    }
    free(gc_out);
    char *fsck[] = { "git", gdarg, "fsck", "--connectivity-only", "--no-dangling", "--no-progress", NULL }, *fo = NULL;
    if (!git_ok(fsck, &fo)) {
        fprintf(stderr, "git fsck failed for the compacted copy of %s:\n%s", name, fo ? fo : "");
        free(fo);
        return -1;
    }
    free(fo);

    /* what the disc holds: roots, heads, commits */
    for (size_t i = 0; i < rootl.n; i++) sb_printf(tsv, "%s\troot\t%s\n", name, rootl.v[i]);
    char *refs_out = NULL;
    char *refs[] = { "git", gdarg, "for-each-ref", "--format=%(refname) %(objectname)", NULL };
    git_ok(refs, &refs_out);
    strlist refl = { 0 };
    lines(refs_out, &refl);
    free(refs_out);
    size_t branches = 0, tags = 0;
    char *head_out = NULL, *hd[] = { "git", gdarg, "rev-parse", "--verify", "-q", "HEAD", NULL };
    if (git_ok(hd, &head_out) && *head_out) {
        head_out[strcspn(head_out, "\n")] = 0;
        sb_printf(tsv, "%s\thead\tHEAD %s\n", name, head_out);
    }
    free(head_out);
    for (size_t i = 0; i < refl.n; i++) {
        sb_printf(tsv, "%s\thead\t%s\n", name, refl.v[i]);
        branches += !strncmp(refl.v[i], "refs/heads/", 11);
        tags += !strncmp(refl.v[i], "refs/tags/", 10);
        /* each ref also as a loose file: git needs refs/ to exist, and a disc keeps no empty folders */
        char *sp = strchr(refl.v[i], ' ');
        if (sp && !strncmp(refl.v[i], "refs/", 5)) {
            *sp = 0;
            char *file = join(gd, refl.v[i]), *slash = strrchr(file, '/'), *val = xprintf("%s\n", sp + 1);
            *slash = 0;
            mkdirs(file);
            *slash = '/';
            write_text(file, val);
            *sp = ' ';
            free(file);
            free(val);
        }
    }
    char *cm_out = NULL, *cm[] = { "git", gdarg, "rev-list", "--all", NULL };
    git_ok(cm, &cm_out);
    strlist cml = { 0 };
    lines(cm_out, &cml);
    free(cm_out);
    for (size_t i = 0; i < cml.n; i++) sb_printf(tsv, "%s\tcommit\t%s\n", name, cml.v[i]);
    *note = xprintf("git repository %s: %zu branch%s, %zu tag%s, %zu commit%s, history %s%s; hooks and reflogs (but the "
                    "stash's) left out%s",
                    name, branches, branches == 1 ? "" : "es", tags, tags == 1 ? "" : "s", cml.n, cml.n == 1 ? "" : "s",
                    shallow ? "since " : "full", shallow ? since : "", scrubbed ? ", and credentials in remote URLs" : "");
    strlist_free(&rootl);
    strlist_free(&refl);
    strlist_free(&cml);
    free(from); free(gd); free(gdarg); free(hooks); free(logs); free(cfg);
    return 0;
}

static int by_entry(const void *a, const void *b)
{
    return strcmp(((const entry *)a)->path, ((const entry *)b)->path);
}

void git_prepare(const char *src, const char *workdir, const char *since, entries *files, gitrepos *out)
{
    memset(out, 0, sizeof *out);
    strlist repos = { 0 }, heads = { 0 };      /* each repository, and its .git/HEAD on disk */
    int *bins = xmalloc((files->n + 1) * sizeof *bins);
    for (size_t i = 0; i < files->n; i++) {
        char *r = repo_of(files->v[i].path);
        if (r && !strlist_has(&repos, r)) {
            bins[repos.n] = files->v[i].bin;
            strlist_add(&repos, r);
            strlist_add(&heads, files->v[i].source);
        }
        free(r);
    }
    if (!repos.n) {
        free(bins);
        return;
    }
    out->v = xmalloc(repos.n * sizeof *out->v);
    int have_git = on_path("git");
    for (size_t k = 0; k < repos.n; k++) {
        gitrepo *g = &out->v[out->n++];
        memset(g, 0, sizeof *g);
        g->path = xstrdup(repos.v[k]);
        if (!have_git) {
            g->note = xprintf("git repository %s: .git copied as it is (git is not on PATH): hooks, reflogs and any "
                              "credentials in its config are on the disc", *g->path ? g->path : ".");
            continue;
        }
        /* the repository on disk: where its .git/HEAD is read from (src/PATH, or a disc plan's item) */
        char *abs = xstrdup(heads.v[k]), *dir = xprintf("%s/git-%zu", workdir, k);
        size_t al = strlen(abs);
        if (al >= 10 && !strcmp(abs + al - 10, "/.git/HEAD")) abs[al - 10] = 0;
        else {
            free(abs);
            if (!src) die("cannot tell where the git repository %s is on disk", *g->path ? g->path : ".");
            abs = *g->path ? join(src, g->path) : xstrdup(src);
        }
        sbuf tsv = { 0 };
        sb_puts(&tsv, "");
        fprintf(stderr, "Compacting the git history of %s ...\n", *g->path ? g->path : ".");
        if (compact(abs, dir, g->path, since, &tsv, &g->note)) die("could not make a compacted copy of %s's .git", abs);
        g->tsv = tsv.s;
        /* the payload: the compacted .git in place of the original */
        char *prefix = *g->path ? xprintf("%s/.git/", g->path) : xstrdup(".git/");
        size_t kept = 0;
        for (size_t i = 0; i < files->n; i++)
            if (strncmp(files->v[i].path, prefix, strlen(prefix))) files->v[kept++] = files->v[i];
        files->n = kept;
        entries add, noted;
        scan_payload(dir, "default", &add, &noted);
        files->v = xrealloc(files->v, (files->n + add.n + 1) * sizeof *files->v);
        for (size_t i = 0; i < add.n; i++) {
            entry e = add.v[i];
            char *p = *g->path ? xprintf("%s/%s", g->path, e.path) : xstrdup(e.path);
            e.path = p;
            e.bin = bins[k];
            files->v[files->n++] = e;
        }
        free(prefix);
        free(abs);
        free(dir);
    }
    qsort(files->v, files->n, sizeof *files->v, by_entry);
    strlist_free(&repos);
    strlist_free(&heads);
    free(bins);
}

/* the rows of git.tsv for the repositories whose .git/HEAD is on this disc, or NULL */
char *git_tsv_for(const gitrepos *g, const entries *disc_files)
{
    sbuf b = { 0 };
    for (size_t k = 0; k < g->n; k++) {
        if (!g->v[k].tsv) continue;
        char *head = *g->v[k].path ? xprintf("%s/.git/HEAD", g->v[k].path) : xstrdup(".git/HEAD");
        int here = 0;
        for (size_t i = 0; i < disc_files->n && !here; i++) here = !strcmp(disc_files->v[i].path, head);
        free(head);
        if (!here) continue;
        if (!b.s) sb_puts(&b, "# arv git 1\trepository (relative to data/)\tkind (root, head, shallow, commit)\tvalue\n");
        sb_puts(&b, g->v[k].tsv);
    }
    return b.s;
}

/* is this path inside a .git folder (or a .git file)? */
int git_internal(const char *path)
{
    return !strcmp(path, ".git") || !strncmp(path, ".git/", 5) || strstr(path, "/.git/") ||
           (strlen(path) > 5 && !strcmp(path + strlen(path) - 5, "/.git"));
}
