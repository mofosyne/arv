/*
 * The assistant (research/plan.md, "an assistant for archival work"): a small loop around a model.
 * Each reply is one JSON object: read (an arv command that only reads, run at once, its output
 * given back), propose (a change, which the person applies, or not) or say. What it may read is
 * arv's own split (command_risk, risk.c): a "read" of anything else becomes a proposal. What it
 * knows is a text file (data/assistant.txt, or the home's config/assistant.txt) and the command
 * list arv help --json gives. Sized for local models: the context is a budget, a read's output is
 * cut to an eighth of it, and old outputs go first when the conversation is full.
 *
 *   arv-assist chat [--llm-context N] [LLM options]        at a terminal: lines in, lines out
 *   arv-assist chat --json [--llm-context N] [LLM options] for the GUI: {"messages": [...],
 *       "context": "what is on screen"} in; {"steps": [...], "reply": {...}, "messages": [...]} out
 *   arv-assist chat --risk                                 a proposal the person edited, {"command":
 *       [...]} in, judged again: {"propose": [...], "risk": ..., "shown": ...} out; no model needed
 */
#define _XOPEN_SOURCE 700
#include "assist.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern const char ASSISTANT[];              /* data/assistant.txt, built in */

#define MAX_READS 6                         /* reads in one turn before an answer is asked for */
#define REPLY_TOKENS 1024                   /* kept free for the model's reply */

typedef struct {
    llm_client c;
    const char *home;
    char *arv, *system;
    int budget;                             /* the context, in tokens */
} chat;

static int tokens_of(const char *s)         /* roughly: no tokeniser, a margin instead */
{
    return (int)(strlen(s) * 2 / 7) + 4;
}

/* ------------------------------------------------------------------ running arv */

