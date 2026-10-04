/* Keeping the record after a disc is made (src/arv/cli.py): check, burned, note, locate.
 * Every change appends an Event, as the Python arv does. */
#define _XOPEN_SOURCE 700
#include "arv.h"
#include "../rs03/rs03.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

/* RS03's test of an image: every sector against its CRC, the parity against the data. 1 = whole. */
static int image_test(const char *image, sbuf *out)
{
    rs03_report r;
    char err[512], line[200];
    if (rs03_verify(image, &r, err, sizeof err)) {
        sb_printf(out, "%s\n", err);
        return 0;
    }
    rs03_describe(&r.lay, line, sizeof line);
    int ok = r.header_ok && !r.bad_data && !r.bad_crc && !r.bad_ecc;
    sb_printf(out, "RS03: %s\nheader %s; %llu data sectors with a wrong CRC, %llu CRC sectors and %llu parity sectors damaged\n",
              line, r.header_ok ? "good" : "DAMAGED", (unsigned long long)r.bad_data, (unsigned long long)r.bad_crc,
              (unsigned long long)r.bad_ecc);
    return ok;
}

/* RS03 repair in place, then the test again. 1 = whole now. *lost: no RS03 layout was found;
   *medium: the medium size of the layout it found (0: none) */
static int image_repair(const char *image, sbuf *out, int *lost, uint64_t *medium)
{
    rs03_repair_report f;
    char err[512];
    int rc = rs03_repair(image, &f, err, sizeof err);
    *lost = rc == -2;
    *medium = rc == -1 || rc == -2 ? 0 : f.lay.medium_sectors;
    if (rc) {
        if (!out->s || !strstr(out->s, err)) sb_printf(out, "repair: %s\n", err);   /* the test may have said it */
        return 0;
    }
    if (f.missing) sb_printf(out, "%llu sectors were missing from the end of the image\n", (unsigned long long)f.missing);
    sb_printf(out, "repaired: %llu data sectors, %llu CRC sectors, %llu parity sectors\n", (unsigned long long)f.repaired_data,
              (unsigned long long)f.repaired_crc, (unsigned long long)f.repaired_ecc);
    if (f.unrepaired_positions)
        sb_printf(out, "NOT repaired: %llu sectors at %llu positions have more damage than the error correction can mend\n",
                  (unsigned long long)f.unrepaired_sectors, (unsigned long long)f.unrepaired_positions);
    return image_test(image, out);
}

/* 'text', quoted for a POSIX shell */
static char *shell_quote(const char *text)
{
    sbuf q = { 0 };
    sb_puts(&q, "'");
    for (const char *c = text; *c; c++) {
        if (*c == '\'') sb_puts(&q, "'\\''");
        else sb_printf(&q, "%c", *c);
    }
    sb_puts(&q, "'");
    return q.s;
}

/* What to do when arv could not repair an image: the commands to paste, with the medium size that
   tells dvdisaster Light where the layers are (0: not known) */
static void hand_over(FILE *to, const char *image, int lost, uint64_t medium)
{
    char *q = shell_quote(image), *map = xprintf("%s.map", image), *qm = shell_quote(map);
    char *n = medium ? xprintf(" -n %llu", (unsigned long long)medium) : xstrdup("");
    if (lost) {
        fprintf(to, "\narv found nothing to repair with: the RS03 header and the start of the CRC layer are damaged,\n"
                    "or this is not an augmented image. dvdisaster Light searches the whole image for them%s:\n\n",
                medium ? "; the medium size tells it where the layers are" : "");
        fprintf(to, "    dvdisaster -i %s -f%s\n    dvdisaster -i %s -t%s\n\n", q, n, q, n);
    } else {
        fprintf(to, "\nSome damage is beyond the error correction: too many sectors lost at the same positions.\n"
                    "Read more of the disc into the same image (only the missing sectors are read): another copy\n"
                    "of this disc, or this one again with dvdisaster Light, which retries hard and repairs as it reads\n"
                    "(both keep the same ddrescue map file, so each reads only what is still missing):\n\n"
                    "    ddrescue -b 2048 /dev/sr0 %s %s\n"
                    "    dvdisaster -d /dev/sr0 -r --rescue --ignore-iso-size --mapfile %s -i %s%s\n\n"
                    "then repair again:\n\n"
                    "    arv check --image %s --repair\n\n", q, qm, qm, q, n, q);
    }
    if (!medium)
        fputs("(Add -n with the disc's medium size, if you can find it: MediumSectors in its Binding record in a\n"
              "catalogue, or the -n the disc's README.txt gives.)\n", to);
    fputs("dvdisaster Light: https://github.com/teaching-droid/dvdisaster-light. Its -f exits with 1 even after a\n"
          "good repair; -t says whether the image is whole.\n", to);
    free(q); free(map); free(qm); free(n);
}

