![Github Banner](https://cwd.systems/img/banner.png)

```python
class CWD_OpSec():
    
  def __init__(self):
    self.name = "cwd";
    self.username = "cwdsystems";
    self.location = "USA, Canada, Pakistan, KyrgzRepublic, Indonesia";
    self.protonmail = "@cwdsystems";
    self.web = "https://cwd.systems";
    self.languages ="Objective C, Python";
  
  def __str__(self):
    return self.name

if __name__ == '__main__':
    me = CWD_OpSec()
```

CWD SYSTEMS Linux kernel
========================

Linux Kernel that was tuned and built as Brooklyn Supreme is now OpSec Kernel. Numerous optimizations and tweaks have been applied for high availability and network wide security.
The build instructions for SBC vary when compared to official kernel.org build instructions. Follow the steps to build the OpSec Kernel for your SBC on Debian based distros.

```
sudo apt install git bc bison flex libssl-dev make
git clone https://github.com/infinitydaemon/OpSec-Kernel.git
cd OpSec-Kernel
KERNEL=kernel8
make bcm2711_defconfig
edit the .config file and put a different name and build version for your kernel as :
CONFIG_LOCALVERSION="-OpSec-6x"
make -j4 zImage modules dtbs
sudo make modules_install
sudo cp arch/arm/boot/dts/*.dtb /boot/
sudo cp arch/arm/boot/dts/overlays/*.dtb* /boot/overlays/
sudo cp arch/arm/boot/dts/overlays/README /boot/overlays/
sudo cp arch/arm/boot/zImage /boot/$KERNEL.img
sudo make install
```

After the built kernel is installed, edit the boot config

```
sudo nano /boot/config.txt
```

In the first line, put kernel=NAME. Where NAME is the name of the kernel you specified from CONFIG_LOCALVERSION. The easiest way to find out the full name of kernel is by doing a "ls -l" under /boot directory.

In order to build the documentation, use ``make htmldocs`` or
``make pdfdocs``.  The formatted documentation can also be read online at:

    https://www.kernel.org/doc/html/latest/