/* arv ARGV (after --home), standard input empty: its exit status, and what it printed in *out */
static int run_arv(const chat *ch, const strlist *argv, char **out)
{
    int fds[2];
    *out = NULL;
    if (!ch->arv || pipe(fds)) {
        *out = xstrdup(ch->arv ? "cannot run arv" : "arv is not found (set $ARV)");
        return 127;
    }
    pid_t pid = fork();
    if (!pid) {
        int null = open("/dev/null", O_RDONLY);
        dup2(null, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        char **av = xmalloc((argv->n + 4) * sizeof *av);
        size_t n = 0;
        av[n++] = ch->arv;
        if (ch->home) { av[n++] = "--home"; av[n++] = (char *)ch->home; }
        for (size_t i = 0; i < argv->n; i++) av[n++] = argv->v[i];
        av[n] = NULL;
        execv(ch->arv, av);
        _exit(127);
    }
    close(fds[1]);
    sbuf b = { 0 };
    char buf[65536];
    ssize_t r;
    while ((r = read(fds[0], buf, sizeof buf)) > 0) sb_add(&b, buf, (size_t)r);
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    *out = b.s ? b.s : xstrdup("");
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static char *shown(const strlist *argv)     /* arv find "a b": as a shell would take it */
{
    sbuf b = { 0 };
    sb_puts(&b, "arv");
    for (size_t i = 0; i < argv->n; i++) {
        const char *a = argv->v[i];
        int plain = *a && !a[strcspn(a, " \t'\"\\$`|&;<>()*?[]#~")];
        if (plain) sb_printf(&b, " %s", a);
        else {
            sb_puts(&b, " '");
            for (; *a; a++) sb_puts(&b, *a == '\'' ? "'\\''" : (char[]){ *a, 0 });
            sb_puts(&b, "'");
        }
    }
    return b.s;
}

/* ------------------------------------------------------------------ what it knows */

/* the instructions, then the commands: a line each, marked reads or propose (and its risk) */
static char *system_text(const chat *ch)
{
    sbuf b = { 0 };
    char *mine = ch->home ? xprintf("%s/config/assistant.txt", ch->home) : NULL, *text = mine ? read_text(mine) : NULL;
    sb_puts(&b, text ? text : ASSISTANT);
    free(mine);
    free(text);
    strlist help = { 0 };
    strlist_add(&help, "help");
    strlist_add(&help, "--json");
    char *out = NULL;
    int rc = run_arv(ch, &help, &out);
    strlist_free(&help);
    jv *h = rc ? NULL : json_parse(out);
    free(out);
    sb_puts(&b, "\nCommands (reads: run it; otherwise propose it; the risk says how much it changes):\n");
    const jv *groups = json_get(h, "groups");
    for (size_t g = 0; groups && groups->kind == 'a' && g < groups->n; g++) {
        const jv *cmds = json_get(groups->vals[g], "commands");
        for (size_t i = 0; cmds && cmds->kind == 'a' && i < cmds->n; i++) {
            const jv *c = cmds->vals[i], *risks = json_get(c, "risks");
            const char *use = jstr_of(json_get(c, "usage")), *risk = jstr_of(json_get(c, "risk")), *name = jstr_of(json_get(c, "name"));
            if (!use || !name || !strcmp(name, "gui") || !strcmp(name, "help")) continue;
            sb_printf(&b, "- %.*s  [%s", (int)(strlen(use) > 4 && !strncmp(use, "arv ", 4) ? strlen(use) - 4 : strlen(use)),
                      !strncmp(use, "arv ", 4) ? use + 4 : use, risk && !strcmp(risk, "none") ? "reads" : risk ? risk : "high");
            for (size_t k = 0; risks && risks->kind == 'a' && k < risks->n; k++) {
                const char *when = jstr_of(json_get(risks->vals[k], "when")), *r = jstr_of(json_get(risks->vals[k], "risk"));
                if (when && r) sb_printf(&b, "; %s: %s", when, !strcmp(r, "none") ? "reads" : r);
            }
            sb_puts(&b, "]\n");
        }
    }
    jfree(h);
    return b.s;
}

/* the context the server says it has (llama.cpp's /props), else 8,192 */
static int context_of_server(const llm_client *c)
{
    char *root = xstrdup(c->url), *reply = NULL, *err = NULL;
    size_t n = strlen(root);
    if (n >= 3 && !strcmp(root + n - 3, "/v1")) root[n - 3] = 0;
    char *u = xprintf("%s/props", root);
    int status = 0, ctx = 0;
    if (!c->external && !http_request(u, "GET", NULL, c->key, 3, &status, &reply, &err) && status == 200) {
        jv *p = json_parse(reply);
        const jv *d = json_get(p, "default_generation_settings"), *v = json_get(d, "n_ctx");
        if (!v) v = json_get(p, "n_ctx");
        if (v && v->kind == 't' && v->str) ctx = atoi(v->str);
        jfree(p);
    }
    free(root);
    free(u);
    free(reply);
    free(err);
    return ctx > 0 ? ctx : 8192;
}

/* ------------------------------------------------------------------ the conversation */

static void say_to(jv *messages, const char *role, const char *content)
{
    jv *m = jobj();
    jput(m, "role", jstr(role));
    jput(m, "content", jstr(content));
    jpush(messages, m);
}

/* a copy of the conversation for one request: the system text first, then as much of the rest as
 * the budget holds: older reads' output dropped first, then older turns; never the latest */
static jv *for_request(const chat *ch, const jv *messages)
{
    int room = ch->budget - REPLY_TOKENS - tokens_of(ch->system), total = 0;
    size_t n = messages->n;
    char *drop = xmalloc(n + 1);
    memset(drop, 0, n + 1);
    for (size_t i = 0; i < n; i++) total += tokens_of(jstr_of(json_get(messages->vals[i], "content")) ? jstr_of(json_get(messages->vals[i], "content")) : "");
    for (int pass = 0; pass < 2 && total > room; pass++)        /* outputs first, then whole turns */
        for (size_t i = 0; i + 2 < n && total > room; i++) {
            const char *t = jstr_of(json_get(messages->vals[i], "content"));
            if (drop[i] || !t || (pass == 0 && strncmp(t, "Output of ", 10))) continue;
            drop[i] = 1;
            total -= tokens_of(t);
        }
    jv *out = jarr();
    say_to(out, "system", ch->system);
    for (size_t i = 0; i < n; i++) {
        const char *role = jstr_of(json_get(messages->vals[i], "role")), *t = jstr_of(json_get(messages->vals[i], "content"));
        if (!role || !t) continue;
        say_to(out, role, drop[i] ? "[left out: the conversation is longer than the model's context]" : t);
    }
    free(drop);
    return out;
}

/* the reply's JSON object, also when fenced in ``` or said with words around it */
static jv *reply_object(const char *reply)
{
    const char *a = strchr(reply, '{'), *z = strrchr(reply, '}');
    if (!a || !z || z < a) return NULL;
    char *s = xprintf("%.*s", (int)(z - a + 1), a);
    jv *j = json_parse(s);
    free(s);
    if (j && j->kind != 'o') { jfree(j); j = NULL; }
    return j;
}

static int argv_of(const jv *a, strlist *out)          /* a list of strings, "arv" first or not */
{
    memset(out, 0, sizeof *out);
    if (!a || a->kind != 'a' || !a->n) return -1;
    for (size_t i = 0; i < a->n; i++) {
        const char *s = jstr_of(a->vals[i]);
        if (!s) { strlist_free(out); return -1; }
        if (!i && !strcmp(s, "arv") && a->n > 1) continue;
        if (!strcmp(s, "--home") || !strcmp(s, "-C") || !strcmp(s, "--archive")) { strlist_free(out); return -1; }
        strlist_add(out, s);
    }
    return out->n ? 0 : -1;
}

/* One turn: the model reads until it proposes or says something. Each read is put in *steps;
 * the answer is returned ({"say": ...} or {"propose": [...], "why": ..., "risk": ...}). */
static jv *turn(chat *ch, jv *messages, jv *steps, void (*progress)(const jv *step))
{
    for (int round = 0; round <= MAX_READS; round++) {
        if (round == MAX_READS) say_to(messages, "user", "That is enough reading: answer now, with say or propose.");
        jv *req = for_request(ch, messages);
        char *err = NULL, *reply = llm_chat(&ch->c, req, "0.2", 1, &err);
        if (!reply) {
            jv *a = jobj();
            jput(a, "error", jstr(err ? err : "no reply"));
            free(err);
            return a;
        }
        say_to(messages, "assistant", reply);
        jv *j = reply_object(reply);
        const jv *rd = json_get(j, "read"), *pr = json_get(j, "propose"), *sy = json_get(j, "say");
        strlist argv;
        if (rd && round < MAX_READS && !argv_of(rd, &argv)) {
            const char *risk = command_risk((int)argv.n, argv.v);
            if (strcmp(risk, "none")) {                         /* not a read: the person decides */
                jv *a = jobj();
                jput(a, "propose", jstrings(&argv));
                jput(a, "why", jstr("asked to run this, which changes the record"));
                jput(a, "risk", jstr(risk));
                char *cmd = shown(&argv);
                jput(a, "shown", jstr(cmd));
                free(cmd);
                strlist_free(&argv);
                jfree(j);
                free(reply);
                return a;
            }
            char *out = NULL, *cmd = shown(&argv);
            int rc = run_arv(ch, &argv, &out);
            size_t cap = (size_t)ch->budget / 8 * 7 / 2, len = strlen(out);
            char *note = len > cap ? xprintf("\n[... %zu more bytes left out: read narrower: one disc, one path, one namespace]", len - cap) : xstrdup("");
            char *msg = xprintf("Output of `%s` (exit %d):\n%.*s%s", cmd, rc, (int)(len > cap ? cap : len), out, note);
            say_to(messages, "user", msg);
            jv *step = jobj();
            jput(step, "command", jstrings(&argv));
            jput(step, "shown", jstr(cmd));
            jput(step, "risk", jstr("none"));
            char code[16];
            snprintf(code, sizeof code, "%d", rc);
            jput(step, "exit", jnum(code));
            jput(step, "output", jstr(out));
            jpush(steps, step);
            if (progress) progress(step);
            free(msg);
            free(note);
            free(out);
            free(cmd);
            strlist_free(&argv);
            jfree(j);
            free(reply);
            continue;
        }
        jv *a = jobj();
        if (pr && !argv_of(pr, &argv)) {
            jput(a, "propose", jstrings(&argv));
            const char *why = jstr_of(json_get(j, "why"));
            jput(a, "why", jstr(why ? why : ""));
            jput(a, "risk", jstr(command_risk((int)argv.n, argv.v)));
            char *cmd = shown(&argv);
            jput(a, "shown", jstr(cmd));
            free(cmd);
            strlist_free(&argv);
        } else if (sy && jstr_of(sy)) {
            jput(a, "say", jstr(jstr_of(sy)));
        } else {                                                /* not as asked: shown as it came */
            jput(a, "say", jstr(reply));
            jput(a, "unformatted", jnum("true"));
        }
        jfree(j);
        free(reply);
        return a;
    }
    return NULL;
}

/* ------------------------------------------------------------------ at a terminal, and for the GUI */

static void print_step(const jv *step)
{
    const char *out = jstr_of(json_get(step, "output"));
    int lines = 0;
    for (const char *p = out ? out : ""; *p; p++) lines += *p == '\n';
    fprintf(stderr, "  $ %s   (exit %s, %d line%s)\n", jstr_of(json_get(step, "shown")), json_get(step, "exit")->str, lines, lines == 1 ? "" : "s");
}

static int terminal(chat *ch)
{
    int tty = isatty(0);
    jv *messages = jarr();
    char line[4096];
    if (tty) fprintf(stderr, "Ask about the archive (%s). It reads on its own; a change it proposes is yours to apply. "
                             "Ctrl+D ends.\n", ch->c.model ? ch->c.model : "the model");
    for (;;) {
        if (tty) fputs("> ", stderr);
        if (!fgets(line, sizeof line, stdin)) break;
        line[strcspn(line, "\n")] = 0;
        if (!*line) continue;
        say_to(messages, "user", line);
        jv *steps = jarr(), *a = turn(ch, messages, steps, print_step);
        const char *say = jstr_of(json_get(a, "say")), *error = jstr_of(json_get(a, "error"));
        if (error) fprintf(stderr, "Error: %s\n", error);
        else if (say) printf("%s\n", say);
        else if (json_get(a, "propose")) {
            const char *risk = jstr_of(json_get(a, "risk")), *why = jstr_of(json_get(a, "why"));
            printf("Proposed (%s risk): %s\n", risk, jstr_of(json_get(a, "shown")) ? jstr_of(json_get(a, "shown")) : "?");
            if (why && *why) printf("  %s\n", why);
            char yes[16] = "";
            if (tty) {
                fputs("Apply it? [y/N] ", stderr);
                if (!fgets(yes, sizeof yes, stdin)) yes[0] = 0;
            } else {
                puts("  (not applied: answer at a terminal, or run it yourself)");
            }
            strlist argv;
            if ((yes[0] == 'y' || yes[0] == 'Y') && !argv_of(json_get(a, "propose"), &argv)) {
                char *out = NULL;
                int rc = run_arv(ch, &argv, &out);
                fputs(out, stdout);
                char *msg = xprintf("I applied it: exit %d.\n%.2000s", rc, out);
                say_to(messages, "user", msg);
                free(msg);
                free(out);
                strlist_free(&argv);
            } else {
                say_to(messages, "user", "I did not apply it.");
            }
        }
        fflush(stdout);
        jfree(a);
        jfree(steps);
    }
    jfree(messages);
    return 0;
}

static int for_gui(chat *ch)
{
    sbuf in = { 0 };
    char buf[65536];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, stdin)) > 0) sb_add(&in, buf, r);
    jv *req = in.s ? json_parse(in.s) : NULL, *messages = jarr(), *d = jobj(), *steps = jarr();
    free(in.s);
    const jv *given = json_get(req, "messages");
    for (size_t i = 0; given && given->kind == 'a' && i < given->n; i++) {
        const char *role = jstr_of(json_get(given->vals[i], "role")), *t = jstr_of(json_get(given->vals[i], "content"));
        if (role && t && (!strcmp(role, "user") || !strcmp(role, "assistant"))) say_to(messages, role, t);
    }
    const char *context = jstr_of(json_get(req, "context"));
    if (context && *context && messages->n) {                  /* what is on screen, with the latest words */
        jv *last = messages->vals[messages->n - 1];
        char *t = xprintf("%s\n\n(On screen: %s)", jstr_of(json_get(last, "content")), context);
        jput(last, "content", jstr(t));
        free(t);
    }
    jv *a = messages->n ? turn(ch, messages, steps, NULL) : NULL;
    jput(d, "steps", steps);
    jput(d, "reply", a ? a : jobj());
    jput(d, "messages", messages);
    jput(d, "model", jstr(ch->c.model ? ch->c.model : ""));
    jput(d, "external", jnum(ch->c.external ? "true" : "false"));
    sbuf b = { 0 };
    json_dump(&b, d, -1);
    printf("%s\n", b.s);
    free(b.s);
    jfree(d);
    jfree(req);
    return 0;
}