/* A field of the disc's Binding record (NULL: none) */
static const char *binding_field(const archive *cat, const char *disc_id, const char *name)
{
    for (size_t i = 0; i < cat->bindings.n; i++) {
        const char *vol = rec_get(cat->bindings.v[i], "Volume");
        if (vol && disc_id && !strcmp(vol, disc_id)) return rec_get(cat->bindings.v[i], name);
    }
    return NULL;
}

/* The image's size and SHA-256 at creation, from the disc's Binding (NULL: not recorded) */
static const char *image_hash(const archive *cat, const char *disc_id, uint64_t *sectors)
{
    for (size_t i = 0; i < cat->bindings.n; i++) {
        const rec_record *b = cat->bindings.v[i];
        const char *vol = rec_get(b, "Volume"), *sha = rec_get(b, "ImageSha256"), *n = rec_get(b, "ImageSectors");
        if (vol && sha && n && !strcmp(vol, disc_id)) {
            *sectors = strtoull(n, NULL, 10);
            return sha;
        }
    }
    return NULL;
}

/* Reads the first `sectors` sectors of a drive (or file) and hashes them, after asking the
   system to drop what it has cached of it, so that the disc itself is read. 0, or -1 with *out
   saying what went wrong. */
static int read_back(const char *dev, uint64_t sectors, char hex[65], sbuf *out)
{
    enum { SECTOR = 2048, RUN = 512 };  /* sectors per read: 1 MiB */
    static unsigned char buf[RUN * SECTOR];
    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        sb_printf(out, "cannot open %s: %s\n", dev, strerror(errno));
        return -1;
    }
    (void)posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    sha256_ctx c;
    unsigned char digest[32];
    sha256_init(&c);
    int tty = isatty(2), last = -1;
    for (uint64_t at = 0; at < sectors;) {
        size_t want = (size_t)(sectors - at < RUN ? sectors - at : RUN) * SECTOR, got = 0;
        while (got < want) {
            size_t n = want - got < sizeof buf - got ? want - got : sizeof buf - got;
            ssize_t r = pread(fd, buf + got, n, (off_t)(at * SECTOR + got));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) {
                if (r < 0) sb_printf(out, "unreadable near sector %llu of %llu: %s\n", (unsigned long long)(at + got / SECTOR),
                                     (unsigned long long)sectors, strerror(errno));
                else sb_printf(out, "the disc ends at sector %llu; the image has %llu\n", (unsigned long long)(at + got / SECTOR),
                               (unsigned long long)sectors);
                close(fd);
                return -1;
            }
            got += (size_t)r;
        }
        sha256_update(&c, buf, want);
        at += want / SECTOR;
        int pc = (int)(at * 100 / sectors);
        if (tty && pc != last) fprintf(stderr, "\r  %3d%%", last = pc);
    }
    if (tty) fputs("\r      \r", stderr);
    (void)posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd);
    sha256_final(&c, digest);
    sha256_hex(digest, hex);
    return 0;
}

