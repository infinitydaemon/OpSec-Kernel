import subprocess

# Dictionary of file paths and their corresponding SHA256 hash values
hashes = {
    "/bin/bash": "0c3dc0a741d39ce67f6e0b47d7a35f16b9f343cfe11e3b20ed055cefb49a998e",
    "/bin/cat": "c7f8d651ad9c1a4fa4b09a5e8fb1b5cb228c5a5a24240a1d194d8f299496ae24",
    "/bin/chmod": "f7c6e95511cf2c1b9e52995d2f82c09f6b7c4233a53b82f3b10f1f054deee6f9",
    "/bin/chown": "1a7c22e2d40661c7df7020c476822e7b4386988659e83d20b4e4cc4f0c82b0d4",
    "/bin/cp": "7dd5a5419f7c8d4fc52b4fc4e5d5b5fc8df30c5f7be5c23580f9c3a3a416a1de",
    # add more files here
}

# Check integrity of each file
for path, expected_hash in hashes.items():
    try:
        output = subprocess.check_output(["sha256sum", path], universal_newlines=True)
        actual_hash = output.split()[0]
        if actual_hash != expected_hash:
            print(f"WARNING: {path} has been modified (expected hash {expected_hash}, actual hash {actual_hash})")
    except subprocess.CalledProcessError:
        print(f"ERROR: failed to calculate hash for {path}")    

