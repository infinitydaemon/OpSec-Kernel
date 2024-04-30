#!/usr/bin/python3

import argparse
import zymkey

def enable_zboot():
    # Enable zboot loader
    zymkey.client.enable_zboot()

if __name__ == "__main__":
    # Setup arg parser
    parser = argparse.ArgumentParser(description="Enable zboot loader from uboot for Zymbit SCM4")

    # Parse arguments
    args = parser.parse_args()

    # Enable zboot loader
    enable_zboot()
