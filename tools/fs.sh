#!/bin/bash

# specify directory to check
DIR_TO_CHECK="/usr"

# get current list of files and their md5sums
CURRENT_LIST=$(find "${DIR_TO_CHECK}" -type f -exec md5sum {} +)

# check if previous list exists
if [ -f "${DIR_TO_CHECK}/previous_list" ]; then
    # get previous list of files and their md5sums
    PREVIOUS_LIST=$(cat "${DIR_TO_CHECK}/previous_list")

    # compare current and previous lists
    if [ "${CURRENT_LIST}" != "${PREVIOUS_LIST}" ]; then
        echo "Changes detected in ${DIR_TO_CHECK}"
        # do something, like send a notification or log the changes
    else
        echo "No changes detected in ${DIR_TO_CHECK}"
    fi
else
    echo "Previous list not found, creating..."
    echo "${CURRENT_LIST}" > "${DIR_TO_CHECK}/previous_list"
fi
