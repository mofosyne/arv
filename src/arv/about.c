/* What a data object says about itself (research/plan.md, "Data objects describe themselves"):
 * read by rules from its own files, no model. At the object's root: the README's first heading
 * and first paragraph of prose; a package manifest's name, one-line description and licence
 * (package.json, Cargo.toml, pyproject.toml, Arduino's library.properties); the licence a
 * LICENSE or COPYING file is, from its first lines; and for a git repository, its first and last
 * commit dates (Coverage: author dates on its branches, tags and HEAD) and where it was published
 * (Origin: the origin remote as host/path).
 * No author names or emails: the catalogue goes on every disc. */
#define _XOPEN_SOURCE 700
#include "arv.h"

#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_READ (64 * 1024)            /* of a README or manifest: the top is what describes it */
#define MAX_DESCRIPTION 300

static void set(char **field, const char *value)
{
    if (*field || !value || !*value) return;      /* the first source to give it wins */
    *field = xstrdup(value);
}

/* the first file at dir's top whose lowercase name is one of names, in their order; NULL */
static char *find_file(const char *dir, const char *const *names)
{
    DIR *d = opendir(dir);
    if (!d) return NULL;
    strlist here = { 0 };
    for (struct dirent *e; (e = readdir(d));) strlist_add(&here, e->d_name);
    closedir(d);
    char *out = NULL;
    for (int k = 0; names[k] && !out; k++)
        for (size_t i = 0; i < here.n && !out; i++)
            if (!strcasecmp(here.v[i], names[k])) {
                char *p = join(dir, here.v[i]);
                struct stat st;
                if (!stat(p, &st) && S_ISREG(st.st_mode)) out = p;
                else free(p);
            }
    strlist_free(&here);
    return out;
}

static char *read_top(const char *path)
{
    char *t = path ? read_text(path) : NULL;
    if (t && strlen(t) > MAX_READ) t[MAX_READ] = 0;
    return t;
}

static char *trimmed(const char *s, size_t n)
{
    while (n && isspace((unsigned char)*s)) s++, n--;
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xprintf("%.*s", (int)n, s);
}

/* ------------------------------------------------------------------ Markdown, reStructuredText, plain text */

/* inline markup off: images and HTML tags gone, links to their text, emphasis and code marks
 * dropped, spaces collapsed */
static char *plain(const char *s)
{
    const char *start = s;
    sbuf b = { 0 };
    sb_puts(&b, "");
    while (*s) {
        if (s[0] == '!' && s[1] == '[') {                       /* ![alt](url): an image */
            const char *close = strstr(s, "](");
            const char *end = close ? strchr(close, ')') : NULL;
            if (end) { s = end + 1; continue; }
        }
        if (*s == '[') {                                        /* [text](url) or [text][ref]: the text */
            const char *close = strchr(s, ']');
            if (close && (close[1] == '(' || close[1] == '[')) {
                const char *end = strchr(close + 2, close[1] == '(' ? ')' : ']');
                if (end) {
                    char *inner = xprintf("%.*s", (int)(close - s - 1), s + 1), *p = plain(inner);
                    sb_puts(&b, p);
                    free(inner);
                    free(p);
                    s = end + 1;
                    continue;
                }
            }
        }
        if (*s == '<') {                                        /* <tag ...>, </tag>, <https://...> */
            const char *end = strchr(s, '>');
            if (end && (isalpha((unsigned char)s[1]) || s[1] == '/' || s[1] == '!')) { s = end + 1; continue; }
        }
        if (*s == '`' || (*s == '*' && (s[1] == '*' || s == start || !isalnum((unsigned char)s[-1]) || !isalnum((unsigned char)s[1])))
            || (s[0] == '_' && s[1] == '_')) {
            s += s[0] == s[1] && *s != '`' ? 2 : 1;
            continue;
        }
        if (isspace((unsigned char)*s)) {
            if (b.len && b.s[b.len - 1] != ' ') sb_puts(&b, " ");
            s++;
            continue;
        }
        sb_add(&b, s, 1);
        s++;
    }
    char *out = trimmed(b.s, b.len);
    free(b.s);
    return out;
}

