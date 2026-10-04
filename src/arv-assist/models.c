/*
 * arv models (models.py): the built-in model for arv tag, a pinned embedding model run by
 * llama.cpp's llama-embedding program. Nothing here is needed for archiving.
 *
 * - Models are pinned to an exact Hugging Face revision and SHA-256, downloaded once into
 *   <home>/cache/models/ (with curl), never committed: every disc carries the repository, so
 *   weights would ride along forever.
 * - The runtime is llama.cpp's llama-embedding, run as a plain program (texts in, vectors out): no
 *   server, no port. It is taken from --llama-embedding, $ARCHIVE_LLAMA_EMBEDDING,
 *   <home>/cache/runtime/, or PATH; arv models build-runtime compiles it (git, cmake, a C++ compiler).
 */
#define _XOPEN_SOURCE 700
#include "assist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const struct model {
    const char *name, *file, *url, *sha256, *licence, *kind, *query_prefix;
    long long size;
} MODELS[] = {
    { "bge-small-en-v1.5", "bge-small-en-v1.5-q8_0.gguf",
      "https://huggingface.co/CompendiumLabs/bge-small-en-v1.5-gguf/resolve/"
      "d32f8c040ea3b516330eeb75b72bcc2d3a780ab7/bge-small-en-v1.5-q8_0.gguf",
      "ec38e8da142596baa913124ae50550de284b6916bf59577ef2f0cb9660c2f514",
      "MIT (BAAI/bge-small-en-v1.5; GGUF by CompendiumLabs)", "embedding",
      /* bge v1.5 expects this instruction on the short "query" side (here: tag descriptions) */
      "Represent this sentence for searching relevant passages: ", 36806944 },
};
#define NMODELS (sizeof MODELS / sizeof *MODELS)
#define LLAMA_CPP_REPO "https://github.com/ggml-org/llama.cpp.git"
#define RUNTIME "llama-embedding"

static const struct model *model_named(const char *name)
{
    for (size_t i = 0; i < NMODELS; i++)
        if (!strcmp(MODELS[i].name, name)) return &MODELS[i];
    return NULL;
}

const char *model_query_prefix(const char *name)
{
    for (size_t i = 0; i < NMODELS; i++)            /* a server's model name may contain ours */
        if (strstr(name, MODELS[i].name)) return MODELS[i].query_prefix;
    return "";
}

static char *models_dir(const arv_home *h)
{
    home_ensure(h);
    return join(h->cache_dir, "models");
}

char *model_file(const arv_home *h, const char *name)
{
    const struct model *m = model_named(name);
    if (!m) return NULL;
    char *dir = models_dir(h), *path = join(dir, m->file);
    free(dir);
    return path;
}

char *find_runtime(const arv_home *h, const char *given)
{
    const char *env = getenv("ARCHIVE_LLAMA_EMBEDDING");
    char *inside = xprintf("%s/runtime/%s", h->cache_dir, RUNTIME);
    const char *cands[] = { given, env && *env ? env : NULL, inside };
    for (size_t i = 0; i < 3; i++)
        if (cands[i] && !access(cands[i], X_OK)) {
            char *found = xstrdup(cands[i]);
            free(inside);
            return found;
        }
    free(inside);
    const char *path = getenv("PATH");
    char *copy = xstrdup(path ? path : "/usr/bin:/bin");
    for (char *d = strtok(copy, ":"); d; d = strtok(NULL, ":")) {
        char *c = join(d, RUNTIME);
        if (!access(c, X_OK)) { free(copy); return c; }
        free(c);
    }
    free(copy);
    return NULL;
}

static int sha256_of(const char *path, char hex[65])
{
    uint64_t n;
    return hash_file(path, hex, -1, &n);
}

/* a model file put in place only when its checksum is the pinned one */
static char *install(const arv_home *h, const struct model *m, const char *source)
{
    char hex[65];
    if (sha256_of(source, hex)) die("cannot read %s", source);
    if (strcmp(hex, m->sha256))
        die2("%s does not have the expected checksum for this model (got %s); not installed", source, hex);
    char *dest = model_file(h, m->name), *dir = models_dir(h);
    if (mkdirs(dir)) die("cannot create %s", dir);
    copy_file(source, dest);
    free(dir);
    return dest;
}

