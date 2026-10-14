/*
 * arv-assist: arv's optional local-AI helpers. arv runs it for these commands:
 *
 *   arv describe FOLDER|DISC-ID|OBJECT [--save DRAFT] [--apply DRAFT|-] ...   title, description,
 *                                         subjects and folder tags from a local LLM, reviewed by you
 *   arv tag FOLDER|DISC-ID [--save DRAFT] [--apply] ...   folder tags from your tag vocabulary
 *
 * and arv-gui runs `arv-assist suggest` and `arv-assist llm-status`. --home and --archive work as
 * in arv.
 */
#define _XOPEN_SOURCE 700
#include "assist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char USAGE[] =
    "usage: arv describe FOLDER|DISC-ID|OBJECT [--save DRAFT] [--apply DRAFT|- [--suggested]] [--disc-root DIR] [--rounds N]\n"
    "                    [--questions N]\n"
    "       arv describe DISC-ID|OBJECT --answer   the questions kept from before (arv todo lists them); no model needed\n"
    "                    [--show-inventory] [--llm-url URL] [--llm-model NAME] [--llm-allow-remote]\n"
    "                    [--vision [--vision-model NAME] [--vision-url URL] [--vision-per-folder N] [--vision-max N]]\n"
    "       arv tag FOLDER|DISC-ID [--save DRAFT] [--apply] [--top N] [--vocab FILE] [--disc-root DIR] [--rules-only]\n"
    "               [--show-summaries] [--embed-url URL [--embed-model NAME] [--llm-allow-remote]]\n"
    "Model servers are local by default (Ollama: " LLM_DEFAULT_URL "); --llm-allow-remote permits remote URLs.\n"
    "Tag embeddings use an OpenAI-compatible /v1/embeddings endpoint. Drafts (--save) are JSON that arv make --draft takes.\n";

/* arv COMMAND --help: that command's lines of the usage, then the note on local models */
static void command_usage(const char *cmd)
{
    char *word = xprintf("arv %s ", cmd);
    int in = 0;
    for (const char *line = USAGE, *end; *line; line = end + 1) {
        end = strchr(line, '\n');
        size_t len = (size_t)(end - line);
        int starts = !strncmp(line, "usage: arv ", 11) || !strncmp(line, "       arv ", 11);
        if (starts) in = strstr(line, word) && strstr(line, word) < line + len;
        else if (line[0] != ' ') in = 1;            /* the closing note */
        if (!in) continue;
        if (starts && line[0] == ' ') printf("usage: %.*s\n", (int)len - 7, line + 7);   /* "       arv ..." */
        else printf("%.*s\n", (int)len, line);
    }
    free(word);
}

int main(int argc, char **argv)
{
    arv_argv0 = argv[0];
    const char *home = NULL;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--home") && i + 1 < argc) home = argv[++i];
        else if (!strcmp(argv[i], "-C") && i + 1 < argc) home = argv[++i];
        else if (!strcmp(argv[i], "--archive") && i + 1 < argc) home_archive_name = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { fputs(USAGE, stdout); return 0; }
        else break;
    }
    if (i >= argc) { fputs(USAGE, stderr); return 2; }
    const char *cmd = argv[i];
    /* the rest, with --home passed on to the command */
    char **rest = xmalloc((size_t)(argc + 3) * sizeof *rest);
    int n = 0;
    for (int k = i + 1; k < argc; k++) rest[n++] = argv[k];
    if (home) { rest[n++] = "--home"; rest[n++] = (char *)home; }
    rest[n] = NULL;
    for (int k = i + 1; k < argc; k++)
        if (!strcmp(argv[k], "-h") || !strcmp(argv[k], "--help")) {
            command_usage(cmd);
            return 0;
        }
    int rc = !strcmp(cmd, "describe") ? assist_describe(n, rest) : !strcmp(cmd, "tag") ? assist_tag(n, rest)
           : !strcmp(cmd, "suggest") ? assist_suggest(n, rest)
           : !strcmp(cmd, "llm-status") ? assist_llm_status(n, rest) : 2;
    if (rc == 2) fputs(USAGE, stderr);
    return rc;
}
