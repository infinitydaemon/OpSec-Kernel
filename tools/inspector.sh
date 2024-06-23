#!/bin/bash

# Script to check for critical kernel messages after loggin in
check_kernel_critical_messages() {
    echo -e "\e[1;34m**************************************************\e[0m"
    echo -e "\e[1;34m* Checking kernel logs for critical messages...  *\e[0m"
    echo -e "\e[1;34m**************************************************\e[0m"

    # Search for critical messages in the kernel log
    critical_messages=$(dmesg | grep -i "critical")

    if [ -n "$critical_messages" ]; then
        echo -e "\e[1;31mCritical messages found:\e[0m"
        echo "$critical_messages"
    else
        echo -e "\e[1;32mNo critical messages found in kernel logs.\e[0m"
    fi
}

# Main function
main() {
    echo -e "\e[1;34m**************************************************\e[0m"
    echo -e "\e[1;34m* Starting kernel log inspection...              *\e[0m"
    echo -e "\e[1;34m**************************************************\e[0m"

    check_kernel_critical_messages

    echo -e "\e[1;34m**************************************************\e[0m"
    echo -e "\e[1;34m* Kernel log inspection completed.               *\e[0m"
    echo -e "\e[1;34m**************************************************\e[0m"
}

# Run the main function
main
