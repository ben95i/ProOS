#!/bin/bash
## ProOS for RPi, Bootup Script by Ben Provenzano III
#### Ran by proinit.service on boot after network startup ####

### Set CPU clock speed scaling
echo "schedutil" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

### Website RAM folder: each Pi option command's output file (servercmds.sh)
mkdir -p /var/www/html/ram
chown www-data:www-data /var/www/html/ram

### Start network configuration website / automation API
systemctl --no-block start nodeapi.service
systemctl --no-block start lighttpd.service

## Console On Display
systemctl --no-block start getty@tty1.service

exit