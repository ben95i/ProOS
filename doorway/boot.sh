#!/bin/bash
##########################################
## Doorway Camera by Ben Provenzano III ##
##########################################

## GPIO Services
systemctl start rpi-gposerver

## RAM Disk
systemctl start rpi-ramdisk

## Attach Flash Drive (and enable swap on it)
mkdir -p /media/usb0
if mountpoint -q /media/usb0 || mount /dev/sda1 /media/usb0; then
  /opt/rpi/swapspace.sh /media/usb0/swapfile
else
  echo "flash drive not mounted, swap not enabled."
fi

## Attach SMB Share
FILES_IP="192.168.1.4"
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
systemctl set-environment rpi_imxout_w=1280
systemctl set-environment rpi_imxout_h=720
systemctl set-environment rpi_imxfps=15
systemctl set-environment rpi_imxbps=1200000
systemctl set-environment rpi_imxsm=1920x1080:10
systemctl set-environment rpi_imxrotate=90
systemctl start rpi-rtspcam

sleep 5

## LDR Light Sensor
LDR_SENSOR_PIN="27"
systemctl set-environment rpi_ldrpin=$LDR_SENSOR_PIN
systemctl set-environment rpi_ldrnight=200000
systemctl set-environment rpi_ldrday=100000
systemctl start rpi-ldrdetect

sleep 5

## Motion Detection Server
systemctl start rpi-minimotion

## Network Watchdog
systemctl start rpi-netwatchdog

## Cameras Cleanup Timer
systemctl start rpi-camcleanup.timer

exit
