#!/bin/bash
#
# Blu‑ray Hybrid Disc Image Archival Script
#
# 2025
#
# This script creates a hybrid ISO image that combines UDF with ISO9660 
# (including Rock Ridge and Joliet extensions). It uses ISO‑level 3 to allow
# files larger than 4GB, though note that ISO‑level 3 only removes the size 
# limit—not the filename/path length limits. Rock Ridge (-R) preserves full
# POSIX attributes (and longer names up to 255 bytes), while Joliet (-J with 
# -joliet-long) creates a secondary directory tree for Windows compatibility.
#
# It then augments the generated ISO with error correction data using dvdisaster.
#
# Usage: ./create_iso.sh <source_folder> [<destination_iso_image>]

# -----------------------------------------------------------
# Step 1: Check for required dependencies
# -----------------------------------------------------------
for cmd in genisoimage dvdisaster; do
    if ! command -v "$cmd" &> /dev/null; then
        echo "Error: $cmd is not installed. Please install it."
        exit 1
    fi
done

# -----------------------------------------------------------
# Step 2: Validate script arguments
# -----------------------------------------------------------
if [ "$#" -lt 1 ]; then
    echo "Got $# args"
    echo "Usage: $0 <source_folder> [<destination_iso_image>]"
    exit 1
fi

# -----------------------------------------------------------
# Step 3: Derive Source Folder and Default Names
# -----------------------------------------------------------
SOURCE_FOLDER="$1"

# Remove trailing slash (if any) and extract just the folder name
DEFAULT_FOLDER_NAME=${SOURCE_FOLDER%/}
DEFAULT_FOLDER_NAME=${DEFAULT_FOLDER_NAME##*/}

# Generate a default volume label based on the folder name.
# The expected folder name format is something like:
#   2025-01-13_Projects_2020_-_2025
# The script strips off the date part and converts the remaining 
# text to a title-cased label (with spaces instead of underscores).
# So the folder name example above would be interpreted as:
#   Projects 2020 - 2025
DEST_LABEL=${DEFAULT_FOLDER_NAME#*-*-*_} #<< Exclude date to better fit into ISO9660 volume label
DEST_LABEL=$(echo "$DEST_LABEL" | sed 's/[^_]\+/\L\u&/g' | sed 's/_/ /g')

echo "SOURCE_FOLDER = $SOURCE_FOLDER"
echo "DEFAULT_FOLDER_NAME = $DEFAULT_FOLDER_NAME"
echo "DEST_LABEL = $DEST_LABEL"

# ISO9660 has a 32-character limit for the volume label.
MAX_VOLID_LEN=32
if [ ${#DEST_LABEL} -gt $MAX_VOLID_LEN ]; then
    echo "Volume label is longer than $MAX_VOLID_LEN characters; '$DEST_LABEL'. Exiting..."
    exit 1
fi

echo "Using volume label = $DEST_LABEL"

# Get the destination ISO image filename, defaulting to <folder_name>.iso if not provided
DEST_IMAGE=${2:-${DEFAULT_FOLDER_NAME}.iso}
echo "DEST_IMAGE = $DEST_IMAGE"

# -----------------------------------------------------------
# Step 4: Create the Hybrid ISO Image using genisoimage
# -----------------------------------------------------------
echo "Creating hybrid ISO image..."

# Breakdown of key options:
#   -udf                  : Include UDF support (useful for DVD/BD formats)
#   -R                    : Enable Rock Ridge extensions for POSIX attributes and long filenames (up to 255 bytes)
#   -J -joliet-long       : Create an additional Joliet tree for Windows with extended filename support (up to 64/103 characters)
#   -allow-lowercase      : Retain lowercase letters in filenames
#   -allow-multidot       : Allow filenames with multiple dots
#   -allow-limited-size   : Prevent issues with files larger than 4GB in hybrid ISO/UDF images.
#   -iso-level 3          : Use ISO9660 level 3 to remove the 4GB file size limit
#   -V "$DEST_LABEL"      : Set the volume label (must be 32 characters or less)
#   -o "$DEST_IMAGE"      : Specify the output file for the ISO image
#   "$SOURCE_FOLDER"      : Source directory to be included in the ISO image
genisoimage -udf -R -J -joliet-long -allow-lowercase -allow-multidot -allow-limited-size -iso-level 3 -V "$DEST_LABEL" -o "$DEST_IMAGE" "$SOURCE_FOLDER"
if [ $? -ne 0 ]; then
    echo "Error: Failed to create ISO image with genisoimage."
    exit 1
fi

echo "ISO image created at $DEST_IMAGE"

# -----------------------------------------------------------
# Step 5: Enhance the ISO Image with Error Correction using dvdisaster
# -----------------------------------------------------------
echo "Enhancing image with error correction using dvdisaster..."

# Breakdown of dvdisaster options:
#   -i "$DEST_IMAGE"   : Specify the input ISO image
#   -mRS03             : Use error correction method RS03 (suitable for larger images)
#   -o image           : Specify that the output should be an augmented image (i.e. the ECC data is added to the ISO)
#   -c                 : Create ECC (error correction) information
dvdisaster -i "$DEST_IMAGE" -mRS03 -o image -c
if [ $? -ne 0 ]; then
    echo "Warning: Failed to add error correction."
else
    echo "Protected ISO image created successfully."
fi

exit 0