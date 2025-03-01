#!/bin/bash

# Work in progress for kernel update. This disregards the apt updates needed to upgrade the kernel releases. 
# sudo apt install jq

GITHUB_API_URL="https://api.github.com/repos/infinitydaemon/OpSec-Kernel/releases/latest"
USER_AGENT="curl/7.77.0"

# Function to fetch latest release data from GitHub API
fetch_latest_release() {
    curl -s -H "User-Agent: $USER_AGENT" "$GITHUB_API_URL"
}

# Function to parse JSON and get the latest version tag
get_latest_version() {
    jq -r '.tag_name'
}

# Main script logic
data=$(fetch_latest_release)
if [[ $? -ne 0 ]]; then
    echo "Curl error: Failed to fetch data from GitHub API."
    exit 1
fi

latest_version=$(echo "$data" | get_latest_version)

# Get current kernel version
current_version=$(uname -r)

# Compare versions and output result
if [[ "$latest_version" > "$current_version" ]]; then
    echo "An update is available! Latest version: $latest_version"
else
    echo "No updates available. Current version: $current_version"
fi

