# Work in progress.

#!/bin/bash

# GitHub repository URL for the latest release
REPO_URL="https://api.github.com/repos/infinitydaemon/OpSec-Kernel/releases/latest"

# Fetch the latest release tag
LATEST_RELEASE=$(curl -s "$REPO_URL" | grep '"tag_name":' | sed -E 's/.*"([^"]+)".*/\1/')

if [ -z "$LATEST_RELEASE" ]; then
    echo "Failed to fetch the latest release. Please check your internet connection or the repository URL."
    exit 1
fi

# Download the image.7z file from the latest release
DOWNLOAD_URL="https://github.com/infinitydaemon/OpSec-Kernel/releases/download/$LATEST_RELEASE/image.7z"
echo "Downloading image.7z from $DOWNLOAD_URL..."
curl -L -o image.7z "$DOWNLOAD_URL"

if [ ! -f "image.7z" ]; then
    echo "Failed to download image.7z."
    exit 1
fi

# Extract the image.7z file
echo "Extracting image.7z..."
7z x image.7z

if [ $? -ne 0 ]; then
    echo "Failed to extract image.7z."
    exit 1
fi

# Find the extracted image file (assuming it's a .img file)
IMAGE_FILE=$(find . -name "*.img" -o -name "*.iso" | head -n 1)

if [ -z "$IMAGE_FILE" ]; then
    echo "No image file found in the extracted files."
    exit 1
fi

echo "Found image file: $IMAGE_FILE"

# Find the MMC card (e.g., /dev/mmcblk0)
MMC_DEVICE=$(lsblk -o NAME,TRAN | grep "mmc" | awk '{print "/dev/"$1}')

if [ -z "$MMC_DEVICE" ]; then
    echo "MMC card not found. Please insert the MMC card and try again."
    exit 1
fi

echo "MMC device found: $MMC_DEVICE"

# Confirm before writing to the MMC card
read -p "Are you sure you want to write $IMAGE_FILE to $MMC_DEVICE? This will erase all data on the MMC card. (y/n): " CONFIRM

if [[ "$CONFIRM" != "y" && "$CONFIRM" != "Y" ]]; then
    echo "Operation canceled."
    exit 0
fi

# Write the image to the MMC card using dd
echo "Writing $IMAGE_FILE to $MMC_DEVICE..."
sudo dd if="$IMAGE_FILE" of="$MMC_DEVICE" bs=4M status=progress

if [ $? -ne 0 ]; then
    echo "Failed to write the image to the MMC card."
    exit 1
fi

# Sync and cleanup
echo "Syncing..."
sync

echo "Operation completed successfully."
