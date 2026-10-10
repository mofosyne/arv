/*
 * Local-LLM help with descriptive metadata (llm.py). The model gets a compact inventory of the
 * folder or disc (names, counts, sizes, dates, file types, short README-style text files) and
 * returns a suggested title, description, subjects and folder tags, plus questions only the owner
 * can answer. Any OpenAI-compatible chat server works: Ollama (http://127.0.0.1:11434/v1),
 * llama.cpp's llama-server, LM Studio, vLLM. Only loopback servers are used unless remote ones
 * are allowed explicitly, because the inventory describes private files.
 */
#define _XOPEN_SOURCE 700
#include "assist.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_TEXT_FILES 6
#define MAX_TEXT_CHARS 1500
#define MAX_INVENTORY_CHARS 9000

static const char SYSTEM_PROMPT[] =
    "You are an archivist helping someone document a personal archive disc "
    "(photos, video, source code, documents) so that they, or their family, can "
    "understand it decades from now.\n\n"
    "You receive an inventory of the disc: folder structure, file counts, sizes, "
    "dates, file types, and the text of a few README-like files. You never see the "
    "file contents otherwise.\n\n"
    "Rules:\n"
    "- Use the evidence: folder names, file names, dates and README text often say "
    "what something is (a place, an event, a project). Mention those specifics.\n"
    "- Only state what the inventory supports. Do not invent names, places, events "
    "or purposes. If something is unclear, ask about it instead of guessing.\n"
    "- Title: short (at most 60 characters), specific, human-friendly. Not the "
    "folder name, not a date alone, not \"Personal Archive\".\n"
    "- Description: 2-5 plain sentences on what the disc holds and how it is "
    "organised, naming the main contents, useful to someone who has never seen it.\n"
    "- Subjects: 3-8 short lowercase keywords for the whole disc.\n"
    "- Folder tags: for the folders listed in the inventory (use the exact folder "
    "path shown, without the trailing slash), 1-5 short lowercase tags each that "
    "describe what the folder holds, based only on names, types, dates and text "
    "shown, including any \"What sampled images show\" lines. Skip folders you "
    "cannot say anything useful about.\n"
    "- Questions: up to %d questions that only the owner can answer "
    "and that would most improve the description. Never ask something the "
    "inventory already answers (counts, dates, file names, README text). Ask about "
    "people, places, occasions, purpose, which items are the only copy or most "
    "important, and unclear names. Refer to concrete folder or file names. No "
    "yes/no questions.\n\n"
    "Example (for a different disc):\n"
    "{\"title\": \"Lisbon holiday and garden project, 2016-2017\",\n"
    " \"description\": \"Photos from a family holiday in Lisbon (June 2016) and "
    "design files for a raised-bed garden project. Photos are grouped by day; the "
    "garden folder holds sketches and a parts list.\",\n"
    " \"subjects\": [\"travel\", \"portugal\", \"family\", \"gardening\", \"diy\"],\n"
    " \"folder_tags\": {\"photos/2016-06_Lisbon\": [\"travel\", \"lisbon\", \"family\"],\n"
    "                 \"garden\": [\"diy\", \"gardening\", \"plans\"]},\n"
    " \"questions\": [\"Who is in the Lisbon photos?\",\n"
    "               \"What is in scan_03.tif in misc/, and why was it kept?\"]}\n\n"
    "Reply with a single JSON object with exactly these keys and nothing else:\n"
    "{\"title\": \"...\", \"description\": \"...\", \"subjects\": [\"...\"],\n"
    " \"folder_tags\": {\"folder/path\": [\"tag\", \"...\"]}, \"questions\": [\"...\"]}";

/* ------------------------------------------------------------------ the client */

