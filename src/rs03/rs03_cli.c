/*
 * rs03: add RS03 error correction to a disc image, as `dvdisaster -mRS03 -o image -c` does.
 *
 *   rs03 [-n MEDIUM-SECTORS] [--no-bdr-defect-management] IMAGE
 *   rs03 -t IMAGE      test it: every sector against its CRC, the parity against the data
 */
#include "rs03.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    unsigned long long medium = 0;
    int no_dm = 0, test = 0;
    const char *image = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) medium = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--no-bdr-defect-management")) no_dm = 1;
        else if (!strcmp(argv[i], "-t")) test = 1;
        else if (!image && argv[i][0] != '-') image = argv[i];
        else {
            fputs("usage: rs03 [-n MEDIUM-SECTORS] [--no-bdr-defect-management] IMAGE | rs03 -t IMAGE\n", stderr);
            return 2;
        }
    }
    if (!image) {
        fputs("usage: rs03 [-n MEDIUM-SECTORS] [--no-bdr-defect-management] IMAGE | rs03 -t IMAGE\n", stderr);
        return 2;
    }
    rs03_layout lay;
    char err[512], line[200];
    if (test) {
        rs03_report r;
        if (rs03_verify(image, &r, err, sizeof err)) {
            fprintf(stderr, "rs03: %s\n", err);
            return 1;
        }
        rs03_describe(&r.lay, line, sizeof line);
        printf("%s\nheader %s; %llu data sectors with a wrong CRC, %llu CRC sectors and %llu parity sectors damaged\n",
               line, r.header_ok ? "good" : "DAMAGED", (unsigned long long)r.bad_data, (unsigned long long)r.bad_crc,
               (unsigned long long)r.bad_ecc);
        int ok = r.header_ok && !r.bad_data && !r.bad_crc && !r.bad_ecc;
        puts(ok ? "good: the image is whole" : "DAMAGED: repair it with dvdisaster -f");
        return ok ? 0 : 1;
    }
    if (rs03_augment(image, medium, no_dm, &lay, err, sizeof err)) {
        fprintf(stderr, "rs03: %s\n", err);
        return 1;
    }
    rs03_describe(&lay, line, sizeof line);
    printf("%s\n%llu sectors\n", line, (unsigned long long)lay.total_sectors);
    return 0;
}
