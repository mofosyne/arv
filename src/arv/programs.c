/*
 * arv programs: which of the programs arv can use are here, what each is for, and what is missing
 * without it. None is needed to make, read, check or repair a disc: arv writes the image, its
 * error correction and its catalogue itself. The others add to it (a repository's history, format
 * ids, packed objects, clues for describing), or do the work arv leaves to tools built for it
 * (burning, reading a damaged disc, OCR), which arv never runs.
 *
 *   arv programs [--json]
 */
#define _XOPEN_SOURCE 700
#include "arv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char *name, *alt;     /* alt: another name for the same (open, on macOS) */
    const char *group, *use, *without, *debian, *arch;   /* packages; neither: see from() */
} program;

static const char *from(const program *p)   /* where one with no package comes from */
{
    if (!strcmp(p->name, "sf")) return "Siegfried: github.com/richardlehane/siegfried (a release, or its own .deb)";
    if (!strcmp(p->name, "places"))
        return "sh docs/examples/places-geonames.sh (in arv's source, or share/arv): writes ~/.local/share/arv/places/";
    return "make install in arv's source puts it beside arv";
}

static const program PROGRAMS[] = {
    { "arv-assist", NULL, "helpers", "describing and tagging: arv describe, arv tag", "no describing or tagging", NULL, NULL },
    { "arv-gui", NULL, "helpers", "the interface in a browser: arv gui", "the command line only", NULL, NULL },
    { "git", NULL, "making", "a repository's history kept on the disc (its .git, git.tsv); tools/ from a checkout",
      "repositories copied as plain files (the event says so)", "git", "git" },
    { "sf", NULL, "making", "format ids (PRONOM) for every file: Siegfried", "no format ids (as --no-formats)", NULL, NULL },
    { "tar", NULL, "making", "tools/ from a git checkout (git archive)", "tools/ from an installed arv only", "tar", "tar" },
    { "zstd", NULL, "making", "packed objects: tar+zstd, zstd (arv plan pack)", "those packings refused", "zstd", "zstd" },
    { "xz", NULL, "making", "packed objects: tar+xz, xz", "those packings refused", "xz-utils", "xz" },
    { "gzip", NULL, "making", "packed objects: tar+gzip, gzip", "those packings refused", "gzip", "gzip" },
    { "zip", NULL, "making", "packed objects: zip", "zip packing refused", "zip", "zip" },
    { "unzip", NULL, "making", "a zip packed object read back and restored", "zip objects not read back", "unzip", "unzip" },
    { "exiftool", NULL, "describing", "photos' dates, camera and GPS as clues", "dates from names and file times only, no camera or place",
      "libimage-exiftool-perl", "perl-image-exiftool" },
    { "pdftotext", NULL, "describing", "a PDF's words as clues", "PDFs' words not read", "poppler-utils", "poppler" },
    { "pdfinfo", NULL, "describing", "a PDF's own title, in the description", "PDFs' titles not read", "poppler-utils", "poppler" },
    { "ffmpeg", NULL, "describing", "a frame of a video, for a local vision model (describe --vision)", "videos not looked at", "ffmpeg", "ffmpeg" },
    { "xdg-open", "open", "interface", "arv gui opens your browser", "open the address arv gui prints", "xdg-utils", "xdg-utils" },
    { "xorriso", NULL, "beside", "burning images (arv never burns; see docs/burning.md)", "another burning program, or your system's own",
      "xorriso", "libisoburn" },
    { "ddrescue", NULL, "beside", "reading a damaged disc into an image for arv check --repair", "a damaged disc read another way",
      "gddrescue", "ddrescue" },
    { "ocrmypdf", NULL, "beside", "a text layer for scanned PDFs, before describing", "scans' words not read", "ocrmypdf", "ocrmypdf" },
    { "tesseract", NULL, "beside", "a .txt beside a scanned image, before describing", "scans' words not read", "tesseract-ocr", "tesseract" },
    { "iso-codes", NULL, "data", "regions (ISO 3166-2: states, prefectures, counties) as places, by name",
      "no regions but those you add", "iso-codes", "iso-codes" },
    { "places", NULL, "data", "more places: towns with positions (docs/examples/places-geonames.sh, from GeoNames)",
      "the built-in 1,251 cities and your own", NULL, NULL },
};

static const char *const GROUPS[][2] = {
    { "helpers", "arv's own helpers" },
    { "making", "Making discs (arv make, plan make)" },
    { "describing", "Describing (arv describe, arv tag)" },
    { "interface", "The interface" },
    { "beside", "Beside arv: work it leaves to tools built for it (it never runs them)" },
    { "data", "Place data (beside the built-in countries and cities)" },
};

/* where a program is on PATH, or NULL */
static char *path_of(const char *name)
{
    const char *path = getenv("PATH");
    if (!path || !name) return NULL;
    char *copy = xstrdup(path), *found = NULL;
    for (char *dir = strtok(copy, ":"); dir && !found; dir = strtok(NULL, ":")) {
        char *p = join(*dir ? dir : ".", name);
        if (!access(p, X_OK)) found = p;
        else free(p);
    }
    free(copy);
    return found;
}

