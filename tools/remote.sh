#!/bin/bash

# Define ASCII Art for 0KN Remote Connect
cat << "EOF"


 _____ _   __ _   _  ______                     _         _____                             _   
|  _  | | / /| \ | | | ___ \                   | |       /  __ \                           | |  
| |/' | |/ / |  \| | | |_/ /___ _ __ ___   ___ | |_ ___  | /  \/ ___  _ __  _ __   ___  ___| |_ 
|  /| |    \ | . ` | |    // _ \ '_ ` _ \ / _ \| __/ _ \ | |    / _ \| '_ \| '_ \ / _ \/ __| __|
\ |_/ / |\  \| |\  | | |\ \  __/ | | | | | (_) | ||  __/ | \__/\ (_) | | | | | | |  __/ (__| |_ 
 \___/\_| \_/\_| \_/ \_| \_\___|_| |_| |_|\___/ \__\___|  \____/\___/|_| |_|_| |_|\___|\___|\__|
                                                                                                
                                                                                                


EOF

# Branding Message
echo "Welcome to 0KN Remote Connect"
echo "Starting ngrok on port 80..."
sleep 2
# Start ngrok on port 80
ngrok http 80
