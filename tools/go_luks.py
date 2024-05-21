import subprocess
import sys
import os

def run_command(command):
    """Run a system command and print output."""
    try:
        print(f"Running command: {command}")
        result = subprocess.run(command, shell=True, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        print(result.stdout.decode())
        if result.stderr:
            print(result.stderr.decode(), file=sys.stderr)
    except subprocess.CalledProcessError as e:
        print(f"Error: {e}")
        sys.exit(1)

def install_packages():
    """Install required packages."""
    packages = ['cryptsetup', 'parted']
    for package in packages:
        run_command(f"sudo apt-get install -y {package}")

def prepare_partition(device):
    """Create and prepare a partition on the device."""
    run_command(f"sudo parted {device} mklabel gpt")
    run_command(f"sudo parted -a opt {device} mkpart primary ext4 0% 100%")
    run_command(f"sudo parted {device} print")

def setup_luks(device):
    """Set up LUKS on the device."""
    run_command(f"echo -n 'YES' | sudo cryptsetup luksFormat {device}1")
    run_command(f"sudo cryptsetup luksOpen {device}1 cryptopi")

def create_filesystem():
    """Create a filesystem on the encrypted partition."""
    run_command("sudo mkfs.ext4 /dev/mapper/cryptopi")
    run_command("sudo mkdir -p /mnt/cryptopi")
    run_command("sudo mount /dev/mapper/cryptopi /mnt/cryptopi")

def configure_fstab():
    """Configure fstab for automatic mounting."""
    fstab_entry = "/dev/mapper/cryptopi /mnt/cryptopi ext4 defaults 0 2\n"
    with open("/etc/fstab", "a") as fstab:
        fstab.write(fstab_entry)
    print(f"/etc/fstab updated with: {fstab_entry}")

def main():
    if os.geteuid() != 0:
        print("This script must be run as root.")
        sys.exit(1)
    
    device = "/dev/mmcblk0"
    
    install_packages()
    prepare_partition(device)
    setup_luks(device)
    create_filesystem()
    configure_fstab()
    
    print("eMMC encryption setup complete. Please reboot your system.")

if __name__ == "__main__":
    main()

    
// Professor Raziel. K.B
// CWD SYSTEMS & OKN
