#!/bin/bash

# Function to print colored text
print_color() {
    case $2 in
        red)    echo -e "\e[91m$1\e[0m";;
        green)  echo -e "\e[92m$1\e[0m";;
        yellow) echo -e "\e[93m$1\e[0m";;
        blue)   echo -e "\e[94m$1\e[0m";;
        *)      echo "$1";;
    esac
}

# Print branding
print_color "0KN Remote Connect" green

# Kill any existing ngrok processes
pkill ngrok

# Prompt user for authentication token
print_color "Get your free auth token from https://dashboard.ngrok.com" green
read -p "Enter your ngrok authentication token: " ngrok_token

# Start ngrok on TCP port 80
ngrok authtoken $ngrok_token
ngrok tcp 80
