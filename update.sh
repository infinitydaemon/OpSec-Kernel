#!/bin/bash

# Script to check for the latest kernel release from a GitHub repository
# and compare it with the current kernel version.

# Configuration
GITHUB_API_URL="https://api.github.com/repos/infinitydaemon/OpSec-Kernel/releases/latest"
USER_AGENT="KernelChecker/1.0"

# Colors (if terminal supports)
RED=$(tput setaf 1 2>/dev/null || echo "")
GREEN=$(tput setaf 2 2>/dev/null || echo "")
YELLOW=$(tput setaf 3 2>/dev/null || echo "")
RESET=$(tput sgr0 2>/dev/null || echo "")

# Fetch latest release from GitHub
fetch_latest_release() {
    curl -fsSL -H "User-Agent: $USER_AGENT" "$GITHUB_API_URL"
}

# Extract tag_name from JSON
extract_tag_name() {
    jq -r '.tag_name // empty'
}

# Compare versions
# Returns:
# 0 = equal
# 1 = current < latest (update available)
# 2 = current > latest (ahead, likely custom)
compare_versions() {
    ver1="$1"
    ver2="$2"

    if [[ "$ver1" == "$ver2" ]]; then
        return 0
    elif [[ "$(printf '%s\n' "$ver1" "$ver2" | sort -V | head -n1)" == "$ver1" ]]; then
        return 1
    else
        return 2
    fi
}

# Main logic
echo "${YELLOW}Checking for the latest kernel release...${RESET}"

if ! data=$(fetch_latest_release); then
    echo "${RED}Error: Unable to fetch data from GitHub API.${RESET}"
    exit 1
fi

latest_version=$(echo "$data" | extract_tag_name)

if [[ -z "$latest_version" ]]; then
    echo "${RED}Error: Failed to extract latest version tag.${RESET}"
    exit 1
fi

current_version=$(uname -r)

compare_versions "$current_version" "$latest_version"
cmp_result=$?

case $cmp_result in
    0)
        echo "${GREEN}You are running the latest kernel version: $current_version${RESET}"
        ;;
    1)
        echo "${YELLOW}Update available! Latest: $latest_version | Current: $current_version${RESET}"
        ;;
    2)
        echo "${GREEN}Your kernel version ($current_version) is newer than the latest release ($latest_version).${RESET}"
        ;;
esac
