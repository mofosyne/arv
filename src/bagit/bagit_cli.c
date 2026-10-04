/*
 * bagit: check BagIt bags (RFC 8493).
 *
 *   bagit [--fast] [-v] BAG...     every manifest, every checksum, completeness, Payload-Oxum;
 *                                  --fast: completeness and Payload-Oxum only; -v: every file
 * Exit status: 0 every bag valid, 1 one is not, 2 usage.
 */
#include "bagit.h"

#include <stdio.h>
#include <string.h>

static void say(void *ctx, enum bagit_event what, const char *path, const char *detail)
{
    static const char *const word[] = { "OK      ", "FAILED  ", "MISSING ", "EXTRA   ", "INVALID " };
    (void)ctx;
    printf("%s %s%s%s%s\n", word[what], path, *detail ? " (" : "", detail, *detail ? ")" : "");
}

int main(int argc, char **argv)
{
    unsigned flags = 0;
    int bags = 0, bad = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fast")) flags |= BAGIT_FAST;
        else if (!strcmp(argv[i], "-v")) flags |= BAGIT_VERBOSE;
        else if (argv[i][0] == '-') {
            fputs("usage: bagit [--fast] [-v] BAG...\n", stderr);
            return 2;
        }
    }
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        bagit_report r;
        int invalid = bagit_validate(argv[i], flags, &r, say, NULL);
        bags++;
        bad |= invalid;
        if (invalid)
            printf("%s is invalid: %zu failed, %zu missing, %zu extra, %zu other problems\n", argv[i], r.failed, r.missing,
                   r.extra, r.invalid);
        else
            printf("%s is valid: %zu files and %zu tag files (%s)%s\n", argv[i], r.payload_ok, r.tag_ok, r.algorithms,
                   flags & BAGIT_FAST ? ", checksums not read (--fast)" : "");
    }
    if (!bags) {
        fputs("usage: bagit [--fast] [-v] BAG...\n", stderr);
        return 2;
    }
    return bad;
}
