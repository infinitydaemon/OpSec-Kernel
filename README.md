![Github Banner](https://cwd.systems/img/banner.png)

```python
class CWD_OpSec():
    
  def __init__(self):
    self.name = "cwd";
    self.current.release = "The Dark Hadou";
    self.username = "cwdsystems";
    self.location = "KyrgzRepublic";
    self.protonmail = "@cwdsystems";
    self.web = "https://cwd.systems";
    self.languages ="Objective C, Python,Bash,Perl";
  
  def __str__(self):
    return self.name

if __name__ == '__main__':
    me = CWD_OpSec()
```

CWD SYSTEMS Linux
=================

Linux Kernel that was tuned and built as Brooklyn Supreme is now OpSec Kernel used in CWD & 0KN Appliances. Numerous optimizations and tweaks have been applied for high availability and network wide security on a packaged build which is shipped on CWD and 0KN appliances. Each appliance has its own different kernel and NOT a generic OpSec release due to the fact that each appliance has a different purpose. The build instructions for SBC vary when compared to official kernel.org build instructions. Follow the steps to build the OpSec Kernel for your SBC on Debian based distros. If a menu driven config is required, you will need ncurses-development headers.

```bash
sudo apt install git bc bison flex libssl-dev make libncurses-dev ( optional )
git clone https://github.com/infinitydaemon/OpSec-Kernel.git
cd OpSec-Kernel
- For PRI4
KERNEL=kernel8
make bcm2711_defconfig
- FOR RPI5
KERNEL=kernel_2712
make bcm2712_defconfig
edit the .config file and put a different name and build version for your kernel as :
CONFIG_LOCALVERSION="-OpSec-XXX" , where XXX is the current release version
make -j4 Image.gz modules dtbs
sudo make modules_install
sudo cp arch/arm64/boot/dts/broadcom/*.dtb /boot/
sudo cp arch/arm64/boot/dts/overlays/*.dtb* /boot/overlays/
sudo cp arch/arm64/boot/dts/overlays/README /boot/overlays/
sudo cp arch/arm64/boot/Image.gz /boot/$KERNEL.img
```

After the built kernel is installed, just reboot

```bash
sudo reboot
```

In the first line, put kernel=NAME.img. Where NAME is the name of the kernel you specified from CONFIG_LOCALVERSION. The easiest way to find out the full name of kernel is by doing a "ls -l" under /boot directory.

*Cross-Compiling the Kernel*

This method is only required if you are not building the OpSec Kernel natively on your SBC. To build the sources for cross-compilation, make sure you have the dependencies needed on your machine by executing:

``` sudo apt install git bc bison flex libssl-dev make libc6-dev libncurses5-dev ```

Install the 64-bit Toolchain for a 64-bit Kernel

``` sudo apt install crossbuild-essential-arm64 ```

An example cross-compile as a 64bit build : 

```bash
KERNEL=kernel8
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- 
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image modules dtbs
sudo env PATH=$PATH make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- INSTALL_MOD_PATH=mnt/ext4 modules_install
sudo cp mnt/fat32/$KERNEL.img mnt/fat32/$KERNEL-backup.img
sudo cp arch/arm64/boot/Image mnt/fat32/$KERNEL.img
sudo cp arch/arm64/boot/dts/broadcom/*.dtb mnt/fat32/
sudo cp arch/arm64/boot/dts/overlays/*.dtb* mnt/fat32/overlays/
sudo cp arch/arm64/boot/dts/overlays/README mnt/fat32/overlays/
sudo umount mnt/fat32
sudo umount mnt/ext4

```
In order to build the documentation, use ``make htmldocs`` or
``make pdfdocs``.  The formatted documentation can also be read online at:

    https://www.kernel.org/doc/html/latest/

CWD SYSTEMS does not provide support for Kernel builds. Refer to kernel.org forums for generic build instructions and help.
