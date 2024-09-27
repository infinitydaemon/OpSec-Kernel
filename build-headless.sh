#!/bin/bash

# CWD Kernel Build Script Rev 1 for ARM
# 
# Important Notice:
#
# 1. Use at Your Own Risk:
#    This script is provided "as-is," without any express or implied warranties
#    or guarantees. Use it at your own risk. The author(s) are not responsible
#    for any damage, data loss, or other issues that may arise from the use of
#    this script.
#
# 2. No Warranty:
#    This script is provided without any warranty of any kind, either express or
#    implied, including but not limited to the implied warranties of
#    merchantability, fitness for a particular purpose, or non-infringement. The
#    entire risk as to the quality and performance of the script is with you.
#
# 3. Backup Your Data:
#    Before running this script, ensure you have backed up all important data.
#    The use of this script may result in data loss or corruption, and the
#    author(s) will not be held responsible for any such incidents.
#
# 4. Compatibility and Testing:
#    This script may not be compatible with all systems or configurations. Test
#    thoroughly in a safe environment before using it on a production system.

# Ensure the script is run with sudo
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root (use sudo)" 
   exit 1
fi

# Install required packages
echo "Installing required packages..."
apt update
apt install -y git bc bison flex libssl-dev make libncurses-dev

# Clone the OpSec Kernel repository
echo "Cloning OpSec Kernel repository..."
git clone https://github.com/infinitydaemon/OpSec-Kernel.git

# Navigate into the kernel source directory
cd OpSec-Kernel || { echo "Failed to enter OpSec-Kernel directory"; exit 1; }

# Set the kernel variable
KERNEL=kernel8

# Download custom config file (replace with your actual URL for config file)
echo "Downloading custom config file from GitHub..."
wget -O .config https://raw.githubusercontent.com/infinitydaemon/OpSec-Kernel/refs/heads/main/configs/config-THC-Headless-Server

# Build the kernel, modules, and device tree blobs
echo "Building the kernel and modules..."
make -j4 Image.gz modules dtbs

# Install the modules
echo "Installing modules..."
sudo make modules_install

# Copy the kernel and device tree files to /boot
echo "Copying kernel and dtb files to /boot..."
sudo cp arch/arm64/boot/dts/broadcom/*.dtb /boot/
sudo cp arch/arm64/boot/dts/overlays/*.dtb* /boot/overlays/
sudo cp arch/arm64/boot/dts/overlays/README /boot/overlays/
sudo cp arch/arm64/boot/Image.gz /boot/$KERNEL.img
sudo make install 

# Notify the user to manually update /boot/config.txt. Do not do this on a SCM4 board as it will brick your board.
echo "The kernel build and installation is complete."
echo "Please manually update /boot/config.txt to specify the new kernel."
echo "Add/modify the following line in /boot/config.txt:"
echo "kernel=$KERNEL.img"