int llm_open(llm_client *c, const char *url, const char *model, int allow_remote, char **err)
{
    memset(c, 0, sizeof *c);
    const char *u = url && *url ? url : getenv("ARCHIVE_LLM_URL") && *getenv("ARCHIVE_LLM_URL") ? getenv("ARCHIVE_LLM_URL") : LLM_DEFAULT_URL;
    c->url = xstrdup(u);
    for (size_t n = strlen(c->url); n && c->url[n - 1] == '/'; n--) c->url[n - 1] = 0;
    const char *m = model && *model ? model : getenv("ARCHIVE_LLM_MODEL");
    c->model = m && *m ? xstrdup(m) : NULL;
    c->timeout = 600;
    if (!allow_remote && !url_is_loopback(c->url)) {
        *err = xprintf("refusing to send the inventory to %s: not a local address "
                       "(use --llm-allow-remote for a server you trust)", c->url);
        return -1;
    }
    return 0;
}

jv *llm_call(llm_client *c, const char *path, const jv *body, char **err)
{
    char *url = xprintf("%s%s", c->url, path), *reply = NULL, *e = NULL;
    sbuf b = { 0 };
    if (body) json_dump(&b, body, -1);
    int status = 0;
    jv *out = NULL;
    if (http_request(url, body ? "POST" : "GET", b.s, c->timeout, &status, &reply, &e)) {
        *err = xprintf("cannot reach the LLM server at %s (%s). Is it running?", c->url, e);
    } else if (status != 200) {
        *err = xprintf("%d from %s: %.300s", status, url, reply ? reply : "");
    } else if (!(out = json_parse(reply))) {
        *err = xprintf("the server at %s did not answer with JSON: %.300s", c->url, reply);
    }
    free(e);
    free(reply);
    free(url);
    free(b.s);
    return out;
}

const char *llm_model(llm_client *c, char **err)
{
    if (c->model) return c->model;
    jv *r = llm_call(c, "/models", NULL, err);
    if (!r) return NULL;
    const jv *data = json_get(r, "data");
    const char *id = data && data->kind == 'a' && data->n ? jstr_of(json_get(data->vals[0], "id")) : NULL;
    if (id) c->model = xstrdup(id);
    else *err = xprintf("the server at %s lists no models; pass --llm-model", c->url);
    jfree(r);
    return c->model;
}

char *llm_chat(llm_client *c, jv *messages, const char *temperature, int json_mode, char **err)
{
    const char *model = llm_model(c, err);
    if (!model) return NULL;
    jv *body = jobj();
    jput(body, "model", jstr(model));
    jput(body, "messages", messages);
    jput(body, "temperature", jnum(temperature));
    jput(body, "stream", jnum("false"));
    if (json_mode) {
        jv *fmt = jobj();
        jput(fmt, "type", jstr("json_object"));
        jput(body, "response_format", fmt);
    }
    jv *r = llm_call(c, "/chat/completions", body, err);
    if (!r && json_mode && strstr(*err, "response_format")) {     /* older servers reject it */
        free(*err);
        *err = NULL;
        body->n--;                                          /* response_format is the last key */
        jfree(body->vals[body->n]);
        free(body->keys[body->n]);
        r = llm_call(c, "/chat/completions", body, err);
    }
    jfree(body);
    if (!r) return NULL;
    const jv *choices = json_get(r, "choices");
    const jv *msg = choices && choices->kind == 'a' && choices->n ? json_get(choices->vals[0], "message") : NULL;
    const char *content = jstr_of(json_get(msg, "content"));
    char *out = content ? xstrdup(content) : NULL;
    if (!out) {
        sbuf b = { 0 };
        json_dump(&b, r, -1);
        *err = xprintf("unexpected reply from the server: %.300s", b.s);
        free(b.s);
    }
    jfree(r);
    return out;
}

char *llm_agent(const llm_client *c)
{
    return xprintf("llm:%s", c->model ? c->model : "unknown");
}

/* ------------------------------------------------------------------ the inventory */

static void human(uint64_t n, char out[32])
{
    static const char *const unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)n;
    int u = 0;
    while (v >= 1024 && u < 4) { v /= 1024; u++; }
    if (!u) snprintf(out, 32, "%llu B", (unsigned long long)n);
    else snprintf(out, 32, "%.1f %s", v, unit[u]);
}

