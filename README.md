![Github Banner](https://cwd.systems/img/banner.png)

```python
class CWD_OpSec():
    
  def __init__(self):
    self.name = "cwd";
    self.current.release = "The Crawling Serpent for ARM";
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

Linux Kernel that was tuned and built as Brooklyn Supreme is now OpSec Kernel used in CWD Appliances. Numerous optimizations and tweaks have been applied for high availability and network wide security on a packaged build which is shipped on CWD appliances. Each appliance has its own different kernel and NOT a generic OpSec release due to the fact that each appliance has a different purpose. The build instructions for SBC vary when compared to official kernel.org build instructions. Follow the steps to build the OpSec Kernel for your SBC on Debian based distros. If a menu driven config is required, you will need ncurses-development headers.

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

In order to build the documentation, use ``make htmldocs`` or
``make pdfdocs``.  The formatted documentation can also be read online at:

    https://www.kernel.org/doc/html/latest/

We are dedicated to pushing the boundaries of innovation with our projects. If you appreciate our work and want to support our Research & Development efforts, consider making a donation. Your contributions help us continue our mission and achieve greater milestones.  

### Crypto Donations

- **Litecoin (LTC)**: `LfrJzpybM8ZRTFcd8HYfH4NXFtPKpr5Dpg`  
- **Bitcoin (BTC)**: `13zp3jdZ5utX5vmZaZiDyJtam8daS4uBpC`  
- **Ethereum (ERC20)**: `0x822803b26e4c235658085341aa113555d35e0b4c`  
- **Dogecoin (DOGE)**: `DJTRkmhwhG7W8t7WvAddZBnNkKWML6nHqJ`  

Thank you for your generosity and support!
