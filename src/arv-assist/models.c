/*
 * Which model arv-assist asks, and what it can find (arv gui's Settings, arv-assist llm-status --all).
 *
 * A model server on this computer is used without asking: what it is asked stays here. An
 * external inference provider (bring your own key) is used only when the person chooses it, and
 * each use says so, since it sees what it is asked: names, dates and words from the files.
 * Pictures never go to one. Keys stay in the environment, read when a request is made: arv never
 * writes one down, and never sends one over plain http to another computer.
 *
 *   arv-assist llm-status --all [--json]   the servers here, the providers whose keys are set, and the choice
 *   arv-assist llm-use [--on|--off] [--url URL [--model NAME] [--external]] [--pictures URL [--pictures-model NAME]]
 *                      [--forget describing|pictures] [--clear]   the choice, kept in $XDG_CONFIG_HOME/arv/models.rec
 *   arv-assist llm-test --url URL [--model NAME] [--external] [--pictures]   a fixed question, nothing
 *                      from any file: does it answer, how fast, and in JSON (or, for pictures, about a red square)
 *
 * llm-use has options of its own: arv gui adds the --llm-... options it was started with to every
 * arv-assist command, and they are ignored here.
 */
#define _XOPEN_SOURCE 700
#include "assist.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* model servers that run on this computer, at their usual ports */
static const char *const LOCAL[][2] = {
    { "Ollama", "http://127.0.0.1:11434/v1" },
    { "llama.cpp (llama-server)", "http://127.0.0.1:8080/v1" },
    { "LM Studio", "http://127.0.0.1:1234/v1" },
    { "vLLM", "http://127.0.0.1:8000/v1" },
    { "Jan", "http://127.0.0.1:1337/v1" },
};

/* external providers with an OpenAI-compatible endpoint: name, address, the variable holding the key */
static const char *const PROVIDERS[][3] = {
    { "OpenAI", "https://api.openai.com/v1", "OPENAI_API_KEY" },
    { "Anthropic", "https://api.anthropic.com/v1", "ANTHROPIC_API_KEY" },
    { "Google Gemini", "https://generativelanguage.googleapis.com/v1beta/openai", "GEMINI_API_KEY" },
    { "Mistral", "https://api.mistral.ai/v1", "MISTRAL_API_KEY" },
    { "OpenRouter", "https://openrouter.ai/api/v1", "OPENROUTER_API_KEY" },
    { "Groq", "https://api.groq.com/openai/v1", "GROQ_API_KEY" },
    { "DeepSeek", "https://api.deepseek.com/v1", "DEEPSEEK_API_KEY" },
};
#define NPROVIDERS (sizeof PROVIDERS / sizeof *PROVIDERS)

static const char *env(const char *name)
{
    const char *v = getenv(name);
    return v && *v ? v : NULL;
}

/* OpenAI's address, or OPENAI_BASE_URL when set (a compatible service of one's own) */
static const char *provider_url(size_t i)
{
    return !strcmp(PROVIDERS[i][2], "OPENAI_API_KEY") && env("OPENAI_BASE_URL") ? env("OPENAI_BASE_URL") : PROVIDERS[i][1];
}

/* ------------------------------------------------------------------ the settings file */

char *models_settings_path(void)
{
    const char *x = env("XDG_CONFIG_HOME"), *home = env("HOME");
    if (x) return xprintf("%s/arv/models.rec", x);
    return xprintf("%s/.config/arv/models.rec", home ? home : ".");
}

static rec_file settings;
static int loaded;

static const rec_record *settings_record(void)
{
    if (!loaded) {
        loaded = 1;
        char *p = models_settings_path();
        int bad = 0;
        if (rec_read(p, &settings, &bad)) memset(&settings, 0, sizeof settings);
        free(p);
    }
    for (size_t i = 0; i < settings.nrecords; i++)
        if (!settings.records[i].descriptor) return &settings.records[i];
    return NULL;
}

const char *models_setting(const char *field)
{
    const rec_record *r = settings_record();
    const char *v = r ? rec_get(r, field) : NULL;
    return v && *v ? v : NULL;
}

/* ------------------------------------------------------------------ keys and the notice */

