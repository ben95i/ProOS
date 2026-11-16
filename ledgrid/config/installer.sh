#!/bin/sh
## LEDgrid Configuration Script

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
cp -f $BIN/config/module/config.txt /boot/firmware/
mount -o remount,ro /boot/firmware

## Set LED Type
rm -f /opt/rpi/ledtype.txt
echo "16x16grid" > /opt/rpi/ledtype.txt

## Enable Pixel Menu in Website
sed -i 's/^export const LED_PI = false;$/export const LED_PI = true;/' /var/www/html/opts.js
if ! grep -q '^export const LED_PI = true;$' /var/www/html/opts.js; then
  echo "Pixel menu not enabled: no LED_PI line in /var/www/html/opts.js"
fi

exit
