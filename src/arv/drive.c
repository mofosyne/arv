/* What an optical drive says of the disc in it, beyond its sectors: the BCA serial, a BD-R's own
 * factory serial in its burst cutting area. It tells one copy of an image from another, so
 * arv burned --device records it with the copy and arv check --device knows which copy it read.
 *
 * Read with MMC READ DISC STRUCTURE (BD, format 03h), which needs no AACS authentication on the
 * drives tried (dev-tools/disc-probe.c asks a drive; docs/burning.md, the first-burn drill). The
 * BCA is 64 bytes: four 16-byte units, each written twice. The first is the disc's own (Verbatim
 * BD-R: a factory time to the second and a line number); the third says only BDR and its type.
 * The serial is the first unit, as 32 lowercase hex digits.
 *
 * Linux only (SG_IO); elsewhere, and on a drive or file that does not answer, there is none and
 * copies are told apart by their letters alone. */
#define _DEFAULT_SOURCE
#include "arv.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__) && !defined(__COSMOPOLITAN__)
#include <fcntl.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <unistd.h>

int drive_bca(const char *device, char out[33])
{
    int fd = open(device, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    unsigned char buf[4 + 64], sense[32];
    unsigned char cdb[12] = { 0xad, 1, 0, 0, 0, 0, 0, 0x03, 0, sizeof buf, 0, 0 };
    sg_io_hdr_t io;
    memset(&io, 0, sizeof io);
    memset(buf, 0, sizeof buf);
    io.interface_id = 'S';
    io.cmdp = cdb;
    io.cmd_len = sizeof cdb;
    io.dxferp = buf;
    io.dxfer_len = sizeof buf;
    io.dxfer_direction = SG_DXFER_FROM_DEV;
    io.sbp = sense;
    io.mx_sb_len = sizeof sense;
    io.timeout = 20000;
    int failed = ioctl(fd, SG_IO, &io) < 0 || io.status || io.host_status || io.driver_status;
    close(fd);
    if (failed) return -1;
    for (int unit = 0; unit < 2; unit++) {         /* the first unit, or its repeat when it reads as nothing */
        const unsigned char *u = buf + 4 + 16 * unit;
        int zero = 1;
        for (int i = 0; i < 16; i++) zero &= !u[i];
        if (zero) continue;
        for (int i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", u[i]);
        return 0;
    }
    return -1;
}
#else
int drive_bca(const char *device, char out[33])
{
    (void)device;
    (void)out;
    return -1;
}
#endif

int bca_normal(const char *text, char out[33])
{
    size_t n = 0;
    for (const char *p = text; *p && n < 32; p++) {
        if (isspace((unsigned char)*p) || *p == ':' || *p == '-') continue;
        if (!isxdigit((unsigned char)*p)) return -1;
        out[n++] = (char)tolower((unsigned char)*p);
    }
    out[n] = 0;
    return n == 32 ? 0 : -1;
}