char *models_key(const char *url)
{
    char host[256];
    if (url_host(url, host, sizeof host)) return NULL;
    /* a key goes over https, or to this computer (llama-server --api-key); never in clear text elsewhere */
    if (strncmp(url, "https://", 8) && !url_is_loopback(url)) return NULL;
    if (env("ARCHIVE_LLM_KEY")) return xstrdup(env("ARCHIVE_LLM_KEY"));
    for (size_t i = 0; i < NPROVIDERS; i++) {
        char h[256];
        if (!url_host(provider_url(i), h, sizeof h) && !strcasecmp(h, host) && env(PROVIDERS[i][2]))
            return xstrdup(env(PROVIDERS[i][2]));
    }
    return NULL;
}

void models_say_external(const char *url)
{
    static int said;
    char host[256];
    if (said++ || url_host(url, host, sizeof host)) return;
    fprintf(stderr, "Using %s, an external inference provider: what is asked (names, dates, words from your "
                    "files) is sent there. Pictures are never sent.\n", host);
}

/* ------------------------------------------------------------------ llm-status --all */

static jv *jint(long n)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%ld", n);
    return jnum(buf);
}

static jv *jbool(int b)
{
    return jnum(b ? "true" : "false");
}

/* GET or POST to a server's own API beside /v1 (Ollama's /api/show, llama.cpp's /props), quickly */
static jv *ask_root(const char *v1, const char *path, const jv *body, const char *key)
{
    char *root = xstrdup(v1);
    size_t n = strlen(root);
    if (n >= 3 && !strcmp(root + n - 3, "/v1")) root[n - 3] = 0;
    char *url = xprintf("%s%s", root, path), *reply = NULL, *err = NULL;
    sbuf b = { 0 };
    if (body) json_dump(&b, body, -1);
    int status = 0;
    jv *out = NULL;
    if (!http_request(url, body ? "POST" : "GET", b.s, key, 3, &status, &reply, &err) && status == 200) out = json_parse(reply);
    free(root);
    free(url);
    free(reply);
    free(err);
    free(b.s);
    return out;
}

static long number_of(const jv *v)
{
    return v && v->kind == 't' && v->str ? atol(v->str) : 0;
}

/* a model's context length from Ollama's model_info ("llama.context_length": 131072) */
static long context_of(const jv *info)
{
    for (size_t i = 0; info && info->kind == 'o' && i < info->n; i++) {
        size_t k = strlen(info->keys[i]);
        if (k >= 15 && !strcmp(info->keys[i] + k - 15, ".context_length")) return number_of(info->vals[i]);
    }
    return 0;
}

/* what a server here offers: its models, with what each can do where the server says */
static jv *probe(const char *name, const char *url)
{
    jv *s = jobj();
    jput(s, "name", jstr(name));
    jput(s, "url", jstr(url));
    char *key = models_key(url), *reply = NULL, *err = NULL, *u = xprintf("%s/models", url);
    int status = 0;
    jv *list = NULL;
    if (!http_request(u, "GET", NULL, key, 3, &status, &reply, &err) && status == 200) list = json_parse(reply);
    free(u);
    free(reply);
    const jv *data = json_get(list, "data");
    jput(s, "found", jbool(data && data->kind == 'a'));
    jput(s, "ready", jbool(data && data->kind == 'a' && data->n));
    if (!data || data->kind != 'a') {
        jput(s, "error", jstr(err ? err : status ? "not a model server" : "no answer"));
        free(err);
        free(key);
        jfree(list);
        return s;
    }
    free(err);
    jv *props = ask_root(url, "/props", NULL, key);                /* llama.cpp: one model, its context */
    const jv *dgs = json_get(props, "default_generation_settings"), *mod = json_get(props, "modalities");
    long llama_ctx = number_of(json_get(dgs, "n_ctx"));
    if (!llama_ctx) llama_ctx = number_of(json_get(props, "n_ctx"));
    int llama_vision = mod && json_get(mod, "vision") && json_get(mod, "vision")->str && !strcmp(json_get(mod, "vision")->str, "true");
    jv *lms = ask_root(url, "/api/v0/models", NULL, key);           /* LM Studio: type and context */
    jv *models = jarr();
    int ollama = 1;
    for (size_t i = 0; i < data->n && i < 40; i++) {
        const char *id = jstr_of(json_get(data->vals[i], "id"));
        if (!id) continue;
        jv *m = jobj();
        jput(m, "id", jstr(id));
        int vision = llama_vision, embedding = strstr(id, "embed") != NULL;
        long ctx = llama_ctx;
        if (ollama) {                                               /* Ollama: capabilities, context */
            jv *q = jobj();
            jput(q, "model", jstr(id));
            jv *show = ask_root(url, "/api/show", q, key);
            jfree(q);
            const jv *caps = json_get(show, "capabilities");
            if (!show) ollama = 0;
            for (size_t c = 0; caps && caps->kind == 'a' && c < caps->n; c++) {
                const char *cap = jstr_of(caps->vals[c]);
                if (cap && !strcmp(cap, "vision")) vision = 1;
                if (cap && !strcmp(cap, "embedding")) embedding = 1;
            }
            if (context_of(json_get(show, "model_info"))) ctx = context_of(json_get(show, "model_info"));
            jfree(show);
        }
        const jv *ld = json_get(lms, "data");
        for (size_t k = 0; ld && ld->kind == 'a' && k < ld->n; k++) {
            const char *lid = jstr_of(json_get(ld->vals[k], "id")), *type = jstr_of(json_get(ld->vals[k], "type"));
            if (!lid || strcmp(lid, id)) continue;
            if (type && !strcmp(type, "vlm")) vision = 1;
            if (type && !strcmp(type, "embeddings")) embedding = 1;
            if (number_of(json_get(ld->vals[k], "max_context_length"))) ctx = number_of(json_get(ld->vals[k], "max_context_length"));
        }
        jput(m, "vision", jbool(vision));
        jput(m, "embedding", jbool(embedding));
        if (ctx) jput(m, "context", jint(ctx));
        jpush(models, m);
    }
    jput(s, "models", models);
    jfree(props);
    jfree(lms);
    jfree(list);
    free(key);
    return s;
}

