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
#include <sys/utsname.h>
#include <unistd.h>

typedef struct {
    const char *name, *alt;     /* alt: another name for the same (open, on macOS) */
    const char *group, *use, *without;
    const char *pkg[4];         /* its package on each system (SYSTEMS); NULL: none (see from()), "": built in there */
} program;

/* the systems whose package names arv knows, and how each installs */
static const char *const SYSTEMS[][3] = {
    { "debian", "Debian, Ubuntu", "sudo apt install" },
    { "fedora", "Fedora", "sudo dnf install" },
    { "arch", "Arch", "sudo pacman -S" },
    { "macos", "macOS (Homebrew)", "brew install" },
};
#define NSYSTEMS (sizeof SYSTEMS / sizeof *SYSTEMS)

static const char *from(const program *p)   /* where one with no package comes from */
{
    if (!strcmp(p->name, "sf")) return "Siegfried: github.com/richardlehane/siegfried (a release, or its own .deb)";
    if (!strcmp(p->name, "places"))
        return "sh docs/examples/places-geonames.sh (in arv's source, or share/arv): writes ~/.local/share/arv/places/";
    return "make install in arv's source puts it beside arv";
}

#define SAME(x) { x, x, x, x }
static const program PROGRAMS[] = {
    { "arv-assist", NULL, "helpers", "describing and tagging: arv describe, arv tag", "no describing or tagging", { 0 } },
    { "arv-gui", NULL, "helpers", "the interface in a browser: arv gui", "the command line only", { 0 } },
    { "git", NULL, "making", "a repository's history kept on the disc (its .git, git.tsv); tools/ from a checkout",
      "repositories copied as plain files (the event says so)", SAME("git") },
    { "sf", NULL, "making", "format ids (PRONOM) for every file: Siegfried", "no format ids (as --no-formats)", { 0 } },
    { "tar", NULL, "making", "tools/ from a git checkout (git archive)", "tools/ from an installed arv only", { "tar", "tar", "tar", "" } },
    { "zstd", NULL, "making", "packed objects: tar+zstd, zstd (arv plan pack)", "those packings refused", SAME("zstd") },
    { "xz", NULL, "making", "packed objects: tar+xz, xz", "those packings refused", { "xz-utils", "xz", "xz", "xz" } },
    { "gzip", NULL, "making", "packed objects: tar+gzip, gzip", "those packings refused", { "gzip", "gzip", "gzip", "" } },
    { "zip", NULL, "making", "packed objects: zip", "zip packing refused", { "zip", "zip", "zip", "" } },
    { "unzip", NULL, "making", "a zip packed object read back and restored", "zip objects not read back", { "unzip", "unzip", "unzip", "" } },
    { "exiftool", NULL, "describing", "photos' dates, camera and GPS as clues", "dates from names and file times only, no camera or place",
      { "libimage-exiftool-perl", "perl-Image-ExifTool", "perl-image-exiftool", "exiftool" } },
    { "pdftotext", NULL, "describing", "a PDF's words as clues", "PDFs' words not read", { "poppler-utils", "poppler-utils", "poppler", "poppler" } },
    { "pdfinfo", NULL, "describing", "a PDF's own title, in the description", "PDFs' titles not read",
      { "poppler-utils", "poppler-utils", "poppler", "poppler" } },
    { "ffmpeg", NULL, "describing", "a frame of a video, for a local vision model (describe --vision)", "videos not looked at",
      { "ffmpeg", "ffmpeg-free", "ffmpeg", "ffmpeg" } },
    { "curl", NULL, "describing", "https to an external inference provider you choose, with your own key (Settings)",
      "model servers on this computer only", { "curl", "curl", "curl", "" } },
    { "xdg-open", "open", "interface", "arv gui opens your browser", "open the address arv gui prints", { "xdg-utils", "xdg-utils", "xdg-utils", "" } },
    { "xorriso", NULL, "beside", "burning images (arv never burns; see docs/burning.md)", "another burning program, or your system's own",
      { "xorriso", "xorriso", "libisoburn", "xorriso" } },
    { "ddrescue", NULL, "beside", "reading a damaged disc into an image for arv check --repair", "a damaged disc read another way",
      { "gddrescue", "ddrescue", "ddrescue", "ddrescue" } },
    { "ocrmypdf", NULL, "beside", "a text layer for scanned PDFs, before describing", "scans' words not read", SAME("ocrmypdf") },
    { "tesseract", NULL, "beside", "a .txt beside a scanned image, before describing", "scans' words not read",
      { "tesseract-ocr", "tesseract", "tesseract", "tesseract" } },
    { "iso-codes", NULL, "data", "regions (ISO 3166-2: states, prefectures, counties) as places, by name",
      "no regions but those you add", SAME("iso-codes") },
    { "places", NULL, "data", "more places: towns with positions (docs/examples/places-geonames.sh, from GeoNames)",
      "the built-in 1,251 cities and your own", { 0 } },
};