static char *fetch(const arv_home *h, const struct model *m)
{
    if (!on_path("curl")) die("%s", "downloading a model needs curl (or arv models fetch --from FILE)");
    char *dir = models_dir(h), *part = xprintf("%s/%s.part", dir, m->file);
    if (mkdirs(dir)) die("cannot create %s", dir);
    fprintf(stderr, "Downloading %s (%.1f MB) ...\n", m->name, m->size / 1e6);
    char *argv[] = { "curl", "-fL", "--retry", "3", "-o", part, (char *)m->url, NULL }, *out = NULL;
    if (run(argv, &out)) die("download failed: %s", out ? out : "");
    free(out);
    char *dest = install(h, m, part);
    unlink(part);
    free(part);
    free(dir);
    return dest;
}

static char *build_runtime(const arv_home *h)
{
    if (!on_path("git") || !on_path("cmake")) die("%s", "building llama-embedding needs git and cmake (and a C++ compiler)");
    home_ensure(h);
    char *work = xprintf("%s/runtime/llama.cpp", h->cache_dir), *build = join(work, "build"), *out = NULL;
    struct stat st;
    if (stat(work, &st)) {
        char *clone[] = { "git", "clone", "--depth", "1", LLAMA_CPP_REPO, work, NULL };
        if (run(clone, &out)) die("git clone failed: %s", out);
        free(out);
    }
    /* GGML_NATIVE=OFF: a binary tuned for this exact CPU crashes ("Illegal instruction") on others */
    char *cfg[] = { "cmake", "-S", work, "-B", build, "-DGGML_NATIVE=OFF", "-DLLAMA_CURL=OFF", "-DCMAKE_BUILD_TYPE=Release", NULL };
    if (run(cfg, &out)) die("cmake failed: %s", out);
    free(out);
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    char jobs[24];
    snprintf(jobs, sizeof jobs, "%ld", cpus > 0 ? cpus : 2);
    char *make[] = { "cmake", "--build", build, "--target", RUNTIME, "-j", jobs, NULL };
    if (run(make, &out)) die("the build failed: %s", out);
    free(out);
    char *binary = xprintf("%s/bin/%s", build, RUNTIME), *target = xprintf("%s/runtime/%s", h->cache_dir, RUNTIME);
    unlink(target);
    copy_file(binary, target);          /* a copy, not a link: the home must survive being copied anywhere */
    chmod(target, 0755);
    free(binary);
    free(work);
    free(build);
    return target;
}

/* arv models fetch|status|build-runtime [--model NAME] [--from FILE] [--llama-embedding PATH] */
int assist_models(int argc, char **argv)
{
    const char *action = NULL, *name = "bge-small-en-v1.5", *from = NULL, *binary = NULL, *home = NULL;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--model") && v) { name = v; i++; }
        else if (!strcmp(a, "--from") && v) { from = v; i++; }
        else if (!strcmp(a, "--llama-embedding") && v) { binary = v; i++; }
        else if (!strcmp(a, "--home") && v) { home = v; i++; }
        else if (a[0] != '-' && !action) action = a;
        else return 2;
    }
    if (!action || (strcmp(action, "fetch") && strcmp(action, "status") && strcmp(action, "build-runtime"))) return 2;
    const struct model *m = model_named(name);
    if (!m) die("unknown model %s (known: bge-small-en-v1.5)", name);
    arv_home h;
    home_find(&h, home, NULL);
    if (!strcmp(action, "fetch")) {
        char *path = from ? install(&h, m, from) : fetch(&h, m);
        printf("%s ready: %s\n", m->name, path);
        free(path);
    } else if (!strcmp(action, "build-runtime")) {
        char *built = build_runtime(&h);
        printf("Built %s\n", built);
        free(built);
    }
    if (strcmp(action, "build-runtime")) {
        for (size_t i = 0; i < NMODELS; i++) {
            char *path = model_file(&h, MODELS[i].name);
            printf("%-20s %-10s %6.1f MB  %s  [%s]\n", MODELS[i].name, MODELS[i].kind, MODELS[i].size / 1e6,
                   access(path, R_OK) ? "not downloaded (arv models fetch)" : "downloaded", MODELS[i].licence);
            free(path);
        }
    }
    char *runtime = find_runtime(&h, binary);
    printf("%-20s %s\n", "runtime", runtime ? runtime : "llama-embedding not found (install llama.cpp, or arv models build-runtime)");
    free(runtime);
    return 0;
}
