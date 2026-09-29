"""Blu-ray media capacities and the RS03 space calculation.

Capacities (2048-byte sectors) and the layout arithmetic are taken from
dvdisaster (speed47 fork, src/dvdisaster.h and src/rs03-common.c), so a data
budget computed here matches what dvdisaster will lay out.
"""

import math

SECTOR = 2048
GF_FIELDMAX = 255  # RS03 codeword length: data layers + CRC layer + root layers

# name: (label, sectors with BD-R defect management, sectors without)
MEDIA = {
    "bd25": ("BD-R 25GB", 11826176, 12219392),
    "bd50": ("BD-R DL 50GB", 23652352, 24438784),
    "bd100": ("BD-R XL 100GB", 47305728, 48878592),
    "bd128": ("BD-R XL 128GB", 60403712, 62500864),
}

# dvdisaster -n names, used when the caller prefers a named size
DVDISASTER_NAMES = {"bd25": "BD", "bd50": "BD2", "bd100": "BDXL3", "bd128": "BDXL4"}


def capacity(medium, defect_management=True):
    label, dm, nodm = MEDIA[medium]
    return dm if defect_management else nodm


def label(medium):
    return MEDIA[medium][0]


def rs03_layout(data_sectors, medium_sectors):
    """(roots, redundancy %) that dvdisaster RS03 gives ``data_sectors`` on this medium."""
    per_layer = medium_sectors // GF_FIELDMAX
    ndata = (data_sectors + 2 + per_layer - 1) // per_layer
    ndata = max(ndata, 84)  # dvdisaster clips at 170 roots
    ndata += 1  # CRC layer
    roots = GF_FIELDMAX - ndata
    return roots, roots * 100.0 / ndata


def data_budget(medium_sectors, min_redundancy):
    """Largest image (in sectors) that still gets at least ``min_redundancy`` % RS03 redundancy."""
    per_layer = medium_sectors // GF_FIELDMAX
    # redundancy = roots / (ndata + 1) with roots = 255 - (ndata + 1)
    #  => ndata + 1 <= 255 / (1 + r)
    max_ndata = math.floor(GF_FIELDMAX / (1 + min_redundancy / 100.0)) - 1
    max_ndata = min(max_ndata, GF_FIELDMAX - 1 - 8)  # dvdisaster needs at least 8 roots
    if max_ndata < 1:
        raise ValueError("redundancy %s%% is not possible" % min_redundancy)
    return max_ndata * per_layer - 2