/* this computer's system, by uname and os-release's ID and ID_LIKE ($ARV_OS_RELEASE: another file,
 * for tests): an index into SYSTEMS, or -1 when arv knows none of them */
static int this_system(char **pretty)
{
    struct utsname u;
    *pretty = NULL;
    if (!uname(&u) && !strcmp(u.sysname, "Darwin")) {
        *pretty = xstrdup("macOS");
        return 3;
    }
    const char *path = getenv("ARV_OS_RELEASE") && *getenv("ARV_OS_RELEASE") ? getenv("ARV_OS_RELEASE") : "/etc/os-release";
    char *text = read_text(path);
    if (!text) text = read_text("/usr/lib/os-release");
    if (!text) return -1;
    sbuf ids = { 0 };
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *v = strchr(line, '=');
        if (!v) continue;
        *v++ = 0;
        if (*v == '"') {
            v++;
            if (*v && v[strlen(v) - 1] == '"') v[strlen(v) - 1] = 0;
        }
        if (!strcmp(line, "ID") || !strcmp(line, "ID_LIKE")) sb_printf(&ids, " %s ", v);
        if (!strcmp(line, "PRETTY_NAME")) *pretty = xstrdup(v);
    }
    free(text);
    static const char *const like[][2] = { { "debian", "0" }, { "ubuntu", "0" }, { "fedora", "1" }, { "rhel", "1" },
                                           { "centos", "1" }, { "arch", "2" }, { "manjaro", "2" } };
    int found = -1;
    for (size_t i = 0; ids.s && found < 0 && i < sizeof like / sizeof *like; i++) {
        char *word = xprintf(" %s ", like[i][0]);
        if (strstr(ids.s, word)) found = like[i][1][0] - '0';
        free(word);
    }
    free(ids.s);
    return found;
}

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

