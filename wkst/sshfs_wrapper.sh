#!/bin/bash
## install in /root/sshfs_wrapper.sh
# Define where your credentials file is located
CRED_FILE="/root/smb.credentials"
# Extract the password using awk (splits the line at '=' and prints the 2nd part)
PASSWORD=$(awk -F= '/^password=/{print $2}' "$CRED_FILE" | tr -d '\r\n')
# Export the password as an environment variable (securely read by sshpass)
export SSHPASS="$PASSWORD"
# Execute sshpass, telling it to use the environment variable (-e) and pass all other arguments to ssh
exec sshpass -e ssh -o StrictHostKeyChecking=no "$@"
