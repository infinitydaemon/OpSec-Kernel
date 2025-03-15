#!/bin/bash

# Script to check for the latest kernel release from a GitHub repository and compare it with the current kernel version.

# GitHub API URL for the latest release
GITHUB_API_URL="https://api.github.com/repos/infinitydaemon/OpSec-Kernel/releases/latest"
USER_AGENT="curl/7.77.0"

# Function to fetch the latest release data from GitHub API
fetch_latest_release() {
    curl -s -H "User-Agent: $USER_AGENT" "$GITHUB_API_URL"
}

# Function to parse JSON and get the latest version tag
get_latest_version() {
    jq -r '.tag_name'
}

# Function to compare two version strings
compare_versions() {
    if [[ "$1" == "$2" ]]; then
        echo "0"
    elif [[ "$(printf '%s\n' "$1" "$2" | sort -V | head -n1)" == "$1" ]]; then
        echo "1"
    else
        echo "-1"
    fi
}

# Main script logic
echo "Fetching the latest kernel release information from GitHub..."
data=$(fetch_latest_release)
if [[ $? -ne 0 ]]; then
    echo "Error: Failed to fetch data from GitHub API."
    exit 1
fi

latest_version=$(echo "$data" | get_latest_version)
if [[ -z "$latest_version" ]]; then
    echo "Error: Failed to parse the latest version from GitHub API response."
    exit 1
fi

# Get current kernel version
current_version=$(uname -r)

# Compare versions and output result
comparison_result=$(compare_versions "$current_version" "$latest_version")

if [[ "$comparison_result" == "1" ]]; then
    echo "An update is available! Latest version: $latest_version (Current version: $current_version)"
elif [[ "$comparison_result" == "0" ]]; then
    echo "You are already running the latest version: $current_version"
else
    echo "No updates available. Current version: $current_version (Latest version: $latest_version)"
fi