static int underline(const char *l)             /* "=====" or "-----" (setext, or rst) */
{
    char c = *l;
    if (c != '=' && c != '-' && c != '~' && c != '*' && c != '#') return 0;
    size_t n = 0;
    while (l[n] == c) n++;
    while (l[n] == ' ' || l[n] == '\t') n++;
    return n >= 3 && !l[n];
}

/* not prose: a badge row, an image, HTML, a quote or callout, a table, a list of links, a
 * directive, a horizontal rule */
static int skipped_line(const char *l)
{
    while (*l == ' ') l++;
    if (*l == '<' || *l == '>' || *l == '|' || !strncmp(l, "![", 2) || !strncmp(l, "[!", 2) || !strncmp(l, "[![", 3)
        || !strncmp(l, "..", 2) || !strncmp(l, "#+", 2) || underline(l))
        return 1;
    if (*l == '[') {                            /* [ref]: url */
        const char *c = strchr(l, ']');
        if (c && c[1] == ':') return 1;
    }
    char *p = plain(l);                         /* nothing left once the markup is off */
    int letters = 0;
    for (const char *q = p; *q; q++) letters += isalpha((unsigned char)*q) != 0;
    free(p);
    return letters < 2;
}

/* cut at the last sentence end within the limit, else at a word, with an ellipsis */
static char *cut(const char *s)
{
    if (utf8_chars(s) <= MAX_DESCRIPTION) return xstrdup(s);
    size_t n = 0;
    const char *p = s;
    for (size_t chars = 0; *p && chars < MAX_DESCRIPTION; chars++) utf8_next(&p);
    n = (size_t)(p - s);
    for (size_t i = n; i > MAX_DESCRIPTION / 3; i--)
        if ((s[i - 1] == '.' || s[i - 1] == '!' || s[i - 1] == '?') && s[i] == ' ') return xprintf("%.*s", (int)i, s);
    while (n && s[n] != ' ') n--;
    return xprintf("%.*s...", (int)n, s);
}

/* the README's title (its first heading) and its first paragraph of prose */
static void read_readme(const char *path, char **title, char **description)
{
    char *text = read_top(path);
    if (!text) return;
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path, *dot = strrchr(base, '.');
    int md = dot && (!strcasecmp(dot, ".md") || !strcasecmp(dot, ".markdown"));
    int marked = md || (dot && (!strcasecmp(dot, ".rst") || !strcasecmp(dot, ".org")));
    strlist lines = { 0 };
    for (char *l = text; *l;) {
        char *nl = strchr(l, '\n');
        size_t n = nl ? (size_t)(nl - l) : strlen(l);
        while (n && (l[n - 1] == '\r' || l[n - 1] == ' ' || l[n - 1] == '\t')) n--;
        char *line = xprintf("%.*s", (int)n, l);
        strlist_add(&lines, line);
        free(line);
        l = nl ? nl + 1 : l + strlen(l);
    }
    free(text);
    char *head = NULL;
    sbuf para = { 0 };
    int fence = 0, after_title = 0;
    for (size_t i = 0; i < lines.n; i++) {
        const char *l = lines.v[i], *next = i + 1 < lines.n ? lines.v[i + 1] : "";
        if (!strncmp(l, "```", 3) || !strncmp(l, "~~~", 3)) { fence = !fence; continue; }
        if (fence) continue;
        int heading = 0;
        char *h = NULL;
        if (*l == '#' && strncmp(l, "#+", 2) && strncmp(l, "#!", 2)) {             /* # Title, ## Title */
            const char *s = l;
            while (*s == '#') s++;
            if (*s == ' ' || !*s) {
                size_t n = strlen(s);
                while (n && (s[n - 1] == '#' || s[n - 1] == ' ')) n--;
                h = xprintf("%.*s", (int)n, s);
                heading = 1;
            }
        } else if (!strncasecmp(l, "#+title:", 8)) {                                  /* Org */
            h = xstrdup(l + 8);
            heading = 1;
        } else if (*l && !skipped_line(l) && underline(next) && strlen(next) >= strlen(l) / 2) {   /* Title\n===== */
            h = xstrdup(l);
            heading = 1;
            i++;
        } else if (!md && underline(l) && *next && !underline(next) && i + 2 < lines.n && underline(lines.v[i + 2])) {
            h = xstrdup(next);                                                        /* rst: ====\nTitle\n==== */
            heading = 1;
            i += 2;
        }
        if (heading) {
            if (para.len) break;                /* the paragraph before the next heading */
            char *p = plain(h);
            if (!head && *p) head = p, after_title = 1;
            else free(p);
            free(h);
            continue;
        }
        if (!*l) {
            if (para.len) break;
            continue;
        }
        if (skipped_line(l)) {
            if (para.len) break;
            continue;
        }
        if (!marked && !head && !para.len && !after_title && utf8_chars(l) <= 80 && !*next) {   /* plain text: a short first line */
            char *p = plain(l);
            head = p;
            after_title = 1;
            continue;
        }
        if (para.len) sb_puts(&para, " ");
        sb_puts(&para, l);
    }
    if (head) set(title, head);
    if (para.len) {
        char *p = plain(para.s), *c = cut(p);
        size_t words = 0;
        for (const char *q = c; *q; q++) words += *q == ' ';
        if (words >= 3) set(description, c);    /* a sentence, not a stray word */
        free(p);
        free(c);
    }
    free(head);
    free(para.s);
    strlist_free(&lines);
}