/* the model used for describing, or for pictures: where it was chosen and whether it answers */
typedef struct {
    const char *url, *model, *vision_url, *vision_model;
    int remote;
} given;            /* the --llm-... options (arv gui passes on those it was started with) */

static jv *chosen(int vision, const given *g)
{
    jv *d = jobj();
    const char *url = vision ? g->vision_url : g->url, *model = vision ? g->vision_model : g->model, *from = "option";
    if (vision && !url) {
        url = models_setting("VisionUrl");
        if (!model) model = models_setting("VisionModel");
        from = "settings";
    }
    if (!vision && !url) from = env("ARCHIVE_LLM_URL") ? "environment" : models_setting("Url") ? "settings" : "default";
    llm_client c;
    char *err = NULL;
    int ok = !llm_open(&c, url, model, vision ? 0 : g->remote, &err) && !llm_check(&c, &err);
    if (!url) url = c.url;
    jput(d, "url", jstr(url ? url : ""));
    jput(d, "model", jstr(c.model ? c.model : model ? model : ""));
    jput(d, "from", jstr(from));
    jput(d, "external", jbool(url && !url_is_loopback(url)));
    jput(d, "available", jbool(ok));
    if (!ok && err) jput(d, "error", jstr(err));
    free(err);
    return d;
}

