#!/bin/sh
# library-check.sh: check the copies arv says are due, in a tape library or a disc jukebox.
#
# arv keeps the record and does the checking; mtx moves the media. For each copy that arv todo
# --tsv says is due (never read back, or overdue) and that has a barcode (its own, or its tape's),
# this finds the slot holding that barcode (mtx status), loads it, checks the copy with arv, and
# puts it back. Copies on one tape are checked in one load.
#
#   library-check.sh [-n] CHANGER DRIVE-NUMBER DRIVE [MOUNT]
#     CHANGER        the media changer, as mtx -f takes it (/dev/sg3)
#     DRIVE-NUMBER   the changer's number for the drive (mtx load SLOT DRIVE-NUMBER), usually 0
#     DRIVE          the drive: /dev/sr1 for discs, or the tape drive LTFS mounts (/dev/sg4)
#     MOUNT          tapes: where LTFS mounts a tape, the same place the copies were stored
#                    from (arv checks each at its recorded path)
#     -n             say what would be done, and change nothing
#
# Environment: ARV (arv), MTX (mtx), OVERDUE (years; 5), LTFS_MOUNT (the command that mounts a
# tape; ltfs -o devname=DRIVE MOUNT), LTFS_UMOUNT (umount MOUNT).
# Tried against saved mtx output and stand-in commands, not a real library: run it with -n first.
set -eu
dry=
if [ "${1:-}" = -n ]; then dry=1; shift; fi
if [ $# -lt 3 ]; then sed -n '2,19s/^# \{0,1\}//p' "$0"; exit 2; fi
changer=$1 index=$2 drive=$3 mount=${4:-}
ARV=${ARV:-arv} MTX=${MTX:-mtx}
mount_cmd=${LTFS_MOUNT:-ltfs -o devname=$drive $mount} umount_cmd=${LTFS_UMOUNT:-umount $mount}
tab=$(printf '\t')
run() { if [ -n "$dry" ]; then echo "would run: $*"; else "$@" </dev/null; fi; }

# what is due, a line each: barcode (the tape's, else the copy's own; - if none), disc, copy, form, path
work=$(mktemp)
trap 'rm -f "$work"' EXIT
"$ARV" todo --tsv --overdue "${OVERDUE:-5}" | awk -F "$tab" -v OFS="$tab" '
    $1 == "read-back" || $1 == "overdue" {
        bc = "-"
        if ($6 ~ /^barcode:/) bc = substr($6, 9)
        else { n = split($5, ids, ","); for (i = 1; i <= n; i++) if (ids[i] ~ /^barcode:/) bc = substr(ids[i], 9) }
        print bc, $2, $3, $4, ($8 == "" ? "-" : $8)
    }' | sort > "$work"

status=$("$MTX" -f "$changer" status)
loaded= slot= mounted= failed=0
put_back() {
    if [ -n "$mounted" ]; then run sh -c "$umount_cmd"; mounted=; fi
    if [ -n "$loaded" ]; then run "$MTX" -f "$changer" unload "$slot" "$index"; loaded=; fi
}
while IFS="$tab" read -r bc disc copy form path; do
    if [ "$bc" = - ]; then
        echo "skip: $disc copy $copy has no barcode (arv identify $disc $copy barcode:CODE, or --carrier)"
        continue
    fi
    if [ "$bc" != "$loaded" ]; then
        put_back
        slot=$(printf '%s\n' "$status" | sed -n "s/^ *Storage Element \([0-9]*\):Full *:VolumeTag *= *$bc *\$/\1/p" | head -1)
        if [ -z "$slot" ]; then
            echo "skip: $disc copy $copy: barcode $bc is not in a slot (arv inventory says what is where)"
            continue
        fi
        echo "$bc: slot $slot"
        run "$MTX" -f "$changer" load "$slot" "$index"
        loaded=$bc
    fi
    if [ "$form" = disc ]; then
        run "$ARV" check --device "$drive" --copy "$copy" "$disc" || failed=1
    elif [ -n "$mount" ]; then
        if [ -z "$mounted" ]; then run sh -c "$mount_cmd"; mounted=1; fi
        run "$ARV" stored "$disc" "$path" --copy "$copy" || failed=1
    else
        echo "skip: $disc copy $copy is a $form on a tape: give MOUNT, where LTFS mounts it"
    fi
done < "$work"
put_back
exit $failed
