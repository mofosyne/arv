/*
 * What a drive says about the disc in it, for telling copies apart: the media's manufacturer and
 * type (the same on every disc of a pack), and the BCA serial (one disc's own, written at the
 * factory into the burst cutting area near the hub, if the media has one and the drive will read
 * it). Read only: it sends read commands and writes nothing, so a blank disc is fine (the BCA and
 * the media id are there before anything is burned).
 *
 *   cc -o disc-probe dev-tools/disc-probe.c
 *   ./disc-probe /dev/sr0            (as a user who can open the drive: often the cdrom group)
 *   ./disc-probe /dev/sr0 --raw      (and every reply in hex)
 *
 * Linux only (SG_IO); not part of arv. Untested on real drives when written: run it on yours, with
 * a blank and a burned disc of the same pack, and the answers (and a failure's sense codes) say
 * whether arv can read a per-copy serial there. The offsets decoded below are as dvd+rw-tools reads
 * them; --raw shows the bytes to check them against.
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <scsi/sg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int raw;

typedef struct {
    unsigned char sense[32];
    int status, sense_len;
} reply;

/* one read command; 0, or -1 with r->sense filled in (or errno when the ioctl itself failed) */
static int command(int fd, const unsigned char *cdb, int cdb_len, unsigned char *buf, int len, reply *r)
{
    sg_io_hdr_t io;
    memset(&io, 0, sizeof io);
    memset(r, 0, sizeof *r);
    memset(buf, 0, (size_t)len);
    io.interface_id = 'S';
    io.cmdp = (unsigned char *)cdb;
    io.cmd_len = (unsigned char)cdb_len;
    io.dxferp = buf;
    io.dxfer_len = (unsigned)len;
    io.dxfer_direction = SG_DXFER_FROM_DEV;
    io.sbp = r->sense;
    io.mx_sb_len = sizeof r->sense;
    io.timeout = 20000;
    if (ioctl(fd, SG_IO, &io) < 0) return -1;
    r->status = io.status;
    r->sense_len = io.sb_len_wr;
    return io.status || io.host_status || io.driver_status ? -1 : 0;
}

/* "sense 5/24/00: invalid field in the command" and the like */
static void why(const char *what, const reply *r)
{
    if (!r->sense_len) {
        printf("  %s: failed (%s)\n", what, r->status ? "no sense data" : strerror(errno));
        return;
    }
    int key = r->sense[2] & 0x0f, asc = r->sense[12], ascq = r->sense[13];
    const char *meaning = key == 2 && asc == 0x3a ? "no disc in the drive"
                        : key == 5 && asc == 0x24 ? "the drive does not offer this (invalid field)"
                        : key == 5 && asc == 0x20 ? "the drive does not know this command"
                        : key == 5 && asc == 0x30 ? "not on this kind of media"
                        : key == 5 && asc == 0x6f ? "copy protection: the drive wants AACS authentication first"
                        : key == 2 && asc == 0x04 ? "the drive is not ready (spinning up?)"
                        : "";
    printf("  %s: failed, sense %X/%02X/%02X%s%s\n", what, key, asc, ascq, *meaning ? ": " : "", meaning);
}

static void hex(const unsigned char *p, int n)
{
    for (int i = 0; i < n; i += 16) {
        printf("    %04x ", i);
        for (int j = i; j < i + 16 && j < n; j++) printf(" %02x", p[j]);
        printf("  ");
        for (int j = i; j < i + 16 && j < n; j++) putchar(p[j] >= 32 && p[j] < 127 ? p[j] : '.');
        putchar('\n');
    }
}

static void text(const char *label, const unsigned char *p, int n)
{
    printf("  %s", label);
    for (int i = 0; i < n; i++) putchar(p[i] >= 32 && p[i] < 127 ? p[i] : '.');
    putchar('\n');
}

static const char *profile_name(int p)
{
    switch (p) {
    case 0x00: return "none (no disc?)";
    case 0x08: return "CD-ROM";
    case 0x09: return "CD-R";
    case 0x0a: return "CD-RW";
    case 0x10: return "DVD-ROM";
    case 0x11: return "DVD-R";
    case 0x1b: return "DVD+R";
    case 0x2b: return "DVD+R DL";
    case 0x40: return "BD-ROM";
    case 0x41: return "BD-R (sequential)";
    case 0x42: return "BD-R (random)";
    case 0x43: return "BD-RE";
    default: return "other";
    }
}