static int status_all(int as_json, const given *g)
{
    jv *d = jobj();
    char *path = models_settings_path();
    jput(d, "settings", jstr(path));
    jput(d, "help", jstr(models_setting("Help") && !strcmp(models_setting("Help"), "off") ? "off" : "on"));
    jput(d, "curl", jbool(on_path("curl")));
    jput(d, "text", chosen(0, g));
    if (g->vision_url || models_setting("VisionUrl")) jput(d, "vision", chosen(1, g));
    jv *servers = jarr();
    strlist probed = { 0 };
    const char *probes = env("ARV_LLM_PROBE");          /* NAME=URL;...: in place of the usual ports (for tests) */
    if (probes) {
        char *copy = xstrdup(probes);
        for (char *p = strtok(copy, ";"); p; p = strtok(NULL, ";")) {
            char *eq = strchr(p, '=');
            if (!eq) continue;
            *eq = 0;
            jpush(servers, probe(p, eq + 1));
            strlist_add(&probed, eq + 1);
        }
        free(copy);
    } else {
        for (size_t i = 0; i < sizeof LOCAL / sizeof *LOCAL; i++) {
            jpush(servers, probe(LOCAL[i][0], LOCAL[i][1]));
            strlist_add(&probed, LOCAL[i][1]);
        }
    }
    const char *mine[] = { g->url, g->vision_url, env("ARCHIVE_LLM_URL"), models_setting("Url"), models_setting("VisionUrl") };
    for (size_t i = 0; i < sizeof mine / sizeof *mine; i++)                  /* a server here at another address, as chosen */
        if (mine[i] && url_is_loopback(mine[i]) && !strlist_has(&probed, mine[i])) {
            jpush(servers, probe("chosen", mine[i]));
            strlist_add(&probed, mine[i]);
        }
    jput(d, "servers", servers);
    jv *prov = jarr();
    for (size_t i = 0; i < NPROVIDERS; i++) {
        jv *p = jobj();
        jput(p, "name", jstr(PROVIDERS[i][0]));
        jput(p, "url", jstr(provider_url(i)));
        jput(p, "key", jstr(PROVIDERS[i][2]));
        jput(p, "set", jbool(env(PROVIDERS[i][2]) != NULL));
        jput(p, "ready", jbool(env(PROVIDERS[i][2]) && on_path("curl")));
        if (!env(PROVIDERS[i][2]) || !on_path("curl"))
            jput(p, "why", jstr(!env(PROVIDERS[i][2]) ? "no key in the environment" : "needs curl"));
        jpush(prov, p);
    }
    jput(d, "providers", prov);
    jput(d, "key_set", jbool(env("ARCHIVE_LLM_KEY") != NULL));
    sbuf b = { 0 };
    if (as_json) {
        json_dump(&b, d, -1);
        printf("%s\n", b.s);
    } else {
        const jv *t = json_get(d, "text");
        printf("Describing: %s %s (%s)%s\n", jstr_of(json_get(t, "url")), jstr_of(json_get(t, "model")),
               jstr_of(json_get(t, "from")), !strcmp(json_get(t, "external")->str, "true") ? ", an external provider" : "");
        if (!strcmp(json_get(t, "available")->str, "false")) printf("  not available: %s\n", jstr_of(json_get(t, "error")));
        puts("\nOn this computer:");
        for (size_t i = 0; i < servers->n; i++) {
            const jv *s = servers->vals[i], *ms = json_get(s, "models");
            printf("  %-8s %-26s %s\n", strcmp(json_get(s, "found")->str, "true") ? "-" : "found", jstr_of(json_get(s, "name")),
                   jstr_of(json_get(s, "url")));
            for (size_t k = 0; ms && k < ms->n; k++) {
                const jv *m = ms->vals[k];
                printf("           %s%s%s", jstr_of(json_get(m, "id")), !strcmp(json_get(m, "vision")->str, "true") ? " (vision)" : "",
                       !strcmp(json_get(m, "embedding")->str, "true") ? " (embeddings)" : "");
                if (json_get(m, "context")) printf(", context %s", json_get(m, "context")->str);
                puts("");
            }
        }
        puts("\nExternal providers (used only when chosen; what is asked is sent there):");
        for (size_t i = 0; i < prov->n; i++)
            printf("  %-8s %-14s %s (%s)\n", strcmp(json_get(prov->vals[i], "set")->str, "true") ? "-" : "key set",
                   jstr_of(json_get(prov->vals[i], "name")), jstr_of(json_get(prov->vals[i], "url")),
                   jstr_of(json_get(prov->vals[i], "key")));
        printf("\nSettings: %s\nChoose with: arv-assist llm-use --url URL [--model NAME] [--external] (or arv gui's Settings)\n", path);
    }
    free(b.s);
    free(path);
    strlist_free(&probed);
    jfree(d);
    return 0;
}

/* --llm-url, --llm-model, --vision-url, --vision-model, --llm-allow-remote: 1 when argv[*i] was one */
static int given_option(int argc, char **argv, int *i, given *g)
{
    const char *a = argv[*i];
    if (!strcmp(a, "--llm-allow-remote")) return g->remote = 1;
    const char **slot = !strcmp(a, "--llm-url") ? &g->url : !strcmp(a, "--llm-model") ? &g->model
        : !strcmp(a, "--vision-url") ? &g->vision_url : !strcmp(a, "--vision-model") ? &g->vision_model : NULL;
    if (!slot || *i + 1 >= argc) return 0;
    *slot = argv[++*i];
    return 1;
}

