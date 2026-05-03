#!/bin/bash
########################################################
## CameraPi by Ben Provenzano III  #####################
########################################################
########################################################

FILES_IP="192.168.1.4"
LDR_SENSOR_PIN="22"

## GPIO Services
systemctl set-environment rpi_ldrpin=$LDR_SENSOR_PIN
systemctl start rpi-ldrdetect
systemctl start rpi-gposerver

## Video RAM disk
mkdir -p /mnt/ramdisk
chmod 777 /mnt/ramdisk
mount -t tmpfs -o size=284m ramdisk /mnt/ramdisk

## Attach flash drive
mkdir -p /media/usb0
mount /dev/sda1 /media/usb0
sleep 5

## Attach server SMB share
if ping -c 1 $FILES_IP > /dev/null 2> /dev/null
then
  systemctl start autofs
  sleep 5
fi

## Google Drive
if [ ! -e "/media/usb0/rclone" ]; then
  echo "flash drive not connected, not starting rclone."
else
  systemctl start rpi-rclone
  sleep 5
fi

## Motion Server
systemctl start rpi-motion.service

## Cameras Cleanup Timer
systemctl start rpi-camcleanup.timer

exit