static const char *base_of(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* os.path.splitext's extension, lowercased ("" for none; a leading dot is not one) */
static void ext_of(const char *path, char out[32])
{
    const char *b = base_of(path), *dot = strrchr(b, '.');
    out[0] = 0;
    if (!dot || dot == b) return;
    const char *p = b;
    while (*p == '.') p++;
    if (dot < p) return;
    snprintf(out, 32, "%s", dot);
    for (char *q = out; *q; q++) *q = (char)tolower((unsigned char)*q);
}

/* README-like names: readme, notes, about, description, info, index, contents, changelog, todo */
static int text_name(const char *base)
{
    static const char *const names[] = { "readme", "notes", "note", "about", "description", "info", "index", "contents",
                                         "changelog", "todo", NULL };
    for (int i = 0; names[i]; i++) {
        size_t n = strlen(names[i]);
        if (!strncasecmp(base, names[i], n) && (!base[n] || base[n] == '.' || base[n] == '_' || base[n] == '-')) return 1;
    }
    return 0;
}

static int text_ext(const char *ext)
{
    static const char *const exts[] = { ".txt", ".md", ".rst", ".org", ".nfo", ".rec", NULL };
    for (int i = 0; exts[i]; i++)
        if (!strcmp(ext, exts[i])) return 1;
    return 0;
}

/* at most n bytes, cut at a UTF-8 character boundary */
static size_t utf8_cut(const char *s, size_t n)
{
    size_t len = strlen(s);
    if (len <= n) return len;
    while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

typedef struct {
    char *key;
    size_t n;
    uint64_t size;
    strlist samples, deeper;
} folder_row;

static const items *sort_items;
static int text_order(const void *a, const void *b)
{
    const item *x = &sort_items->v[*(const size_t *)a], *y = &sort_items->v[*(const size_t *)b];
    size_t dx = 0, dy = 0;
    for (const char *p = x->path; *p; p++) dx += *p == '/';
    for (const char *p = y->path; *p; p++) dy += *p == '/';
    if (dx != dy) return dx < dy ? -1 : 1;
    int nx = !text_name(base_of(x->path)), ny = !text_name(base_of(y->path));
    if (nx != ny) return nx - ny;
    return strcmp(x->path, y->path);
}

char *llm_inventory(const items *l, const char *name, const rec_record *existing, const char *text_root)
{
    sbuf b = { 0 };
    char hs[32];
    uint64_t total = 0;
    int y0 = 0, y1 = 0;
    for (size_t i = 0; i < l->n; i++) {
        total += l->v[i].size;
        if (l->v[i].mtime) {
            struct tm tm;
            time_t t = l->v[i].mtime;
            localtime_r(&t, &tm);
            int y = tm.tm_year + 1900;
            if (!y0 || y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    human(total, hs);
    sb_printf(&b, "Archive name: %s\nFiles: %zu, total %s\n", name, l->n, hs);
    if (y0) sb_printf(&b, "File dates (modification time): %d to %d\n", y0, y1);
    /* file types, most common first (ties in order of first appearance) */
    strlist exts = { 0 };
    size_t *counts = NULL;
    for (size_t i = 0; i < l->n; i++) {
        char e[32];
        ext_of(l->v[i].path, e);
        const char *k = *e ? e : "(none)";
        size_t j = 0;
        while (j < exts.n && strcmp(exts.v[j], k)) j++;
        if (j == exts.n) {
            strlist_add(&exts, k);
            counts = xrealloc(counts, exts.n * sizeof *counts);
            counts[j] = 0;
        }
        counts[j]++;
    }
    sb_puts(&b, "File types: ");
    for (int shown = 0; shown < 15; shown++) {
        size_t best = exts.n;
        for (size_t j = 0; j < exts.n; j++)
            if (counts[j] && (best == exts.n || counts[j] > counts[best])) best = j;
        if (best == exts.n) break;
        sb_printf(&b, "%s%s x%zu", shown ? ", " : "", exts.v[best], counts[best]);
        counts[best] = 0;
    }
    sb_puts(&b, "\n");
    strlist_free(&exts);
    free(counts);

    if (existing) {
        static const char *const keys[] = { "Title", "Description", "Subject", "Note", "Coverage", NULL };
        sb_puts(&b, "\nMetadata already recorded:\n");
        for (int k = 0; keys[k]; k++)
            for (size_t f = 0; f < existing->nfields; f++)
                if (!strcmp(existing->fields[f].name, keys[k]) && *existing->fields[f].value)
                    sb_printf(&b, "  %s: %s\n", keys[k], existing->fields[f].value);
    }

    sb_puts(&b, "\nFolders (files, size, sample names):\n");
    folder_row *rows = NULL;
    size_t nrows = 0;
    for (size_t i = 0; i < l->n; i++) {
        const char *p = l->v[i].path, *slash = strrchr(p, '/');
        char *parent = slash ? xprintf("%.*s", (int)(slash - p), p) : xstrdup(".");
        /* group deeper folders under depth 3: the key is the first three parts */
        const char *third = NULL;
        int k = 0;
        for (const char *q = parent; *q && !third; q++)
            if (*q == '/' && ++k == 3) third = q;
        char *key = third ? xprintf("%.*s", (int)(third - parent), parent) : xstrdup(parent);
        char *deeper = NULL;
        if (third) {
            const char *d = third + 1, *e = strchr(d, '/');
            deeper = e ? xprintf("%.*s", (int)(e - d), d) : xstrdup(d);
        }
        size_t j = 0;
        while (j < nrows && strcmp(rows[j].key, key)) j++;
        if (j == nrows) {
            rows = xrealloc(rows, (nrows + 1) * sizeof *rows);
            memset(&rows[nrows], 0, sizeof *rows);
            rows[nrows++].key = xstrdup(key);
        }
        rows[j].n++;
        rows[j].size += l->v[i].size;
        if (deeper && !strlist_has(&rows[j].deeper, deeper)) strlist_add(&rows[j].deeper, deeper);
        if (rows[j].samples.n < 4) strlist_add(&rows[j].samples, base_of(p));
        free(parent);
        free(key);
        free(deeper);
    }
    for (size_t j = 0; j < nrows; j++) {
        if (j < 80) {
            human(rows[j].size, hs);
            sb_printf(&b, "  %s/ - %zu file%s, %s", rows[j].key, rows[j].n, rows[j].n == 1 ? "" : "s", hs);
            if (rows[j].deeper.n) sb_printf(&b, " (+%zu subfolders)", rows[j].deeper.n);
            sb_puts(&b, ": ");
            for (size_t k = 0; k < rows[j].samples.n; k++) sb_printf(&b, "%s%s", k ? ", " : "", rows[j].samples.v[k]);
            sb_puts(&b, "\n");
        }
        free(rows[j].key);
        strlist_free(&rows[j].samples);
        strlist_free(&rows[j].deeper);
    }
    if (nrows > 80) sb_printf(&b, "  ... and %zu more folders\n", nrows - 80);
    free(rows);

    /* README-like text files, shallowest and best-named first */
    size_t *texts = NULL, nt = 0;
    for (size_t i = 0; i < l->n; i++) {
        char e[32];
        ext_of(l->v[i].path, e);
        if ((text_name(base_of(l->v[i].path)) || text_ext(e)) && l->v[i].size <= 256 * 1024) {
            texts = xrealloc(texts, (nt + 1) * sizeof *texts);
            texts[nt++] = i;
        }
    }
    sort_items = l;
    if (nt) qsort(texts, nt, sizeof *texts, text_order);
    for (size_t k = 0, shown = 0; text_root && k < nt && shown < MAX_TEXT_FILES; k++) {
        char *path = join(text_root, l->v[texts[k]].path), *text = read_text(path);
        free(path);
        if (!text) continue;
        const char *s = text, *e = text + strlen(text);
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) { free(text); continue; }
        size_t keep = utf8_cut(text, MAX_TEXT_CHARS), len = strlen(text);
        text[keep] = 0;
        s = text;
        while (*s && isspace((unsigned char)*s)) s++;
        e = text + strlen(text);
        while (e > s && isspace((unsigned char)e[-1])) e--;
        shown++;
        sb_printf(&b, "\n--- %s ---\n%.*s%s\n", l->v[texts[k]].path, (int)(e - s), s, len > MAX_TEXT_CHARS ? " [...]" : "");
        free(text);
    }
    free(texts);

    /* lines were each ended with \n; the inventory ends without one */
    while (b.len && b.s[b.len - 1] == '\n') b.s[--b.len] = 0;
    if (b.len > MAX_INVENTORY_CHARS) {
        b.s[utf8_cut(b.s, MAX_INVENTORY_CHARS)] = 0;
        b.len = strlen(b.s);
        sb_puts(&b, "\n[inventory truncated]");
    }
    return b.s ? b.s : xstrdup("");
}

/* every folder (and each one above it) holding files, as the inventory names them; "." too */
void llm_folders(const items *l, strlist *out)
{
    memset(out, 0, sizeof *out);
    strlist_add(out, ".");
    for (size_t i = 0; i < l->n; i++)
        for (const char *s = strchr(l->v[i].path, '/'); s; s = strchr(s + 1, '/')) {
            char *f = xprintf("%.*s", (int)(s - l->v[i].path), l->v[i].path);
            if (!strlist_has(out, f)) strlist_add(out, f);
            free(f);
        }
}

/* ------------------------------------------------------------------ suggestions */

/* whitespace collapsed and trimmed, at most limit bytes */
static char *clean_text(const char *s, size_t limit)
{
    sbuf b = { 0 };
    int space = 0;
    for (; s && *s; s++) {
        if (isspace((unsigned char)*s)) { space = b.len > 0; continue; }
        if (space) sb_puts(&b, " ");
        space = 0;
        sb_add(&b, s, 1);
    }
    if (!b.s) return xstrdup("");
    b.s[utf8_cut(b.s, limit)] = 0;
    return b.s;
}

static void lower_ascii(char *s)
{
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

/* a list (or a string split at , ; and line breaks), cleaned, without case-insensitive repeats */
static void clean_items(const jv *v, size_t limit, size_t count, int lowercase, strlist *out)
{
    strlist raw = { 0 };
    if (v && v->kind == 's') {
        char *copy = xstrdup(v->str);
        for (char *t = copy, *next; t; t = next) {
            next = strpbrk(t, ",;\n");
            if (next) *next++ = 0;
            strlist_add(&raw, t);
        }
        free(copy);
    } else if (v && v->kind == 'a') {
        for (size_t i = 0; i < v->n; i++)
            if (v->vals[i]->kind == 's' || v->vals[i]->kind == 't') strlist_add(&raw, v->vals[i]->str);
    }
    for (size_t i = 0; i < raw.n && out->n < count; i++) {
        char *t = clean_text(raw.v[i], limit);
        if (lowercase) lower_ascii(t);
        int dup = !*t;
        for (size_t j = 0; j < out->n && !dup; j++) dup = !strcasecmp(out->v[j], t);
        if (!dup) strlist_add(out, t);
        free(t);
    }
    strlist_free(&raw);
}

int llm_parse(const char *content, const strlist *folders, suggestion *out, char **err)
{
    memset(out, 0, sizeof *out);
    const char *s = content, *e;
    const char *fence = strstr(s, "```");
    char *body;
    if (fence && (e = strstr(fence + 3, "```"))) {
        const char *in = fence + 3;
        if (!strncmp(in, "json", 4)) in += 4;
        body = xprintf("%.*s", (int)(e - in), in);
    } else {
        body = xstrdup(s);
    }
    char *open = strchr(body, '{'), *close = strrchr(body, '}');
    if (!open || !close || close < open) {
        *err = xprintf("the model did not return JSON: %.200s", content);
        free(body);
        return -1;
    }
    close[1] = 0;
    jv *d = json_parse(open);
    free(body);
    if (!d || d->kind != 'o') {
        *err = xprintf("the model returned invalid JSON: %.200s", content);
        jfree(d);
        return -1;
    }
    out->title = clean_text(jstr_of(json_get(d, "title")), 80);
    out->description = clean_text(jstr_of(json_get(d, "description")), 1500);
    clean_items(json_get(d, "subjects"), 40, 10, 1, &out->subjects);
    clean_items(json_get(d, "questions"), 300, 8, 0, &out->questions);
    const jv *ft = json_get(d, "folder_tags");
    for (size_t i = 0; ft && ft->kind == 'o' && i < ft->n; i++) {
        char *folder = clean_text(ft->keys[i], 4096);
        char *f = folder;
        while (*f == '/') f++;
        size_t n = strlen(f);
        while (n && f[n - 1] == '/') f[--n] = 0;
        strlist tags = { 0 };
        if (!folders || strlist_has(folders, f)) clean_items(ft->vals[i], 30, 5, 1, &tags);
        if (tags.n) {
            strlist *t = ftags_get(&out->folders, f, 1);
            strlist_free(t);
            *t = tags;
        }
        free(folder);
    }
    jfree(d);
    return 0;
}

jv *suggestion_json(const suggestion *s, int with_questions)
{
    jv *o = jobj(), *ft = jobj();
    jput(o, "title", jstr(s->title ? s->title : ""));
    jput(o, "description", jstr(s->description ? s->description : ""));
    jput(o, "subjects", jstrings(&s->subjects));
    for (size_t i = 0; i < s->folders.n; i++)
        if (s->folders.tags[i].n) jput(ft, s->folders.folder[i], jstrings(&s->folders.tags[i]));
    jput(o, "folder_tags", ft);
    if (with_questions) jput(o, "questions", jstrings(&s->questions));
    int caps = 0;
    for (size_t i = 0; i < s->folders.n; i++) caps |= s->folders.caption[i] != NULL;
    if (caps) {
        jv *c = jobj();
        for (size_t i = 0; i < s->folders.n; i++)
            if (s->folders.caption[i]) jput(c, s->folders.folder[i], jstr(s->folders.caption[i]));
        jput(o, "folder_captions", c);
    }
    return o;
}

void suggestion_free(suggestion *s)
{
    free(s->title);
    free(s->description);
    strlist_free(&s->subjects);
    strlist_free(&s->questions);
    ftags_free(&s->folders);
    memset(s, 0, sizeof *s);
}

static jv *message(const char *role, const char *content)
{
    jv *m = jobj();
    jput(m, "role", jstr(role));
    jput(m, "content", jstr(content));
    return m;
}

/* one round; answers: question, answer, question, answer ... from earlier rounds */
int llm_suggest(llm_client *c, const char *inventory, const strlist *answers, const suggestion *previous,
                int max_questions, const strlist *folders, suggestion *out, char **err)
{
    jv *messages = jarr();
    char *system = xprintf(SYSTEM_PROMPT, max_questions), *user = xprintf("Inventory:\n\n%s", inventory);
    jpush(messages, message("system", system));
    jpush(messages, message("user", user));
    free(system);
    free(user);
    if (previous) {
        sbuf b = { 0 };
        jv *p = suggestion_json(previous, 1);
        json_dump(&b, p, -1);
        jpush(messages, message("assistant", b.s));
        jfree(p);
        free(b.s);
    }
    if (answers && answers->n) {
        sbuf b = { 0 };
        sb_puts(&b, "The owner answered your questions:\n\n");
        for (size_t i = 0; i + 1 < answers->n; i += 2)
            sb_printf(&b, "%sQ: %s\nA: %s", i ? "\n\n" : "", answers->v[i], answers->v[i + 1]);
        sb_puts(&b, "\n\nRevise the title, description, subjects and folder tags using these answers (the answers are "
                    "authoritative). Ask only new questions that are still worth asking, or none.");
        jpush(messages, message("user", b.s));
        free(b.s);
    }
    char *content = llm_chat(c, messages, "0.2", 1, err);
    if (!content) return -1;
    int rc = llm_parse(content, folders, out, err);
    free(content);
    return rc;
}
