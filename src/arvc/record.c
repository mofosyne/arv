/* Keeping the record after a disc is made (src/arv/cli.py): check, burned, note, locate.
 * Every change appends an Event, as the Python arv does. */
#define _XOPEN_SOURCE 700
#include "arvc.h"
#include "../rs03/rs03.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    arv_home h;
    archive cat;
} opened;

static rec_record *open_disc_record(opened *o, const char *given, const char *disc_id)
{
    home_find(&o->h, given, NULL);
    archive_load(&o->cat, o->h.rec_path);
    rec_record *d = archive_disc(&o->cat, disc_id);
    if (!d) die2("no disc %s in %s", disc_id, o->h.rec_path);
    return d;
}

/* every hand edit leaves a 'metadata modification' event */
static void metadata_change(archive *cat, const char *disc_id, const char *note)
{
    char *who = person();
    recs_add(&cat->events, new_event(disc_id, "metadata modification", "success", who, "human", note));
    free(who);
}

static int field_count(const rec_record *r, const char *name)
{
    int n = 0;
    for (size_t i = 0; i < r->nfields; i++) n += !strcmp(r->fields[i].name, name);
    return n;
}

/* ------------------------------------------------------------------ the volume label */

/* the label of an image or drive: the ISO 9660 primary volume descriptor, else UDF's logical
 * volume identifier (src/arv/image.py read_volume_label) */
static char *read_volume_label(const char *path)
{
    FILE *fp = fopen(path, "rb");
    unsigned char s[2048];
    char *out = NULL;
    if (!fp) return NULL;
    if (!fseeko(fp, 16 * 2048, SEEK_SET) && fread(s, 1, 2048, fp) == 2048 && !memcmp(s + 1, "CD001", 5)) {
        out = xmalloc(33);
        memcpy(out, s + 40, 32);
        out[32] = 0;
    } else if (!fseeko(fp, 256 * 2048, SEEK_SET) && fread(s, 1, 2048, fp) == 2048 && (s[0] | s[1] << 8) == 2) {
        unsigned long length = s[16] | s[17] << 8 | (unsigned long)s[18] << 16 | (unsigned long)s[19] << 24;
        unsigned long start = s[20] | s[21] << 8 | (unsigned long)s[22] << 16 | (unsigned long)s[23] << 24;
        for (unsigned long i = 0; i < length / 2048 && i < 64 && !out; i++) {
            if (fseeko(fp, (off_t)(start + i) * 2048, SEEK_SET) || fread(s, 1, 2048, fp) != 2048) break;
            int tag = s[0] | s[1] << 8;
            if (tag == 8) break;
            if (tag != 6) continue;
            const unsigned char *id = s + 84;           /* dstring: compression id, characters, length */
            int n = id[127];
            sbuf b = { 0 };
            if (id[0] == 8)
                for (int k = 1; k < n; k++) {
                    unsigned c = id[k];
                    char u[3] = { (char)(c < 0x80 ? c : 0xC0 | c >> 6), (char)(0x80 | (c & 0x3F)), 0 };
                    sb_add(&b, u, c < 0x80 ? 1 : 2);
                }
            else if (id[0] == 16)
                for (int k = 1; k + 1 < n; k += 2) {
                    unsigned c = (unsigned)(id[k] << 8 | id[k + 1]);
                    char u[3];
                    if (c < 0x80) { u[0] = (char)c; sb_add(&b, u, 1); }
                    else if (c < 0x800) { u[0] = (char)(0xC0 | c >> 6); u[1] = (char)(0x80 | (c & 0x3F)); sb_add(&b, u, 2); }
                    else { char w[3] = { (char)(0xE0 | c >> 12), (char)(0x80 | (c >> 6 & 0x3F)), (char)(0x80 | (c & 0x3F)) }; sb_add(&b, w, 3); }
                }
            out = b.s ? b.s : xstrdup("");
        }
    }
    fclose(fp);
    return out;
}

/* ------------------------------------------------------------------ check */

/* the last few meaningful lines of dvdisaster's output, for the event note */
static char *summary(const char *output, int lines)
{
    static const char *const skip[] = { "Copyright", "This software", "is free", "under the", "See the file", "dvdisaster ", NULL };
    char *copy = xstrdup(output), *kept[4096];
    int n = 0;
    for (char *l = strtok(copy, "\n"); l && n < 4096; l = strtok(NULL, "\n")) {
        int skipped = 0;
        for (int k = 0; skip[k]; k++) skipped |= !strncmp(l, skip[k], strlen(skip[k]));
        while (isspace((unsigned char)*l)) l++;
        char *e = l + strlen(l);
        while (e > l && isspace((unsigned char)e[-1])) *--e = 0;
        if (!skipped && *l) kept[n++] = l;
    }
    sbuf b = { 0 };
    sb_puts(&b, "");
    for (int i = n > lines ? n - lines : 0; i < n; i++) sb_printf(&b, "%s%s", b.len ? "\n" : "", kept[i]);
    free(copy);
    return b.s;
}

