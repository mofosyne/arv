/*
 * rs03: dvdisaster's RS03 error correction for disc images, in C99 with no libraries.
 *
 * Augments an image the way `dvdisaster -i IMAGE -mRS03 -o image -c [-n SECTORS]` does, byte for
 * byte: the ecc header after the data, padding sectors, a CRC layer and Reed-Solomon parity
 * layers fill the image up to the medium size, interleaved over the whole disc. The format is
 * dvdisaster's (GPLv3; this is a reimplementation of its 0.79.10 / dvdisaster Light encoder, with
 * the format written up in docs/rs03.md), so any dvdisaster can test and repair the result.
 */
#ifndef RS03_H
#define RS03_H

#include <stddef.h>
#include <stdint.h>

/* Medium sizes in 2048-byte sectors, as dvdisaster knows them (dvdisaster.h) */
#define RS03_CDR        (351 * 1024)
#define RS03_DVD_SL     2295104
#define RS03_DVD_DL     4171712
#define RS03_BD_SL      11826176
#define RS03_BD_DL      23652352
#define RS03_BDXL_TL    47305728
#define RS03_BDXL_QL    60403712
#define RS03_BD_SL_NODM   12219392       /* without BD-R defect management */
#define RS03_BD_DL_NODM   24438784
#define RS03_BDXL_TL_NODM 48878592
#define RS03_BDXL_QL_NODM 62500864

typedef struct {
    uint64_t data_sectors;      /* the image before augmenting */
    uint64_t medium_sectors;    /* the medium the layout was computed for */
    uint64_t sectors_per_layer; /* medium_sectors / 255 */
    uint64_t total_sectors;     /* the augmented image: 255 layers */
    uint64_t data_padding;      /* padding sectors between the header and the CRC layer */
    uint64_t first_crc, first_ecc;
    int ndata, nroots;          /* RS(255, ndata); ndata counts the CRC layer */
    double redundancy;          /* nroots * 100 / ndata */
} rs03_layout;

/* The layout for an image of data_sectors on a medium of medium_sectors; with medium_sectors 0,
 * the smallest standard medium leaving at least 8 roots (no_dm: BD-R without defect management).
 * Returns 0, or -1 with *err set. */
int rs03_layout_for(uint64_t data_sectors, uint64_t medium_sectors, int no_dm, rs03_layout *lay, const char **err);

/* Augments the image file at path in place. Returns 0, or -1 with err (errlen bytes) filled in.
 * lay (may be NULL) receives the layout used. */
int rs03_augment(const char *path, uint64_t medium_sectors, int no_dm, rs03_layout *lay, char *err, size_t errlen);

/* What rs03_verify found: the header, the CRC of every data sector, every CRC sector and every
 * parity sector compared with what the data gives. All zero (and header_ok): the image is whole. */
typedef struct {
    rs03_layout lay;
    int header_ok;
    uint64_t bad_data;          /* data sectors whose CRC differs from the CRC layer's */
    uint64_t bad_crc;           /* CRC sectors whose layout fields differ */
    uint64_t bad_ecc;           /* parity sectors that differ */
} rs03_report;

/* Checks an augmented image (dvdisaster's, or rs03_augment's). Returns 0 with *report filled in,
 * or -1 with err filled in when it is not an RS03 image or cannot be read. */
int rs03_verify(const char *path, rs03_report *report, char *err, size_t errlen);

/* dvdisaster's summary line: "8 MiB data, 4 MiB ecc (84 roots; 49.1% redundancy), 0 MiB padding." */
void rs03_describe(const rs03_layout *lay, char *out, size_t outlen);

#endif
