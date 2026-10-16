#!/bin/bash

# Check if the script is running as root
if [ "$EUID" -ne 0 ]; then
    echo "Error: This script must be run as root." >&2
    exit 1
fi
echo "Installing the configuration on wkst..."

# Directory holding this script and the config files it installs
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p /opt/scripts

cp "$SCRIPT_DIR/nvidia-clocklock.service" /etc/systemd/system/nvidia-clocklock.service
chmod 664 /etc/systemd/system/nvidia-clocklock.service
systemctl enable --now nvidia-clocklock.service

mkdir -p /mnt/SFTP
mkdir -p /mnt/Data
mkdir -p /mnt/Media
mkdir -p /mnt/Scratch
mkdir -p /mnt/Cameras
mkdir -p /mnt/Regions
mkdir -p /mnt/Games
mkdir -p /mnt/MP3s
mkdir -p /mnt/External
cp "$SCRIPT_DIR/fstab" /etc/fstab
chmod 664 /etc/fstab

cp "$SCRIPT_DIR/sshfs_wrapper.sh" /root/sshfs_wrapper.sh
chmod +x /root/sshfs_wrapper.sh

cp "$SCRIPT_DIR/bashrc" /home/ben/.bashrc
chmod +x /home/ben/.bashrc
chown ben:ben /home/ben/.bashrc

cp "$SCRIPT_DIR/toggle_split_screen.sh" /opt/scripts/toggle_split_screen.sh
chmod +x /opt/scripts/toggle_split_screen.sh

echo "Complete, reboot system for changes to take effect."
exit
