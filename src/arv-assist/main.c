/*
 * arv-assist: arv's optional local-AI helpers. arv runs it for these commands:
 *
 *   arv describe FOLDER|DISC-ID [--save DRAFT] [--apply DRAFT] ...   title, description, subjects and
 *                                         folder tags from a local LLM, reviewed by you
 *   arv tag FOLDER|DISC-ID [--save DRAFT] [--apply] ...   folder tags from your tag vocabulary
 *   arv models fetch|status|build-runtime        the small built-in model for arv tag
 *
 * and arv-gui runs `arv-assist suggest` and `arv-assist llm-status`. --home and --archive work as
 * in arv.
 */
#define _XOPEN_SOURCE 700
#include "assist.h"

#include <stdio.h>
#include <string.h>

static const char USAGE[] =
    "usage: arv describe FOLDER|DISC-ID [--save DRAFT] [--apply DRAFT] [--disc-root DIR] [--rounds N] [--questions N]\n"
    "                    [--show-inventory] [--llm-url URL] [--llm-model NAME] [--llm-allow-remote]\n"
    "                    [--vision [--vision-model NAME] [--vision-url URL] [--vision-per-folder N] [--vision-max N]]\n"
    "       arv tag FOLDER|DISC-ID [--save DRAFT] [--apply] [--top N] [--vocab FILE] [--disc-root DIR] [--rules-only]\n"
    "               [--show-summaries] [--llama-embedding PATH] [--model NAME | --model-file GGUF]\n"
    "               [--embed-url URL [--embed-model NAME] [--llm-allow-remote]]\n"
    "       arv models fetch|status|build-runtime [--model NAME] [--from FILE] [--llama-embedding PATH]\n"
    "Local models only: an OpenAI-compatible server on this machine (Ollama: " LLM_DEFAULT_URL "), or\n"
    "llama.cpp's llama-embedding for arv tag. Drafts (--save) are JSON that arv make --draft takes.\n";

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
    int rc = !strcmp(cmd, "describe") ? assist_describe(n, rest) : !strcmp(cmd, "tag") ? assist_tag(n, rest)
           : !strcmp(cmd, "models") ? assist_models(n, rest) : !strcmp(cmd, "suggest") ? assist_suggest(n, rest)
           : !strcmp(cmd, "llm-status") ? assist_llm_status(n, rest) : 2;
    if (rc == 2) fputs(USAGE, stderr);
    return rc;
}
