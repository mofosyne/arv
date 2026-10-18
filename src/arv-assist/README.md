# arv-assist

arv's optional local-AI helpers, in C99 and POSIX with no libraries. arv runs it for two
commands; nothing here is needed to make, read, check or repair a disc.

```
arv describe FOLDER|DISC-ID|OBJECT [--save DRAFT] [--apply DRAFT] ...
                                          title, description, subjects and folder tags from a local LLM
arv describe DISC-ID|OBJECT --answer      the questions kept from before (no model)
arv tag FOLDER|DISC-ID [--save DRAFT] [--apply] ...   folder tags from your vocabulary
```

`arv-assist --help` lists every option; the top-level [README](../../README.md) explains them.
arv-gui also runs `arv-assist suggest` (one round of suggestions, JSON in and out),
`arv-assist llm-status [--all]` and `arv-assist llm-use` (its Settings).

- **Local by default.** `describe` talks to an OpenAI-compatible server (Ollama, llama.cpp's
  llama-server, LM Studio, vLLM) on this machine; a remote URL is refused unless
  `--llm-allow-remote`. The model sees an inventory (names, counts, sizes, dates, types, a few
  short README-style files), never file contents; `--vision` sends sampled images, resized with
  ffmpeg when it is installed, to a local vision model, and never to another computer.
- **Which model** (`models.c`): an option, then `$ARCHIVE_LLM_URL`, then the choice kept in
  `$XDG_CONFIG_HOME/arv/models.rec` (`~/.config/arv/`, this computer's, in no archive), then Ollama's
  address. `llm-status --all` lists the servers on this computer (Ollama, llama.cpp, LM Studio,
  vLLM and Jan at their usual ports, with each model's pictures, embeddings and context where the
  server says) and the external providers whose keys are set, each ready or not; `llm-use [--on|--off]
  [--url URL [--model NAME] [--external]] [--pictures URL [--pictures-model NAME]] [--forget
  describing|pictures] [--clear]` keeps a choice (`--off`: no model is asked unless a command names
  one); `llm-test --url URL [--model NAME] [--external] [--pictures]` asks a fixed question, nothing
  from any file, and says whether it answered, how fast, and in JSON (for pictures, a red square).
- **Your own key (an external provider).** OpenAI, Anthropic, Google Gemini, Mistral, OpenRouter,
  Groq and DeepSeek by their OpenAI-compatible addresses, the key from their usual variable
  (`OPENAI_API_KEY` ...) or `ARCHIVE_LLM_KEY`, read at each request and never written down, and never
  sent over plain http to another computer. One is used only when chosen (`--llm-allow-remote`, or
  `llm-use --external`), and each use says so on standard error. https goes through curl, given
  its options on standard input so the key is in no command line.
- `tag` uses an OpenAI-compatible `/v1/embeddings` server selected with `--embed-url`; remote
  endpoints also need `--llm-allow-remote`. Use `--rules-only` to tag from vocabulary match rules
  without a server. It does not download or build models or runtimes.
- A data object (`arv describe NAME`, or `object:NAME`) is described as a disc is: the model sees
  its files from its disc's listing and what it says about itself already; its title,
  description and notes are written on its newest version, with an event naming it.
- Questions the model asks in a draft written with nobody there are kept for later
  (`<home>/drafts/questions.rec`, home only): `arv todo` lists them, `--answer` asks them, and an
  interactive `describe` asks them first. Their file is arv's (`src/arv/questions.c`).
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
| `llm.c` | the inventory, the prompt, reading the model's reply |
| `vision.c` | sampling images, captions |
| `http.c` | a small HTTP/1.1 client (plain http, to local servers) |
| `common.c` | shared by the above |

`make check` (`sh check.sh`) runs every command against a fake model server
(`dev-tools/fake-llm.c`), including its `/v1/embeddings` endpoint, so it needs no model.