/* ------------------------------------------------------------------ package manifests */

/* KEY = "value" in one of the [sections] of a TOML file (single-line strings; license = {text = "..."}) */
static char *toml_get(const char *text, const char *const *sections, const char *key)
{
    int in = 0;
    for (const char *l = text; *l;) {
        const char *nl = strchr(l, '\n');
        size_t n = nl ? (size_t)(nl - l) : strlen(l);
        char *line = trimmed(l, n);
        l = nl ? nl + 1 : l + n;
        if (*line == '[') {
            in = 0;
            for (int k = 0; sections[k]; k++) in |= !strcmp(line, sections[k]);
        } else if (in && !strncmp(line, key, strlen(key))) {
            const char *s = line + strlen(key);
            while (*s == ' ') s++;
            if (*s == '=') {
                s++;
                while (*s == ' ') s++;
                if (*s == '{') {                        /* license = { text = "MIT" } */
                    const char *t = strstr(s, "text");
                    if (!t) t = strstr(s, "file");
                    s = t ? strchr(t, '"') : NULL;
                }
                if (s && (*s == '"' || *s == '\'') && strncmp(s, "\"\"\"", 3)) {
                    const char *e = strchr(s + 1, *s);
                    if (e) {
                        char *out = xprintf("%.*s", (int)(e - s - 1), s + 1);
                        free(line);
                        return out;
                    }
                }
            }
        }
        free(line);
    }
    return NULL;
}

/* KEY=value lines (Arduino's library.properties) */
static char *prop_get(const char *text, const char *key)
{
    size_t k = strlen(key);
    for (const char *l = text; *l;) {
        const char *nl = strchr(l, '\n');
        size_t n = nl ? (size_t)(nl - l) : strlen(l);
        if (n > k && !strncmp(l, key, k) && l[k] == '=') return trimmed(l + k + 1, n - k - 1);
        l = nl ? nl + 1 : l + n;
    }
    return NULL;
}

static void read_manifests(const char *dir, char **name, char **description, char **licence)
{
    char *path = join(dir, "package.json"), *text = read_top(path);
    if (text) {
        jv *doc = json_parse(text);
        const jv *n = doc ? json_get(doc, "name") : NULL, *d = doc ? json_get(doc, "description") : NULL,
                 *l = doc ? json_get(doc, "license") : NULL;
        if (n && n->kind == 's') set(name, n->str);
        if (d && d->kind == 's') set(description, d->str);
        if (l && l->kind == 's') set(licence, l->str);
        jfree(doc);
    }
    free(path);
    free(text);
    static const char *const cargo[] = { "[package]", NULL }, *const py[] = { "[project]", "[tool.poetry]", NULL };
    const char *const *sections[] = { cargo, py };
    const char *files[] = { "Cargo.toml", "pyproject.toml" };
    for (int f = 0; f < 2; f++) {
        path = join(dir, files[f]);
        text = read_top(path);
        if (text) {
            char *v;
            if ((v = toml_get(text, sections[f], "name"))) { set(name, v); free(v); }
            if ((v = toml_get(text, sections[f], "description"))) { set(description, v); free(v); }
            if ((v = toml_get(text, sections[f], "license"))) { set(licence, v); free(v); }
        }
        free(path);
        free(text);
    }
    path = join(dir, "library.properties");
    text = read_top(path);
    if (text) {
        char *v;
        if ((v = prop_get(text, "name"))) { set(name, v); free(v); }
        if ((v = prop_get(text, "sentence"))) { set(description, v); free(v); }
        if ((v = prop_get(text, "license"))) { set(licence, v); free(v); }
    }
    free(path);
    free(text);
}