int cmd_check(int argc, char **argv)
{
    const char *given = NULL, *device = NULL, *image = NULL, *disc_id = NULL, *note = NULL;
    int verbose = 0, repair = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--repair")) repair = 1;
        else if (i + 1 < argc && (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--home"))) given = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--device")) device = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--image")) image = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--note")) note = argv[++i];
        else if (argv[i][0] != '-' && !disc_id) disc_id = argv[i];
        else return 2;
    }
    if (!device == !image) return 2;
    if (device && repair) die("%s", "read the disc into an image first (see the disc's README.txt, REPAIR), then: arv check --image IMAGE --repair");
    const char *source = device ? device : image;
    if (access(source, F_OK)) die("%s does not exist", source);
    /* repairing first: a damaged image may not even give its label */
    sbuf out = { 0 };
    int ok = 0, was_whole = 0, lost = 0;
    uint64_t found_medium = 0;
    if (repair) {
        fprintf(stderr, "Testing and repairing %s ...\n", image);
        was_whole = ok = image_test(image, &out);
        if (!ok) ok = image_repair(image, &out, &lost, &found_medium);
        sb_puts(&out, !was_whole && ok ? "the image was damaged and is whole again\n" : ok ? "the image is whole\n"
                                                                                      : "the image is still damaged\n");
    }
    char *from_label = NULL;
    if (!disc_id) {
        char *label = read_volume_label(source);
        char *word = label ? strtok(label, " \t") : NULL;
        if (word) disc_id = from_label = xstrdup(word);
        else if (!repair) die("no volume label on %s; pass the disc id explicitly", source);
        free(label);
    }
    opened o;
    home_find(&o.h, given, NULL);
    archive_load(&o.cat, o.h.rec_path);
    int logged = disc_id && archive_disc(&o.cat, disc_id);
    if (!logged && !repair) die("disc %s is not in the catalogue", disc_id);
    int read_only = logged && repair && access(o.h.rec_path, W_OK);   /* e.g. run from a mounted disc */
    logged &= !read_only;
    char *what, *agent;
    if (repair) {
        what = xstrdup("image test and repair (RS03: every sector against its CRC, the parity against the data)");
        agent = xstrdup(VERSION);
    } else if (device && image_hash(&o.cat, disc_id, &(uint64_t){ 0 })) {    /* every sector, against the image */
        uint64_t sectors = 0;
        const char *want = image_hash(&o.cat, disc_id, &sectors);
        char hex[65];
        fprintf(stderr, "Checking %s (%s): reading the %llu sectors of its image ...\n", disc_id, source, (unsigned long long)sectors);
        ok = !read_back(device, sectors, hex, &out) && !strcmp(hex, want);
        if (out.len) sb_puts(&out, "read the disc into an image and repair it (README.txt on the disc, REPAIR)\n");
        else sb_printf(&out, "read %llu sectors from %s: SHA-256 %s\n%s\n", (unsigned long long)sectors, device, hex,
                       ok ? "the disc holds exactly the image arv made" : "DIFFERENT from the image arv made: test it with dvdisaster -s, or read it into an image and arv check --image --repair");
        what = xprintf("read-back of the whole image from %s, against its SHA-256 at creation", device);
        agent = xstrdup(VERSION);
    } else if (device) {
        if (!on_path("dvdisaster"))
            die("%s has no image hash in the catalogue to read it back against; scanning it needs dvdisaster Light on PATH", disc_id);
        fprintf(stderr, "Checking %s (%s) ...\n", disc_id, source);
        char *a[] = { "dvdisaster", "-d", (char *)device, "-s", "--no-progress", NULL }, *output = NULL;
        ok = run(a, &output) == 0;
        sb_puts(&out, output ? output : "");
        free(output);
        what = xprintf("disc scan with dvdisaster -s on %s", device);
        agent = xstrdup("dvdisaster");
    } else {
        fprintf(stderr, "Checking %s (%s) ...\n", disc_id, source);
        ok = image_test(image, &out);
        if (ok) sb_puts(&out, "the image is whole\n");
        else {
            char *q = shell_quote(image);
            sb_printf(&out, "repair it: arv check --image %s --repair\n", q);
            free(q);
        }
        what = xstrdup("image test (RS03: every sector against its CRC, the parity against the data)");
        agent = xstrdup(VERSION);
    }
    const char *output = out.s ? out.s : "";
    char *sum = summary(output, 6), *text = note ? xprintf("%s\n%s\n%s", what, sum, note) : xprintf("%s\n%s", what, sum);
    if (logged) {
        const char *outcome = !ok ? "failure" : repair && !was_whole ? "warning" : "success";
        recs_add(&o.cat.events, new_event(disc_id, "fixity check", outcome, agent, "automatic", text));
        archive_save(&o.cat, o.h.rec_path);
    }
    printf("%s\n", !ok || verbose || repair ? output : sum);
    if (disc_id) printf("%s: %s\n", disc_id, ok ? (repair && !was_whole ? "REPAIRED" : "OK") : "FAILED - see output above");
    else printf("%s: %s\n", image, ok ? (was_whole ? "OK" : "REPAIRED") : "FAILED - see output above");
    if (!logged)
        fprintf(stderr, "(not logged: %s)\n", read_only ? "the catalogue found is read-only"
                                              : disc_id ? "that disc is not in this catalogue" : "no disc id; the image has no readable label");
    if (repair && !ok) {            /* the medium size: the catalogue's, the layout's, or the image's own */
        const char *ms = binding_field(&o.cat, disc_id, "MediumSectors");
        uint64_t medium = ms ? strtoull(ms, NULL, 10) : found_medium;
        struct stat st;
        if (!medium && !stat(image, &st) && st.st_size % 2048 == 0 && (st.st_size / 2048) % 255 == 0) medium = (uint64_t)st.st_size / 2048;
        hand_over(stdout, image, lost, medium);
    }
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
