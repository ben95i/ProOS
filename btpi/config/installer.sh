#!/bin/sh
## AirZero II Configuration Script v7.0

# Import Host name
HOSTNAME=`cat /opt/rpi/config/hostname`
## Set Module Path
BIN=/opt/rpi
# Import Brand name
MODNAME=`cat $BIN/config/module/brand.txt`

if [ ! -e "/opt/rpi/rpi.sh" ]; then
echo "Core components missing or corrupted."
echo "Exiting..."
sleep 1
exit
else
echo "Components integrity verified."
fi
echo "Starting $MODNAME configuration..."

## Copy Boot Config
mount -o remount,rw /boot/firmware
cp -f $BIN/config/module/config.txt /boot/firmware/
mount -o remount,ro /boot/firmware

## ALSA Configuration
cp -f $BIN/config/module/asound.conf /etc
chmod 644 /etc/asound.conf
chown root:root /etc/asound.conf

## Routed Hotspot Configuration
#cp -f /etc/dnsmasq.routed /etc/dnsmasq.conf
#chmod 644 /etc/dnsmasq.conf
#chown root:root /etc/dnsmasq.conf

exit
