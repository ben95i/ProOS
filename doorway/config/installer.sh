#!/bin/bash
## AptCam Configuration Script

# Import Host name
HOSTNAME=`cat /opt/rpi/config/hostname`
## Module Path
BIN=/opt/rpi
# Import Module Name
MODNAME=`cat $BIN/config/module/brand.txt`

if [ ! -e "/opt/rpi/rpi.sh" ]; then
  echo "Core components missing or corrupted."
  echo "Exiting..."
  exit
fi
echo "Starting $MODNAME configuration..."

## Copy Boot Config
mount -o remount,rw /boot/firmware
cp -f $BIN/config/module/config.txt /boot/firmware/
mount -o remount,ro /boot/firmware

## Pi Permissions
rm -f /etc/sudoers.d/010_pi-perms
rm -f /etc/sudoers.d/010_pi-nopasswd
sh -c "touch /etc/sudoers.d/010_pi-nopasswd"
sh -c "echo \"pi ALL=(ALL) NOPASSWD: ALL\" >> /etc/sudoers.d/010_pi-nopasswd"
chown root:root /etc/sudoers.d/010_pi-nopasswd
chmod u=rwx,g=rx,o=rx /etc/sudoers.d/010_pi-nopasswd
chmod u=r,g=r,o= /etc/sudoers.d/010_pi-nopasswd

## AutoFS Configuration
cp -f $BIN/config/module/auto.master /etc/
chmod 644 /etc/auto.master
chown root:root /etc/auto.master
cp -f $BIN/config/module/auto.map /etc/
chmod 644 /etc/auto.map
chown root:root /etc/auto.map
cp -f $BIN/config/module/auto.creds /etc/
chmod 400 /etc/auto.creds
chown root:root /etc/auto.creds
mkdir -p /mnt/smb

## Cameras Cleanup Settings (size limit for the clips on Google Drive)
cp -f $BIN/config/module/camcleanup.conf /etc/camcleanup.conf
chmod 644 /etc/camcleanup.conf
chown root:root /etc/camcleanup.conf

## Remove the USB flash drive copy script (the drive was removed)
rm -f /usr/bin/cpusb
rm -f /opt/rpi/cpusb.sh

exit
