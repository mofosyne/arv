"""arv's optional add-on, in Python: the local AI helpers and the graphical interface.

Commands:
  describe  improve titles, descriptions and tags with a local LLM
  tag       suggest folder tags from your tag vocabulary (match rules, small built-in model)
  models    fetch / check the built-in model for 'arv tag'
  gui       graphical interface in your web browser

Everything else is the arv command itself, a C program (src/arvc): the launcher (./arv in a
checkout, tools/arv/arv on a disc) passes other commands to it. The helpers write drafts
(--save d.json) that `arv make --draft d.json` takes, or apply them to a disc in the catalogue.
"""

import argparse
import os
import sys

from . import NAME, homes, llm, models, tagger, describe

REPO_NAME = NAME
COMMANDS = ("describe", "tag", "models", "gui")


def add_llm_options(parser):
    parser.add_argument("--llm-url", help="OpenAI-compatible server (default: $ARCHIVE_LLM_URL or %s, Ollama)"
                        % llm.DEFAULT_URL)
    parser.add_argument("--llm-model", help="model name (default: $ARCHIVE_LLM_MODEL or the server's first model)")
    parser.add_argument("--llm-allow-remote", action="store_true",
                        help="allow a non-local LLM server (the inventory describes your private files)")
    parser.add_argument("--vision", action="store_true",
                        help="also show sample images (and video frames, with ffmpeg) to a local vision model; "
                             "local servers only")
    parser.add_argument("--vision-model", help="vision-capable model (default: the --llm-model)")
    parser.add_argument("--vision-url", help="server for the vision model (default: the --llm-url; must be local)")
    parser.add_argument("--vision-per-folder", type=int, default=3, help="images sampled per folder (default: 3)")
    parser.add_argument("--vision-max", type=int, default=40, help="images sampled in total (default: 40)")



def cmd_tag(args):
    return tagger.run(args)


def cmd_models(args):
    return tagger.models_command(args)


def cmd_describe(args):
    return describe.run(args)


def cmd_gui(args):
    from . import gui
    return gui.main(args)


def build_parser():
    p = argparse.ArgumentParser(prog="arv", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--home", help="home catalogue folder (default: $ARV_HOME, else the nearest .arv folder "
                                  "or pointer above the current folder, else the machine config's default; "
                                  "see `arv where`)")
    p.add_argument("--archive", metavar="NAME", help="use the home registered under NAME in ~/.config/arv/homes.rec")
    sub = p.add_subparsers(dest="command", required=True)
    ds = sub.add_parser("describe", help="improve metadata with a local LLM (a folder, or a disc in the catalogue)")
    ds.add_argument("target", help="folder to be archived, or a disc id")
    ds.add_argument("--rounds", type=int, default=2, help="question rounds (default: 2)")
    ds.add_argument("--questions", type=int, default=5, help="questions per round (default: 5)")
    ds.add_argument("--save", help="write the reviewed result as a draft JSON for 'arv make --draft'")
    ds.add_argument("--disc-root", help="mounted disc, so README-style files on it can be read")
    ds.add_argument("--show-inventory", action="store_true", help="print exactly what would be sent, and stop")
    ds.add_argument("--apply", metavar="DRAFT", help="apply a saved draft to the disc (no LLM needed)")
    add_llm_options(ds)
    ds.set_defaults(func=cmd_describe)
    tg = sub.add_parser("tag", help="suggest folder tags from your tag vocabulary (small built-in model)")
    tg.add_argument("target", help="folder to be archived, or a disc id")
    tg.add_argument("--top", type=int, default=3, help="at most this many tags per folder (default: 3)")
    tg.add_argument("--vocab", help="tag vocabulary recfile (default: <home>/config/tags.rec)")
    tg.add_argument("--save", help="write (or merge into) a draft JSON for 'arv make --draft'")
    tg.add_argument("--apply", action="store_true", help="for a disc: write the tags without prompting")
    tg.add_argument("--disc-root", help="mounted disc, so README files on it can be read")
    tg.add_argument("--show-summaries", action="store_true", help="print what the model compares, and stop")
    tg.add_argument("--rules-only", action="store_true",
                    help="only the vocabulary's Match rules (no model needed)")
    tg.add_argument("--llama-embedding", help="path to llama.cpp's llama-embedding")
    tg.add_argument("--model", default=models.DEFAULT_EMBEDDING, help="built-in model (default: %(default)s)")
    tg.add_argument("--embed-url", help="use an OpenAI-compatible /v1/embeddings server instead of the built-in model")
    tg.add_argument("--embed-model", help="embedding model name on that server (default: its first model)")
    tg.add_argument("--llm-allow-remote", action="store_true", help="allow a non-local embeddings server")
    tg.set_defaults(func=cmd_tag)
    mo = sub.add_parser("models", help="built-in model for 'arv tag': fetch, status, build-runtime")
    mo.add_argument("action", choices=["fetch", "status", "build-runtime"])
    mo.add_argument("--model", default=models.DEFAULT_EMBEDDING, choices=list(models.MODELS))
    mo.add_argument("--from", dest="from_file", help="install from a local file (checksum-verified) instead of downloading")
    mo.add_argument("--llama-embedding", help="path to llama.cpp's llama-embedding")
    mo.set_defaults(func=cmd_models)
    g = sub.add_parser("gui", help="open the graphical interface in your web browser")
    g.add_argument("--port", type=int, default=0, help="port on 127.0.0.1 (default: any free port)")
    g.add_argument("--no-browser", action="store_true", help="print the URL instead of opening a browser")
    add_llm_options(g)
    g.set_defaults(func=cmd_gui)
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    args.home, args.home_found = homes.find(args.home, args.archive)
    try:
        return args.func(args)
    except BrokenPipeError:  # output piped into e.g. `head`, which stopped reading
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        return 0