/* ------------------------------------------------------------------ the licence file */

/* the licence a LICENSE or COPYING file is, as an SPDX id, from its first lines; NULL */
static const char *licence_of(const char *dir)
{
    static const char *const names[] = { "LICENSE", "LICENSE.md", "LICENSE.txt", "LICENCE", "LICENCE.md", "LICENCE.txt",
                                         "COPYING", "COPYING.md", "COPYING.txt", NULL };
    char *path = find_file(dir, names), *text = read_top(path);
    free(path);
    if (!text) return NULL;
    if (strlen(text) > 4096) text[4096] = 0;
    sbuf b = { 0 };                             /* lowercase, one space between words: lines wrap anywhere */
    sb_puts(&b, " ");
    for (const char *s = text; *s; s++) {
        if (isspace((unsigned char)*s)) {
            if (b.s[b.len - 1] != ' ') sb_puts(&b, " ");
        } else {
            char c = (char)tolower((unsigned char)*s);
            sb_add(&b, &c, 1);
        }
    }
    free(text);
    const char *t = b.s, *id = NULL;
#define HAS(s) (strstr(t, s) != NULL)
    if (HAS("gnu affero general public license")) id = "AGPL-3.0";
    else if (HAS("gnu lesser general public license")) id = HAS("version 2.1") ? "LGPL-2.1" : HAS("version 3") ? "LGPL-3.0" : "LGPL";
    else if (HAS("gnu library general public license")) id = "LGPL-2.0";
    else if (HAS("gnu general public license")) id = HAS("version 3") ? "GPL-3.0" : HAS("version 2") ? "GPL-2.0" : "GPL";
    else if (HAS("apache license") && HAS("version 2.0")) id = "Apache-2.0";
    else if (HAS("mozilla public license") && HAS("2.0")) id = "MPL-2.0";
    else if (HAS("eclipse public license") && HAS("2.0")) id = "EPL-2.0";
    else if (HAS("boost software license")) id = "BSL-1.0";
    else if (HAS("free and unencumbered software released into the public domain")) id = "Unlicense";
    else if (HAS("cc0 1.0 universal")) id = "CC0-1.0";
    else if (HAS("mit license") || HAS("permission is hereby granted, free of charge")) id = "MIT";
    else if (HAS("isc license") || HAS("permission to use, copy, modify, and/or distribute this software for any purpose")) id = "ISC";
    else if (HAS("redistribution and use in source and binary forms"))
        id = HAS("neither the name") || HAS("the names of its contributors") ? "BSD-3-Clause" : "BSD-2-Clause";
#undef HAS
    free(b.s);
    return id;
}

/* ------------------------------------------------------------------ git */

/* a remote's URL as host/path: no scheme, user, port or .git; NULL for a local path */
char *about_origin(const char *url)
{
    const char *s = url, *scheme = strstr(url, "://");
    if (!strncmp(url, "file:", 5) || *url == '/' || *url == '.' || *url == '~') return NULL;
    sbuf b = { 0 };
    if (scheme) {
        s = scheme + 3;
        const char *at = strchr(s, '@'), *slash = strchr(s, '/');
        if (at && (!slash || at < slash)) s = at + 1;
        slash = strchr(s, '/');
        const char *colon = strchr(s, ':');
        if (colon && (!slash || colon < slash)) {               /* host:port/path */
            sb_add(&b, s, (size_t)(colon - s));
            s = slash ? slash : s + strlen(s);
        }
    } else {                                                    /* scp-like: user@host:path */
        const char *colon = strchr(url, ':'), *slash = strchr(url, '/');
        if (!colon || (slash && slash < colon) || colon - url < 2) return NULL;   /* C:\... is a drive */
        const char *at = strchr(url, '@');
        if (at && at < colon) s = at + 1;
        sb_add(&b, s, (size_t)(colon - s));
        s = colon + 1;
        if (*s != '/') sb_puts(&b, "/");
    }
    sb_puts(&b, s);
    if (!b.s) return NULL;
    while (b.len && b.s[b.len - 1] == '/') b.s[--b.len] = 0;
    if (b.len > 4 && !strcmp(b.s + b.len - 4, ".git")) b.s[b.len -= 4] = 0;
    if (!strchr(b.s, '.') && !strchr(b.s, '/')) {               /* a bare word is not a place */
        free(b.s);
        return NULL;
    }
    return b.s;
}