static char *where_is(const program *p)
{
    if (!strcmp(p->name, "arv-assist")) return helper_program("ARV_ASSIST", "arv-assist", "arv-assist/build/arv-assist");
    if (!strcmp(p->name, "arv-gui")) return helper_program("ARV_GUI", "arv-gui", "arv-gui/build/arv-gui");
    if (!strcmp(p->name, "iso-codes") || !strcmp(p->name, "places")) {   /* data, not programs: files in the data folders */
        char *iso;
        strlist files;
        known_extra_files(&iso, &files);
        sbuf b = { 0 };
        for (size_t i = 0; i < files.n; i++) sb_printf(&b, "%s%s", i ? ", " : "", files.v[i]);
        strlist_free(&files);
        if (!strcmp(p->name, "iso-codes")) {
            free(b.s);
            return iso;
        }
        free(iso);
        return b.s;
    }
    char *at = path_of(p->name);
    return at ? at : path_of(p->alt);
}

static char *real_where(const program *p)
{
    if (!strcmp(p->name, "places") || !strcmp(p->name, "iso-codes")) return where_is(p);
    char *at = where_is(p), *real = at ? realpath(at, NULL) : NULL;
    if (real) free(at);
    return real ? real : at;
}

int cmd_programs(int argc, char **argv)
{
    int as_json = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) as_json = 1;
        else if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) i++;   /* the same for any home */
        else return 2;
    }
    size_t n = sizeof PROGRAMS / sizeof *PROGRAMS, found = 0;
    char **at = xmalloc(n * sizeof *at);
    for (size_t i = 0; i < n; i++) found += (at[i] = real_where(&PROGRAMS[i])) != NULL;
    sbuf b = { 0 };
    if (as_json) {
        sb_puts(&b, "{" JSON_FORMAT ", \"programs\": [");
        for (size_t i = 0; i < n; i++) {
            const program *p = &PROGRAMS[i];
            sb_puts(&b, i ? ",\n  {\"name\": " : "\n  {\"name\": ");
            json_str(&b, p->name);
            sb_puts(&b, ", \"group\": "); json_str(&b, p->group);
            sb_printf(&b, ", \"found\": %s, \"path\": ", at[i] ? "true" : "false");
            json_str(&b, at[i] ? at[i] : "");
            sb_puts(&b, ", \"for\": "); json_str(&b, p->use);
            sb_puts(&b, ", \"without\": "); json_str(&b, p->without);
            sb_puts(&b, ", \"debian\": "); json_str(&b, p->debian ? p->debian : "");
            sb_puts(&b, ", \"arch\": "); json_str(&b, p->arch ? p->arch : "");
            sb_puts(&b, ", \"from\": "); json_str(&b, p->debian ? "" : from(p));
            sb_puts(&b, "}");
        }
        sb_puts(&b, "\n]}\n");
    } else {
        sb_puts(&b, "None of these is needed to make, read, check or repair a disc: arv does that itself.\n");
        strlist deb = { 0 }, arch = { 0 }, helpers = { 0 };
        for (size_t g = 0; g < sizeof GROUPS / sizeof *GROUPS; g++) {
            sb_printf(&b, "\n%s\n", GROUPS[g][1]);
            for (size_t i = 0; i < n; i++) {
                const program *p = &PROGRAMS[i];
                if (strcmp(p->group, GROUPS[g][0])) continue;
                sb_printf(&b, "  %-8s %-11s %s\n", at[i] ? "ok" : "missing", p->name, p->use);
                if (at[i]) continue;
                sb_printf(&b, "  %-8s %-11s without it: %s\n", "", "", p->without);
                if (p->debian && !strlist_has(&deb, p->debian)) strlist_add(&deb, p->debian);
                if (p->arch && !strlist_has(&arch, p->arch)) strlist_add(&arch, p->arch);
                if (!p->debian) {
                    char *line = xprintf("%s: %s", p->name, from(p));
                    strlist_add(&helpers, line);
                    free(line);
                }
            }
        }
        sb_printf(&b, "\n%zu of %zu found.", found, n);
        if (deb.n) {
            sb_puts(&b, " The missing ones: sudo apt install");
            for (size_t i = 0; i < deb.n; i++) sb_printf(&b, " %s", deb.v[i]);
            sb_puts(&b, "\n  (Arch: sudo pacman -S");
            for (size_t i = 0; i < arch.n; i++) sb_printf(&b, " %s", arch.v[i]);
            sb_puts(&b, ")");
        }
        for (size_t i = 0; i < helpers.n; i++) sb_printf(&b, "\n  %s", helpers.v[i]);
        sb_puts(&b, "\n");
        strlist_free(&deb);
        strlist_free(&arch);
        strlist_free(&helpers);
    }
    fputs(b.s, stdout);
    free(b.s);
    for (size_t i = 0; i < n; i++) free(at[i]);
    free(at);
    return 0;
}
