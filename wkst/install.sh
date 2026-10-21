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

## Network shares: autofs mounts the files server's shares on access
apt-get install -y --no-upgrade --ignore-missing autofs cifs-utils sshfs sshpass
cp "$SCRIPT_DIR/autofs.conf" /etc/autofs.conf
chmod 644 /etc/autofs.conf
chown root:root /etc/autofs.conf
mkdir -p /etc/auto.master.d
if ! grep -q "^+dir:/etc/auto.master.d" /etc/auto.master; then
    echo "+dir:/etc/auto.master.d" >> /etc/auto.master
fi
cp "$SCRIPT_DIR/files.autofs" /etc/auto.master.d/files.autofs
chmod 644 /etc/auto.master.d/files.autofs
chown root:root /etc/auto.master.d/files.autofs
# 644, not executable: autofs runs an executable map as a program
cp "$SCRIPT_DIR/auto.files" /etc/auto.files
chmod 644 /etc/auto.files
chown root:root /etc/auto.files

cp "$SCRIPT_DIR/sshfs_wrapper.sh" /root/sshfs_wrapper.sh
chmod +x /root/sshfs_wrapper.sh

## Share watchdog: detaches the shares while the files server is down
cp "$SCRIPT_DIR/share-watchdog.sh" /opt/scripts/share-watchdog.sh
chmod 755 /opt/scripts/share-watchdog.sh
chown root:root /opt/scripts/share-watchdog.sh
cp "$SCRIPT_DIR/share-watchdog.service" /etc/systemd/system/share-watchdog.service
chmod 644 /etc/systemd/system/share-watchdog.service
chown root:root /etc/systemd/system/share-watchdog.service
systemctl daemon-reload
# Enabled only: the old fstab automounts stay live until the reboot
systemctl enable autofs.service
systemctl enable share-watchdog.service

cp "$SCRIPT_DIR/bashrc" /home/ben/.bashrc
chmod +x /home/ben/.bashrc
chown ben:ben /home/ben/.bashrc

cp "$SCRIPT_DIR/toggle_split_screen.sh" /opt/scripts/toggle_split_screen.sh
chmod +x /opt/scripts/toggle_split_screen.sh

echo "Complete, reboot system for changes to take effect."
exit