int assist_llm_models(int argc, char **argv)
{
    int as_json = 0;
    given g = { 0 };
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) as_json = 1;
        else if (!strcmp(argv[i], "--all")) continue;
        else if (!strcmp(argv[i], "--home") && i + 1 < argc) i++;
        else if (!given_option(argc, argv, &i, &g)) return 2;
    }
    return status_all(as_json, &g);
}

/* ------------------------------------------------------------------ llm-use */

int assist_llm_use(int argc, char **argv)
{
    const char *url = NULL, *model = NULL, *vurl = NULL, *vmodel = NULL, *help = NULL, *forget = NULL;
    int remote = 0, clear = 0;
    given ignored = { 0 };
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--external")) remote = 1;
        else if (!strcmp(a, "--clear")) clear = 1;
        else if (!strcmp(a, "--on") || !strcmp(a, "--off")) help = a + 2;
        else if (!strcmp(a, "--forget") && v && (!strcmp(v, "describing") || !strcmp(v, "pictures"))) forget = argv[++i];
        else if (!strcmp(a, "--home") && v) i++;
        else if (!strcmp(a, "--url") && v) url = argv[++i];
        else if (!strcmp(a, "--model") && v) model = argv[++i];
        else if (!strcmp(a, "--pictures") && v) vurl = argv[++i];
        else if (!strcmp(a, "--pictures-model") && v) vmodel = argv[++i];
        else if (!given_option(argc, argv, &i, &ignored)) return 2;
    }
    if (!clear && !url && !model && !vurl && !vmodel && !help && !forget) return 2;
    char host[256];
    if ((url && url_host(url, host, sizeof host)) || (vurl && url_host(vurl, host, sizeof host))) {
        fprintf(stderr, "Error: not an http:// or https:// address: %s\n", url && url_host(url, host, sizeof host) ? url : vurl);
        return 1;
    }
    if (url && !url_is_loopback(url) && !remote) {
        fprintf(stderr, "Error: %s is not this computer. An external inference provider sees what it is asked (names, "
                        "dates, words from your files); add --external to choose it anyway.\n", url);
        return 1;
    }
    if (vurl && !url_is_loopback(vurl)) {
        fprintf(stderr, "Error: pictures go only to a model on this computer; %s is not one.\n", vurl);
        return 1;
    }
    rec_file f = { 0 };
    rec_record *r = rec_new(&f, "Models");
    const rec_record *old = clear ? NULL : settings_record();
    static const char *const fields[] = { "Help", "Url", "Model", "Remote", "VisionUrl", "VisionModel" };
    for (size_t i = 0; old && i < sizeof fields / sizeof *fields; i++)
        if (rec_get(old, fields[i])) rec_add(r, fields[i], rec_get(old, fields[i]));
    if (url) {
        rec_set(r, "Url", url);
        rec_set(r, "Model", model ? model : "");
        rec_set(r, "Remote", url_is_loopback(url) ? "" : "yes");
    } else if (model) {
        rec_set(r, "Model", model);
    }
    if (help) rec_set(r, "Help", !strcmp(help, "off") ? "off" : "");
    if (forget && !strcmp(forget, "describing")) {
        rec_set(r, "Url", "");
        rec_set(r, "Model", "");
        rec_set(r, "Remote", "");
    }
    if (forget && !strcmp(forget, "pictures")) {
        rec_set(r, "VisionUrl", "");
        rec_set(r, "VisionModel", "");
    }
    if (vurl) {
        rec_set(r, "VisionUrl", vurl);
        rec_set(r, "VisionModel", vmodel ? vmodel : "");
    } else if (vmodel) {
        rec_set(r, "VisionModel", vmodel);
    }
    rec_record kept = { 0 };                        /* the empty fields left out */
    kept.type = "Models";
    for (size_t i = 0; i < r->nfields; i++)
        if (*r->fields[i].value) rec_add(&kept, r->fields[i].name, r->fields[i].value);
    char *path = models_settings_path(), *dir = xstrdup(path);
    *strrchr(dir, '/') = 0;
    sbuf b = { 0 };
    sb_puts(&b, "# The models arv-assist asks, on this computer (arv gui's Settings, or arv-assist llm-use).\n"
                "# Not part of any archive. Keys are never kept here: they stay in the environment.\n"
                "# Remote: yes is the choice of an external provider, which sees what it is asked.\n"
                "# Help: off asks no model at all.\n\n");
    char *text = NULL;
    size_t len = 0;
    if (kept.nfields) rec_format(&kept, &text, &len);
    if (text) sb_add(&b, text, len);
    if (mkdirs(dir)) {
        fprintf(stderr, "Error: cannot make %s (%s)\n", dir, strerror(errno));
        return 1;
    }
    write_text(path, b.s);
    int rc = 0;
    if (rec_get(&kept, "Help")) printf("Model help is off: no model is asked, unless a command names one (--llm-url). %s\n", path);
    else if (!kept.nfields) printf("No model chosen: the default (%s), unless ARCHIVE_LLM_URL says otherwise. %s\n", LLM_DEFAULT_URL, path);
    else printf("Chosen: %s%s%s%s. Kept in %s\n", rec_get(&kept, "Url") ? rec_get(&kept, "Url") : "",
                rec_get(&kept, "Model") ? " " : "", rec_get(&kept, "Model") ? rec_get(&kept, "Model") : "",
                rec_get(&kept, "Remote") ? " (an external provider: each use says so)" : "", path);
    free(text);
    free(b.s);
    free(dir);
    free(path);
    rec_clear(&kept);
    rec_free(&f);
    return rc ? 1 : 0;
}