static void read_git(const char *dir, about *a)
{
    if (!on_path("git")) return;
    char *gd = join(dir, ".git"), *gdarg = xprintf("--git-dir=%s", gd), *out = NULL;
    /* its history: branches, tags and HEAD (not the stash, whose commits are dated when stashed) */
    char *log[] = { "git", gdarg, "log", "--branches", "--tags", "--remotes", "HEAD", "--date=short", "--format=%ad", NULL };
    if (!run(log, &out) && out) {
        char first[11] = "", last[11] = "";
        for (char *l = out; *l;) {
            char *nl = strchr(l, '\n');
            if (nl) *nl = 0;
            if (strlen(l) == 10) {
                if (!*first || strcmp(l, first) < 0) memcpy(first, l, 11);
                if (strcmp(l, last) > 0) memcpy(last, l, 11);
            }
            l = nl ? nl + 1 : l + strlen(l);
        }
        if (*first) {
            char *c = strcmp(first, last) ? xprintf("%s/%s", first, last) : xstrdup(first);
            set(&a->coverage, c);
            free(c);
        }
    }
    free(out);
    out = NULL;
    char *origin[] = { "git", gdarg, "config", "--get", "remote.origin.url", NULL };
    char *any[] = { "git", gdarg, "config", "--get-regexp", "^remote\\..*\\.url$", NULL };
    const char *url = NULL;
    if (!run(origin, &out) && out && *out) url = out;
    else {
        free(out);
        out = NULL;
        if (!run(any, &out) && out && strchr(out, ' ')) url = strchr(out, ' ') + 1;
    }
    if (url) {
        char *u = xprintf("%.*s", (int)strcspn(url, "\n"), url), *o = about_origin(u);
        set(&a->origin, o);
        free(u);
        free(o);
    }
    free(out);
    free(gd);
    free(gdarg);
}

/* ------------------------------------------------------------------ the whole of it */

void about_read(const char *dir, int git, about *a)
{
    memset(a, 0, sizeof *a);
    static const char *const readmes[] = { "README.md", "README.markdown", "README.rst", "README.org", "README.txt",
                                           "README", "READ.ME", "README.1ST", NULL };
    char *readme = find_file(dir, readmes), *name = NULL, *desc = NULL, *licence = NULL, *rtitle = NULL, *rdesc = NULL;
    read_manifests(dir, &name, &desc, &licence);
    if (readme) read_readme(readme, &rtitle, &rdesc);
    set(&a->title, rtitle);
    set(&a->title, name);
    set(&a->description, desc);
    set(&a->description, rdesc);
    set(&a->rights, licence);
    set(&a->rights, licence_of(dir));
    if (git) read_git(dir, a);
    free(readme);
    free(name);
    free(desc);
    free(licence);
    free(rtitle);
    free(rdesc);
}

int about_any(const about *a)
{
    return a->title || a->description || a->coverage || a->rights || a->origin;
}

void about_free(about *a)
{
    free(a->title);
    free(a->description);
    free(a->coverage);
    free(a->rights);
    free(a->origin);
    memset(a, 0, sizeof *a);
}

/* the fields about describes, in the order an Object record has them (after Date) */
const char *const ABOUT_FIELDS[] = { "Title", "Description", "Coverage", "Rights", "Origin", NULL };

void about_fields(const about *a, const char *values[5])
{
    values[0] = a->title;
    values[1] = a->description;
    values[2] = a->coverage;
    values[3] = a->rights;
    values[4] = a->origin;
}