int cmd_check(int argc, char **argv)
{
    const char *given = NULL, *device = NULL, *image = NULL, *disc_id = NULL, *note = NULL;
    int verbose = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) verbose = 1;
        else if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--device")) device = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--image")) image = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--note")) note = argv[++i];
        else if (argv[i][0] != '-' && !disc_id) disc_id = argv[i];
        else return 2;
    }
    if (!device == !image) return 2;
    if (device && !on_path("dvdisaster")) die("%s", "reading a disc needs dvdisaster Light on PATH (or check an image with --image)");
    const char *source = device ? device : image;
    if (access(source, F_OK)) die("%s does not exist", source);
    char *from_label = NULL;
    if (!disc_id) {
        char *label = read_volume_label(source);
        char *word = label ? strtok(label, " \t") : NULL;
        if (!word) die("no volume label on %s; pass the disc id explicitly", source);
        disc_id = from_label = xstrdup(word);
        free(label);
    }
    opened o;
    home_find(&o.h, given, NULL);
    archive_load(&o.cat, o.h.rec_path);
    if (!archive_disc(&o.cat, disc_id)) die("disc %s is not in the catalogue", disc_id);
    fprintf(stderr, "Checking %s (%s) ...\n", disc_id, source);
    char *output = NULL, *what, *agent;
    int ok;
    if (device) {
        char *a[] = { "dvdisaster", "-d", (char *)device, "-s", "--no-progress", NULL };
        ok = run(a, &output) == 0;
        what = xprintf("disc scan with dvdisaster -s on %s", device);
        agent = xstrdup("dvdisaster");
    } else {                         /* every sector: data against its CRC, parity against the data */
        rs03_report r;
        char err[512], line[200];
        if (rs03_verify(image, &r, err, sizeof err)) {
            output = xprintf("%s", err);
            ok = 0;
        } else {
            rs03_describe(&r.lay, line, sizeof line);
            ok = r.header_ok && !r.bad_data && !r.bad_crc && !r.bad_ecc;
            output = xprintf("RS03: %s\nheader %s; %llu data sectors with a wrong CRC, %llu CRC sectors and %llu "
                             "parity sectors damaged\n%s", line, r.header_ok ? "good" : "DAMAGED",
                             (unsigned long long)r.bad_data, (unsigned long long)r.bad_crc, (unsigned long long)r.bad_ecc,
                             ok ? "the image is whole" : "repair it with dvdisaster Light: dvdisaster -i IMAGE -f");
        }
        what = xstrdup("image test (RS03: every sector against its CRC, the parity against the data)");
        agent = xstrdup(VERSION);
    }
    char *sum = summary(output, 6), *text = note ? xprintf("%s\n%s\n%s", what, sum, note) : xprintf("%s\n%s", what, sum);
    recs_add(&o.cat.events, new_event(disc_id, "fixity check", ok ? "success" : "failure", agent, "automatic", text));
    archive_save(&o.cat, o.h.rec_path);
    printf("%s\n", !ok || verbose ? output : sum);
    printf("%s: %s\n", disc_id, ok ? "OK" : "FAILED - see output above");
    free(from_label);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ burned, note, locate */

int cmd_burned(int argc, char **argv)
{
    const char *given = NULL, *disc_id = NULL, *media_id = NULL, *location = NULL, *note = NULL;
    long copies = 1;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--copies")) copies = atol(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--media-id")) media_id = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--location")) location = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--note")) note = argv[++i];
        else if (argv[i][0] != '-' && !disc_id) disc_id = argv[i];
        else return 2;
    }
    if (!disc_id) return 2;
    opened o;
    rec_record *d = open_disc_record(&o, given, disc_id);
    char *count = xprintf("%ld", atol(rec_get(d, "Copies") ? rec_get(d, "Copies") : "0") + copies);
    rec_set(d, "Copies", count);
    if (media_id) rec_add(d, "MediaId", media_id);
    sbuf text = { 0 };
    sb_printf(&text, "burned %ld cop%s", copies, copies == 1 ? "y" : "ies");
    if (location) {
        char *where = place(&o.cat, location);
        int have = 0;
        for (size_t i = 0; i < d->nfields; i++) have |= !strcmp(d->fields[i].name, "Location") && !strcmp(d->fields[i].value, where);
        if (!have) rec_add(d, "Location", where);
        rec_record probe = { 0 };
        probe.type = "Disc";
        rec_add(&probe, "Location", where);
        char *path = archive_where(&o.cat, &probe);
        sb_printf(&text, ", kept at %s", path);
        free(path);
        rec_clear(&probe);
        free(where);
    }
    if (note) sb_printf(&text, "; %s", note);
    char *who = person();
    recs_add(&o.cat.events, new_event(disc_id, "replication", "success", who, "human", text.s));
    archive_save(&o.cat, o.h.rec_path);
    printf("%s: %s copies recorded\n", disc_id, count);
    free(who);
    free(count);
    free(text.s);
    return 0;
}

