# arv-assist

arv's optional local-AI helpers, in C99 and POSIX with no libraries. arv runs it for three
commands; nothing here is needed to make, read, check or repair a disc.

```
arv describe FOLDER|DISC-ID [--save DRAFT] [--apply DRAFT] ...   title, description, subjects and
                                                                  folder tags from a local LLM
arv tag FOLDER|DISC-ID [--save DRAFT] [--apply] ...              folder tags from your vocabulary
arv models fetch|status|build-runtime                            the small model arv tag uses
```

`arv-assist --help` lists every option; the top-level [README](../../README.md) explains them.
arv-gui also runs `arv-assist suggest` (one round of suggestions, JSON in and out) and
`arv-assist llm-status`.

- **Local only.** `describe` talks to an OpenAI-compatible server (Ollama, llama.cpp's
  llama-server, LM Studio, vLLM) on this machine; a remote URL is refused unless
  `--llm-allow-remote`. The model sees an inventory (names, counts, sizes, dates, types, a few
  short README-style files), never file contents; `--vision` sends sampled images, resized with
  ffmpeg when it is installed, to a local vision model.
- `tag` runs llama.cpp's `llama-embedding` as a subprocess on a 37 MB model that `arv models
  fetch` downloads with `curl` and checks against a pinned SHA-256 (or `--from FILE`, offline).
- Drafts (`--save`) are JSON that `arv make --draft` takes; `--apply` writes a reviewed draft to a
  disc already in the catalogue, with a PREMIS event naming the model. A draft may come from
  anywhere (`--apply -` reads it from standard input; `--suggested` leaves a model's draft
  unreviewed): its keys are in the top-level README, "Describing from anywhere".

It links arv's own modules (`../arv`, everything but `arv.c`) for the catalogue, drafts and tags,
so it reads and writes exactly what arv does.

| File | What |
|---|---|
| `main.c` | the commands |
| `describe.c` | `describe`, `suggest`, `llm-status` |
| `tag.c` | `tag`: rules, aliases, embeddings, remembered examples, review |
| `models.c` | `models` |
| `llm.c` | the inventory, the prompt, reading the model's reply |
| `vision.c` | sampling images, captions |
| `http.c` | a small HTTP/1.1 client (plain http, to local servers) |
| `common.c` | shared by the above |

`make check` (`sh check.sh`) runs every command against a fake model server
(`dev-tools/fake-llm.c`) and a fake `llama-embedding` (`dev-tools/fake-llama-embedding`), so it
needs no model.
