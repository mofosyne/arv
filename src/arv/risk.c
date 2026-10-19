/*
 * How much a command changes, for a program that runs arv for someone (arv-assist's assistant,
 * research/plan.md, "an assistant for archival work"): it may run a command of no risk on its own;
 * any other it proposes, and a person applies it.
 *
 *   none     it only reads
 *   low      a value as easily set back: a tag, a label, a note, a description
 *   medium   the record's shape: a name added, a plan changed, a copy recorded, a check logged
 *   high     hard to undo, or beyond the record: names merged or retired, access, a disc made,
 *            files written, packages installed
 *
 * The manual's "Reading and changing the record" says the same in words; arv help --json gives
 * each command's, and src/arv/check.sh holds the two together. A command no rule names is high.
 */
#define _XOPEN_SOURCE 700
#include "arv.h"

#include <string.h>

typedef struct {
    const char *cmd, *sub, *flag, *risk;    /* sub: its first word; flag: anywhere after it */
} risk_rule;

/* the first rule that matches wins: the particular before the general */
static const risk_rule RULES[] = {
    { "drives", NULL, "--check", "medium" }, { "drives", NULL, NULL, "none" },
    { "where", NULL, "--here", "medium" }, { "where", NULL, NULL, "none" },
    { "programs", NULL, "--install", "high" }, { "programs", NULL, NULL, "none" },
    { "retire", NULL, "--yes", "high" }, { "retire", NULL, NULL, "none" },
    { "appraise", NULL, "--due", "none" }, { "appraise", NULL, NULL, "medium" },
    { "check", NULL, "--repair", "high" }, { "check", NULL, NULL, "medium" },
    { "plan", "list", NULL, "none" }, { "plan", "show", NULL, "none" }, { "plan", "tag", NULL, "low" },
    { "plan", "make", NULL, "high" }, { "plan", "again", NULL, "high" }, { "plan", "delete", NULL, "high" },
    { "plan", NULL, NULL, "medium" },
    { "collection", "list", NULL, "none" }, { "collection", "show", NULL, "none" }, { "collection", NULL, NULL, "medium" },
    { "location", "list", NULL, "none" }, { "location", NULL, NULL, "medium" },
    { "tags", "names", NULL, "none" }, { "tags", "show", NULL, "none" }, { "tags", "label", NULL, "low" },
    { "tags", "alias", NULL, "low" }, { "tags", "add", NULL, "medium" }, { "tags", "merge", NULL, "high" },
    { "tags", "split", NULL, "high" }, { "tags", "retire", NULL, "high" }, { "tags", NULL, NULL, "none" },
    { "list", NULL, NULL, "none" }, { "find", NULL, NULL, "none" }, { "objects", NULL, NULL, "none" },
    { "todo", NULL, NULL, "none" }, { "status", NULL, NULL, "none" }, { "log", NULL, NULL, "none" },
    { "diff", NULL, NULL, "none" }, { "sets", NULL, NULL, "none" }, { "map", NULL, NULL, "none" },
    { "keywords", NULL, NULL, "none" }, { "info", NULL, NULL, "none" }, { "audit", NULL, NULL, "none" },
    { "inventory", NULL, NULL, "none" }, { "names", NULL, NULL, "none" }, { "help", NULL, NULL, "none" },
    { "note", NULL, NULL, "low" }, { "retag", NULL, NULL, "low" }, { "undo", NULL, NULL, "low" }, { "describe", NULL, NULL, "low" },
    { "tag", NULL, NULL, "low" },
    { "init", NULL, NULL, "medium" }, { "link", NULL, NULL, "medium" }, { "rename", NULL, NULL, "medium" },
    { "same", NULL, NULL, "medium" }, { "identify", NULL, NULL, "medium" }, { "relabel", NULL, NULL, "medium" },
    { "locate", NULL, NULL, "medium" }, { "stored", NULL, NULL, "medium" }, { "burned", NULL, NULL, "medium" },
    { "access", NULL, NULL, "high" }, { "make", NULL, NULL, "high" }, { "rebuild", NULL, NULL, "high" },
    { "restore", NULL, NULL, "high" }, { "gui", NULL, NULL, "high" }, { "chat", NULL, NULL, "high" },
};

const char *command_risk(int argc, char *const *argv)
{
    if (argc < 1) return "high";
    for (size_t r = 0; r < sizeof RULES / sizeof *RULES; r++) {
        const risk_rule *k = &RULES[r];
        if (strcmp(k->cmd, argv[0])) continue;
        if (k->sub && (argc < 2 || strcmp(k->sub, argv[1]))) continue;
        int has = !k->flag;
        for (int i = 1; !has && i < argc; i++) has = !strcmp(argv[i], k->flag);
        if (has) return k->risk;
    }
    return "high";
}

void command_risks_json(sbuf *b, const char *cmd)
{
    char *plain[] = { (char *)cmd };
    const char *risk = command_risk(1, plain);
    sb_printf(b, ", \"reads\": %s, \"risk\": \"%s\", \"risks\": [", strcmp(risk, "none") ? "false" : "true", risk);
    int first = 1;
    for (size_t r = 0; r < sizeof RULES / sizeof *RULES; r++) {
        const risk_rule *k = &RULES[r];
        if (strcmp(k->cmd, cmd) || (!k->sub && !k->flag)) continue;
        sb_printf(b, "%s{\"when\": \"%s%s%s\", \"risk\": \"%s\"}", first ? "" : ", ", k->sub ? k->sub : "",
                  k->sub && k->flag ? " " : "", k->flag ? k->flag : "", k->risk);
        first = 0;
    }
    sb_puts(b, "]");
}