/* ------------------------------------------------------------------ llm-test */

/* a red square, 16 by 16 (PNG): what a pictures model is shown */
static const char RED_SQUARE[] = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAIAAACQkWg2AAAAFklEQVR42mO4oKBA"
                                 "EmIY1TCqYfhqAABLfxAQegypMgAAAABJRU5ErkJggg==";

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* one fixed question, nothing from any file: does the model answer, how fast, and as asked */
int assist_llm_test(int argc, char **argv)
{
    const char *url = NULL, *model = NULL;
    int external = 0, pictures = 0;
    given ignored = { 0 };
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--external")) external = 1;
        else if (!strcmp(a, "--pictures")) pictures = 1;
        else if (!strcmp(a, "--home") && v) i++;
        else if (!strcmp(a, "--url") && v) url = argv[++i];
        else if (!strcmp(a, "--model") && v) model = argv[++i];
        else if (!given_option(argc, argv, &i, &ignored)) return 2;
    }
    if (!url) return 2;
    jv *d = jobj();
    llm_client c;
    char *err = NULL, *reply = NULL;
    double t0 = now();
    int ok = 0, as_json = 0;
    if (!llm_open(&c, url, model, pictures ? 0 : external, &err) && llm_model(&c, &err)) {
        jv *messages = jarr(), *msg = jobj();
        jput(msg, "role", jstr("user"));
        if (pictures) {
            jv *content = jarr(), *text = jobj(), *image = jobj(), *iu = jobj();
            jput(text, "type", jstr("text"));
            jput(text, "text", jstr("What colour is this square? Answer with one word."));
            jput(iu, "url", jstr(RED_SQUARE));
            jput(image, "type", jstr("image_url"));
            jput(image, "image_url", iu);
            jpush(content, text);
            jpush(content, image);
            jput(msg, "content", content);
        } else {
            jput(msg, "content", jstr("This is a test. Reply with this JSON object and nothing else: {\"ok\": true}"));
        }
        jpush(messages, msg);
        reply = llm_chat(&c, messages, "0", !pictures, &err);
        ok = reply && *reply;
        if (ok && !pictures) {
            jv *j = json_parse(reply);
            as_json = j && j->kind == 'o';
            jfree(j);
        }
    }
    char secs[32];
    snprintf(secs, sizeof secs, "%.1f", now() - t0);
    jput(d, "ok", jbool(ok));
    jput(d, "url", jstr(url));
    jput(d, "model", jstr(c.model ? c.model : model ? model : ""));
    jput(d, "external", jbool(!url_is_loopback(url)));
    jput(d, "seconds", jnum(secs));
    if (reply) jput(d, "reply", jstr(reply));
    if (ok && !pictures) jput(d, "json", jbool(as_json));
    if (ok && pictures) {
        char *low = lower(reply);
        jput(d, "saw_red", jbool(strstr(low, "red") != NULL));
        free(low);
    }
    if (!ok) jput(d, "error", jstr(err ? err : "an empty reply"));
    sbuf b = { 0 };
    json_dump(&b, d, -1);
    printf("%s\n", b.s);
    free(b.s);
    free(reply);
    free(err);
    jfree(d);
    return 0;
}
