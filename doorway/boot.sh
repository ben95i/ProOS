#!/bin/bash
##########################################
## Doorway Camera by Ben Provenzano III ##
##########################################

## GPIO Services
systemctl start rpi-gposerver

## RAM Disk
systemctl start rpi-ramdisk

## Camera Fallback Partition (made once by fallbackpart.sh): repaired first,
## since a power cut may have hit a write, then mounted for minimotion's
## fallback_dir. Its folder is made only on the mounted partition, so
## without it minimotion finds no folder and keeps the movie on the RAM disk.
fallbackMountDir="/mnt/fallback"
fallbackCameraDir="$fallbackMountDir/minimotion"
fallbackDevice=$(blkid -L camfallback)
if [ -n "$fallbackDevice" ] && ! mountpoint -q "$fallbackMountDir"; then
  mkdir -p "$fallbackMountDir"
  e2fsck -p "$fallbackDevice"
  fsckStatus=$?
  ## 4 and up: errors left that -p may not fix on its own
  if [ "$fsckStatus" -ge 4 ]; then
    e2fsck -f -y "$fallbackDevice"
    fsckStatus=$?
  fi
  if [ "$fsckStatus" -lt 4 ]; then
    mount -t ext4 -o noatime,nodev,nosuid,noexec,errors=remount-ro "$fallbackDevice" "$fallbackMountDir"
  else
    echo "Fallback partition $fallbackDevice could not be repaired, not mounted."
  fi
fi
## minimotion runs as root
if mountpoint -q "$fallbackMountDir"; then
  mkdir -p "$fallbackCameraDir"
  chown root:root "$fallbackCameraDir"
  chmod 755 "$fallbackCameraDir"
fi

## Attach SMB Share
FILES_IP="192.168.1.4"
if ping -c 1 $FILES_IP > /dev/null 2> /dev/null
then
  systemctl start autofs
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

## Cameras Cleanup (Google Drive size limit, settings in
## rpi-camcleanup.service.d/doorway.conf). The timer counts from the last
## run, so one run is started now, in the background.
systemctl start --no-block rpi-camcleanup.service
systemctl start rpi-camcleanup.timer

exit
