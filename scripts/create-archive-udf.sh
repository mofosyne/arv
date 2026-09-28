#!/bin/bash
#
# Blu‑ray Archival Script
#
# Warning: Not working... got 'wrong fs type, bad option, bad superblock on /dev/loop1, missing codepage or helper program, or other error.' mount error
#
# This script creates a blank UDF image sized for Blu‑ray media,
# formats it using mkudffs, and optionally mounts it for copying files.
# It is intended for archival to Blu‑ray only.
#
# Usage: ./create_bluray_udf.sh <source_folder> [<image_name>]

# Check for required dependencies
for cmd in mkudffs dvdisaster sudo dd truncate; do
    if ! command -v "$cmd" &> /dev/null; then
        echo "Error: $cmd is not installed. Please install it."
        exit 1
    fi
done

# Check for correct number of arguments
if [ "$#" -lt 1 ]; then
    echo "Got $# args"
    echo "Usage: $0 <source_folder> [<image_name>]"
    exit 1
fi

# Get Source Folder
SOURCE_FOLDER="$1"

# Derive default folder name from the source folder
DEFAULT_FOLDER_NAME=${SOURCE_FOLDER%/}
DEFAULT_FOLDER_NAME=${DEFAULT_FOLDER_NAME##*/}

# Generate a default disc title from the folder name
DEST_TITLE=$(echo "$DEFAULT_FOLDER_NAME" | sed 's/[^_]\+/\L\u&/g' | sed 's/_/ /g')

# Get destination image; if not specified, default to <foldername>.udf
DEST_IMAGE=${2:-${DEFAULT_FOLDER_NAME}.udf}

echo "SOURCE_FOLDER       = $SOURCE_FOLDER"
echo "DEFAULT_FOLDER_NAME = $DEFAULT_FOLDER_NAME"
echo "DEST_TITLE          = $DEST_TITLE"
echo "DEST_IMAGE          = $DEST_IMAGE"

# mkudffs settings for Blu‑ray
MEDIA_TYPE=bdr    # bdr – BD-R (Blu-ray Disc Recordable)
UDF_REV=2.60      # Use highest supported UDF version (Blu-ray requires UDF 2.50+)
echo "MEDIA_TYPE          = $MEDIA_TYPE"
echo "UDF_REV             = $UDF_REV"

# Calculate the size needed (in bytes) for the source folder and add 10% overhead
RAW_SIZE=$(du -sb "$SOURCE_FOLDER" | cut -f1)
OVERHEAD=$(echo "$RAW_SIZE * 0.10" | bc -l | cut -d. -f1)
TOTAL_SIZE=$(echo "$RAW_SIZE + $OVERHEAD" | bc)

echo "Source folder size: $RAW_SIZE bytes"
echo "Caculate 10% UDF metadata overhead: $OVERHEAD bytes"
echo "Allocating image size (with overhead): $TOTAL_SIZE bytes"

# Create a blank file of the calculated size
echo "Creating blank image file..."
truncate -s "$TOTAL_SIZE" "$DEST_IMAGE"
if [ $? -ne 0 ]; then
    echo "Error: Failed to create blank image file."
    exit 1
fi

# Format the blank image as a UDF filesystem using mkudffs
echo "Formatting image as UDF..."
mkudffs --media-type=$MEDIA_TYPE --udfrev=$UDF_REV --label="$DEST_TITLE" "$DEST_IMAGE"
if [ $? -ne 0 ]; then
    echo "Error: Failed to format the image with mkudffs."
    exit 1
fi

# Create a temporary mount point and mount the image
MOUNT_POINT=$(mktemp -d) 
echo "Mounting image at $MOUNT_POINT..."
sudo mount -t udf -o loop,rw "$DEST_IMAGE" "$MOUNT_POINT"
if [ $? -ne 0 ]; then
    echo "Error: Failed to mount the image."
    rmdir "$MOUNT_POINT"
    rm "$DEST_IMAGE"
    exit 1
fi

# Copy the source files into the mounted image
echo "Copying files from $SOURCE_FOLDER to the UDF image..."
sudo cp -a "$SOURCE_FOLDER"/. "$MOUNT_POINT"
if [ $? -ne 0 ]; then
    echo "Error: Failed to copy files."
    sudo umount "$MOUNT_POINT"
    rmdir "$MOUNT_POINT"
    exit 1
fi

sync || echo "Warning: sync command failed"

# Unmount the image and clean up the temporary mount point
echo "Unmounting image..."
sudo umount "$MOUNT_POINT"
rmdir "$MOUNT_POINT"
echo "UDF image created at $DEST_IMAGE"

# Optional: Enhance the image with error correction using dvdisaster
echo "Enhancing image with error correction using dvdisaster..."
dvdisaster -i "$DEST_IMAGE" -mRS02 -n 15% -o image
if [ $? -ne 0 ]; then
    echo "Warning: Failed to add error correction."
else
    echo "Protected image created successfully."
fi

exit 0
