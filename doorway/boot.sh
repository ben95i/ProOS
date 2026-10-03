#!/bin/bash
########################################################
## CameraPi by Ben Provenzano III  #####################
########################################################

FILES_IP="192.168.1.4"

LDR_SENSOR_PIN="27"

## GPIO Services
systemctl set-environment rpi_ldrpin=$LDR_SENSOR_PIN
systemctl start rpi-ldrdetect
systemctl start rpi-gposerver

## RAM Disk
systemctl start rpi-ramdisk

## Attach Flash Drive
mkdir -p /media/usb0
mount /dev/sda1 /media/usb0

## Attach SMB Share
if ping -c 1 $FILES_IP > /dev/null 2> /dev/null
then
  systemctl start autofs
fi

## Google Drive
if [ ! -e "/media/usb0/rclone" ]; then
  echo "flash drive not connected, not starting rclone."
else
  systemctl start rpi-rclone
fi

## Camera RTSP Server
systemctl start rpi-rtspcam

sleep 10

## Motion Detection Server
systemctl start rpi-minimotion

## Network Watchdog
systemctl start rpi-netwatchdog

## Cameras Cleanup Timer
systemctl start rpi-camcleanup.timer

exit
