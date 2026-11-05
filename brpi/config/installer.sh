#!/bin/sh
## Bedroom Pi Configuration Script

# Import Host name
HOSTNAME=`cat /opt/rpi/config/hostname`
## Set Module Path
BIN=/opt/rpi
# Import Brand name
MODNAME=`cat $BIN/config/module/brand.txt`

if [ ! -e "/opt/rpi/rpi.sh" ]; then
  echo "Core components missing or corrupted."
  echo "Exiting..."
  exit
fi
echo "Starting $MODNAME configuration..."

## Copy Boot Config
mount -o remount,rw /boot/firmware
cp -fv $BIN/config/module/config.txt /boot/firmware/
## Disable Wi-Fi
touch /boot/firmware/disable.wifi
mount -o remount,ro /boot/firmware

## ALSA Configuration
cp -f $BIN/config/module/asound.conf /etc/
chmod 644 /etc/asound.conf
chown root:root /etc/asound.conf

## AutoFS configuration
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

## Arduino Command Line (flashes the amp and the bedroom hub)
## The base installer removes arduino-cli from any module whose installer does
## not install it.
if [ ! -e "/usr/bin/arduino-cli" ]; then
  echo "Installing Arduino command line..."
  curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=/usr/bin sh
  arduino-cli version
  arduino-cli core update-index
  arduino-cli core install arduino:avr
  arduino-cli lib install Encoder@1.4.4
fi

## Custom Node API
cp -f $BIN/config/module/custom.js /opt/nodeapi/
chmod 644 /opt/nodeapi/custom.js
chown www-data:www-data /opt/nodeapi/custom.js

exit