/* --risk: a command as the GUI's Edit left it ({"command": [...]} on stdin), judged again, with no
 * model: {"risk": ..., "shown": ...}, or {"error": ...} */
static int risk_of_stdin(void)
{
    sbuf in = { 0 };
    char buf[65536];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, stdin)) > 0) sb_add(&in, buf, r);
    jv *req = in.s ? json_parse(in.s) : NULL, *d = jobj();
    free(in.s);
    strlist argv = { 0 };
    if (!argv_of(json_get(req, "command"), &argv) && argv.n) {
        jput(d, "propose", jstrings(&argv));
        jput(d, "risk", jstr(command_risk((int)argv.n, argv.v)));
        char *cmd = shown(&argv);
        jput(d, "shown", jstr(cmd));
        free(cmd);
    } else
        jput(d, "error", jstr("no command: a list of its words, as {\"command\": [\"retag\", ...]}"));
    strlist_free(&argv);
    sbuf b = { 0 };
    json_dump(&b, d, -1);
    printf("%s\n", b.s);
    free(b.s);
    jfree(d);
    jfree(req);
    return 0;
}

int assist_chat(int argc, char **argv)
{
    chat ch = { 0 };
    const char *url = NULL, *model = NULL;
    int remote = 0, as_json = 0, risk = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--json")) as_json = 1;
        else if (!strcmp(a, "--risk")) risk = 1;
        else if (!strcmp(a, "--llm-allow-remote")) remote = 1;
        else if (!strcmp(a, "--home") && v) ch.home = argv[++i];
        else if (!strcmp(a, "--llm-url") && v) url = argv[++i];
        else if (!strcmp(a, "--llm-model") && v) model = argv[++i];
        else if (!strcmp(a, "--llm-context") && v) ch.budget = atoi(argv[++i]);
        else if ((!strcmp(a, "--vision-url") || !strcmp(a, "--vision-model")) && v) i++;   /* arv gui passes them on */
        else return 2;
    }
    if (risk) return risk_of_stdin();
    char *err = NULL;
    if (llm_open(&ch.c, url, model, remote, &err) || !llm_model(&ch.c, &err)) {
        if (as_json) {
            jv *d = jobj();
            jput(d, "error", jstr(err));
            sbuf b = { 0 };
            json_dump(&b, d, -1);
            printf("%s\n", b.s);
            free(b.s);
            jfree(d);
            return 0;
        }
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }
    if (ch.budget < 2048) ch.budget = context_of_server(&ch.c);
    ch.arv = helper_program("ARV", "arv", "arv/build/arv");
    ch.system = system_text(&ch);
    int rc = as_json ? for_gui(&ch) : terminal(&ch);
    free(ch.system);
    free(ch.arv);
    return rc;
}
