#!/usr/bin/python3

def check_zboot_active():
    # Check if zboot is active
    with open('/proc/cmdline', 'r') as f:
        cmdline = f.read()
        if 'zboot' in cmdline:
            return True
        elif 'u-boot' in cmdline:
            return False
    return False

if __name__ == "__main__":
    # Check if zboot is active
    if check_zboot_active():
        print("zboot is active")
    else:
        print("u-boot is active")