/* " ".join(text.split()), at most width characters ("..." when cut) */
static char *clip(const char *text, size_t width)
{
    sbuf b = { 0 };
    char *copy = xstrdup(text);
    for (char *w = strtok(copy, " \t\n\r"); w; w = strtok(NULL, " \t\n\r")) sb_printf(&b, "%s%s", b.len ? " " : "", w);
    free(copy);
    if (!b.s) return xstrdup("");
    if (utf8_chars(b.s) <= width) return b.s;
    const char *s = b.s;
    for (size_t n = 0; n < width - 3; n++) utf8_next(&s);
    char *out = xprintf("%.*s...", (int)(s - b.s), b.s);
    free(b.s);
    return out;
}

int cmd_note(int argc, char **argv)
{
    const char *given = NULL, *disc_id = NULL, *text = NULL;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!disc_id) disc_id = argv[i];
        else if (!text) text = argv[i];
        else return 2;
    }
    if (!text) return 2;
    opened o;
    rec_record *d = open_disc_record(&o, given, disc_id);
    rec_add(d, "Note", text);
    char *c = clip(text, 60), *msg = xprintf("Note added: %s", c);
    metadata_change(&o.cat, disc_id, msg);
    archive_save(&o.cat, o.h.rec_path);
    free(c);
    free(msg);
    return 0;
}

int cmd_locate(int argc, char **argv)
{
    const char *given = NULL, *disc_id = NULL;
    int add = 0;
    strlist places = { 0 };
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (!strcmp(argv[i], "--add")) add = 1;
        else if (!disc_id) disc_id = argv[i];
        else strlist_add(&places, argv[i]);
    }
    if (!disc_id || !places.n) return 2;
    opened o;
    rec_record *d = open_disc_record(&o, given, disc_id);
    strlist old_all = { 0 }, keep = { 0 };
    for (size_t i = 0; i < d->nfields; i++)
        if (!strcmp(d->fields[i].name, "Location")) strlist_add(&old_all, d->fields[i].value);
    if (add) for (size_t i = 0; i < old_all.n; i++) if (*old_all.v[i]) strlist_add(&keep, old_all.v[i]);
    for (size_t i = 0; i < places.n; i++) {
        char *p = place(&o.cat, places.v[i]);
        if (!archive_location(&o.cat, p))
            fprintf(stderr, "Note: %s is not a location code ('arv location add' to define it); stored as text\n", p);
        if (*p && !strlist_has(&keep, p)) strlist_add(&keep, p);
        free(p);
    }
    /* the new Location fields go where the first old one was (or at the end) */
    rec_record copy = { 0 };
    copy.type = "Disc";
    size_t first = d->nfields;
    for (size_t i = 0; i < d->nfields; i++) if (!strcmp(d->fields[i].name, "Location")) { first = i; break; }
    for (size_t i = 0; i < first; i++) rec_add(&copy, d->fields[i].name, d->fields[i].value);
    for (size_t i = 0; i < keep.n; i++) rec_add(&copy, "Location", keep.v[i]);
    for (size_t i = first; i < d->nfields; i++)
        if (strcmp(d->fields[i].name, "Location")) rec_add(&copy, d->fields[i].name, d->fields[i].value);
    rec_clear(d);
    rec_copy(d, &copy);
    rec_clear(&copy);
    int same = (size_t)field_count(d, "Location") == old_all.n;
    for (size_t i = 0, k = 0; same && i < d->nfields; i++)
        if (!strcmp(d->fields[i].name, "Location")) same = !strcmp(d->fields[i].value, old_all.v[k++]);
    if (!same) {
        sbuf msg = { 0 };
        sb_puts(&msg, "Location: ");
        if (!old_all.n) sb_puts(&msg, "(none)");
        for (size_t i = 0; i < old_all.n; i++) sb_printf(&msg, "%s%s", i ? ", " : "", old_all.v[i]);
        sb_puts(&msg, " -> ");
        int any = 0;
        for (size_t i = 0; i < d->nfields; i++)
            if (!strcmp(d->fields[i].name, "Location")) sb_printf(&msg, "%s%s", any++ ? ", " : "", d->fields[i].value);
        metadata_change(&o.cat, disc_id, msg.s);
        free(msg.s);
    }
    archive_save(&o.cat, o.h.rec_path);
    char *w = archive_where(&o.cat, d);
    printf("%s: %s\n", disc_id, w);
    free(w);
    return 0;
}