/* "sudo apt install a b" for the missing programs of one group (NULL: all), on system k; NULL if none */
static char *install_line(size_t k, const char *group, char **at, size_t n)
{
    strlist pk = { 0 };
    for (size_t i = 0; i < n; i++) {
        const program *p = &PROGRAMS[i];
        if (at[i] || (group && strcmp(p->group, group)) || !p->pkg[k] || !*p->pkg[k] || strlist_has(&pk, p->pkg[k])) continue;
        strlist_add(&pk, p->pkg[k]);
    }
    if (!pk.n) return NULL;
    sbuf b = { 0 };
    sb_puts(&b, SYSTEMS[k][2]);
    for (size_t i = 0; i < pk.n; i++) sb_printf(&b, " %s", pk.v[i]);
    strlist_free(&pk);
    return b.s;
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
    char **at = xmalloc(n * sizeof *at), *pretty = NULL;
    for (size_t i = 0; i < n; i++) found += (at[i] = real_where(&PROGRAMS[i])) != NULL;
    int sys = this_system(&pretty);
    sbuf b = { 0 };
    if (as_json) {
        sb_puts(&b, "{" JSON_FORMAT ", \"system\": ");
        if (sys >= 0) {
            sb_puts(&b, "{\"id\": "); json_str(&b, SYSTEMS[sys][0]);
            sb_puts(&b, ", \"name\": "); json_str(&b, pretty ? pretty : SYSTEMS[sys][1]);
            sb_puts(&b, "}");
        } else {
            sb_puts(&b, "null");
        }
        sb_puts(&b, ", \"systems\": [");
        for (size_t k = 0; k < NSYSTEMS; k++) {
            sb_puts(&b, k ? ", {\"id\": " : "{\"id\": "); json_str(&b, SYSTEMS[k][0]);
            sb_puts(&b, ", \"name\": "); json_str(&b, SYSTEMS[k][1]);
            sb_puts(&b, ", \"install\": "); json_str(&b, SYSTEMS[k][2]);
            sb_puts(&b, "}");
        }
        sb_puts(&b, "], \"programs\": [");
        for (size_t i = 0; i < n; i++) {
            const program *p = &PROGRAMS[i];
            sb_puts(&b, i ? ",\n  {\"name\": " : "\n  {\"name\": ");
            json_str(&b, p->name);
            sb_puts(&b, ", \"group\": "); json_str(&b, p->group);
            sb_printf(&b, ", \"found\": %s, \"path\": ", at[i] ? "true" : "false");
            json_str(&b, at[i] ? at[i] : "");
            sb_puts(&b, ", \"for\": "); json_str(&b, p->use);
            sb_puts(&b, ", \"without\": "); json_str(&b, p->without);
            sb_puts(&b, ", \"packages\": {");     /* "" where it is built in */
            int first = 1;
            for (size_t k = 0; k < NSYSTEMS; k++)
                if (p->pkg[k]) {
                    sb_puts(&b, first ? "" : ", ");
                    json_str(&b, SYSTEMS[k][0]);
                    sb_puts(&b, ": ");
                    json_str(&b, p->pkg[k]);
                    first = 0;
                }
            sb_puts(&b, "}, \"from\": "); json_str(&b, p->pkg[0] ? "" : from(p));
            sb_puts(&b, "}");
        }
        sb_puts(&b, "\n]}\n");
    } else {
        sb_printf(&b, "None of these is needed to make, read, check or repair a disc: arv does that itself.\n"
                      "This system: %s.\n", sys >= 0 ? (pretty ? pretty : SYSTEMS[sys][1]) : "not one arv knows the packages of");
        for (size_t g = 0; g < sizeof GROUPS / sizeof *GROUPS; g++) {
            sb_printf(&b, "\n%s\n", GROUPS[g][1]);
            for (size_t i = 0; i < n; i++) {
                const program *p = &PROGRAMS[i];
                if (strcmp(p->group, GROUPS[g][0])) continue;
                sb_printf(&b, "  %-8s %-11s %s\n", at[i] ? "ok" : "missing", p->name, p->use);
                if (at[i]) continue;
                sb_printf(&b, "  %-8s %-11s without it: %s\n", "", "", p->without);
            }
            for (size_t k = 0; k < NSYSTEMS; k++) {         /* this system's command, or every system's */
                if (sys >= 0 && (int)k != sys) continue;
                char *line = install_line(k, GROUPS[g][0], at, n);
                if (line) sb_printf(&b, "  to install%s%s: %s\n", sys >= 0 ? "" : ", on ", sys >= 0 ? "" : SYSTEMS[k][1], line);
                free(line);
            }
            for (size_t i = 0; i < n; i++)
                if (!at[i] && !PROGRAMS[i].pkg[0] && !strcmp(PROGRAMS[i].group, GROUPS[g][0]))
                    sb_printf(&b, "  %s: %s\n", PROGRAMS[i].name, from(&PROGRAMS[i]));
        }
        sb_printf(&b, "\n%zu of %zu found.", found, n);
        for (size_t k = 0; k < NSYSTEMS; k++) {
            if (sys >= 0 && (int)k != sys) continue;
            char *line = install_line(k, NULL, at, n);
            if (line) sb_printf(&b, "\n  Every missing one%s%s: %s", sys >= 0 ? "" : ", on ", sys >= 0 ? "" : SYSTEMS[k][1], line);
            free(line);
        }
        sb_puts(&b, "\n");
    }
    fputs(b.s, stdout);
    free(b.s);
    free(pretty);
    for (size_t i = 0; i < n; i++) free(at[i]);
    free(at);
    return 0;
}