/* READ DISC STRUCTURE: media 1 = BD, 0 = DVD */
static int structure(int fd, int media, int format, unsigned char *buf, int len, reply *r)
{
    unsigned char cdb[12] = { 0xad, (unsigned char)media, 0, 0, 0, 0, 0, (unsigned char)format,
                              (unsigned char)(len >> 8), (unsigned char)len, 0, 0 };
    return command(fd, cdb, 12, buf, len, r);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argv[1][0] == '-') {
        fputs("usage: disc-probe DEVICE [--raw]   (e.g. /dev/sr0)\n", stderr);
        return 2;
    }
    raw = argc > 2 && !strcmp(argv[2], "--raw");
    int fd = open(argv[1], O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "disc-probe: cannot open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    unsigned char buf[4100];
    reply r;

    unsigned char inquiry[6] = { 0x12, 0, 0, 0, 96, 0 };
    int failed = command(fd, inquiry, 6, buf, 96, &r);
    if (failed && !r.sense_len && errno) {
        fprintf(stderr, "disc-probe: %s does not take SCSI commands (%s): is it an optical drive?\n", argv[1], strerror(errno));
        return 1;
    }
    puts("Drive");
    if (failed) {
        why("INQUIRY", &r);
    } else {
        text("vendor:    ", buf + 8, 8);
        text("model:     ", buf + 16, 16);
        text("firmware:  ", buf + 32, 4);
    }

    puts("Disc");
    unsigned char config[10] = { 0x46, 0x02, 0, 0, 0, 0, 0, 0, 8, 0 };
    int profile = -1;
    if (command(fd, config, 10, buf, 8, &r)) why("GET CONFIGURATION", &r);
    else {
        profile = buf[6] << 8 | buf[7];
        printf("  kind:      %s (profile 0x%04x)\n", profile_name(profile), profile);
    }
    unsigned char info[10] = { 0x51, 0, 0, 0, 0, 0, 0, 0, 34, 0 };
    if (command(fd, info, 10, buf, 34, &r)) why("READ DISC INFORMATION", &r);
    else {
        static const char *const status[] = { "blank", "appendable (open)", "complete (closed)", "other" };
        printf("  state:     %s\n", status[buf[2] & 3]);
    }
    int bd = profile >= 0x40 && profile <= 0x4f;

    puts("Media id (manufacturer and type: the same on every disc of a pack)");
    if (bd) {
        if (structure(fd, 1, 0x00, buf, 4100, &r)) why("BD disc information", &r);
        else {
            const unsigned char *di = buf + 4;
            if (di[0] == 'D' && di[1] == 'I') {
                text("disc type: ", di + 8, 3);
                text("maker:     ", di + 100, 6);
                text("type id:   ", di + 106, 3);
                printf("  revision:  %u\n", di[111]);
            } else {
                puts("  no DI unit where expected (see --raw)");
            }
            if (raw) hex(buf, 4 + 112);
        }
    } else {
        if (structure(fd, 0, 0x11, buf, 4 + 256, &r)) why("DVD-R manufacturer (format 0x11)", &r);
        else if (raw) hex(buf, 4 + 256);
        if (structure(fd, 0, 0x00, buf, 4 + 2048, &r)) why("DVD physical format", &r);
        else {
            text("maker (DVD+R): ", buf + 4 + 19, 8);
            if (raw) hex(buf, 4 + 64);
        }
    }

    puts("BCA serial (one disc's own, if the media has one and the drive reads it)");
    int got = 0;
    char serial[2 * 16 + 1] = "";
    for (int media = bd ? 1 : 0; media >= 0 && !got; media--) {
        if (structure(fd, media, 0x03, buf, 4 + 64, &r)) {
            why(media ? "BD BCA" : "BCA (DVD form)", &r);
            continue;
        }
        int n = (buf[0] << 8 | buf[1]) - 2;
        if (n <= 0 || n > 64) n = 64;
        int zero = 1;
        for (int i = 0; i < n; i++) zero &= !buf[4 + i];
        if (zero) {
            puts("  read, but empty: no BCA on this media");
            continue;
        }
        got = 1;
        printf("  BCA (%d bytes):\n", n);
        hex(buf + 4, n);
        for (int i = 0; i < 16; i++) sprintf(serial + 2 * i, "%02x", buf[4 + i]);   /* the first unit: the disc's own */
    }

    puts("AACS identifiers (usually need AACS authentication; listed to see what the drive says)");
    if (structure(fd, 1, 0x81, buf, 4 + 16, &r)) why("media serial number", &r);
    else hex(buf + 4, 16);
    if (structure(fd, 1, 0x82, buf, 4 + 16, &r)) why("media identifier", &r);
    else hex(buf + 4, 16);

    close(fd);
    if (got) printf("\nA BCA was read: serial %s (its first 16 bytes; the rest repeats it, then says BDR).\n"
                    "On Linux, arv burned --device reads it itself and records it with the copy; elsewhere,\n"
                    "arv burned --bca %s does. (Two Verbatim discs of one pack gave two serials:\n"
                    "check yours the same way once.)\n", serial, serial);
    else puts("\nNo BCA read: copies are told apart by their letters (arv burned says which).");
    return 0;
}
