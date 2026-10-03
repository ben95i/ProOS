#!/bin/bash
################################################
## LEDstrip boot script by Ben Provenzano III ##
################################################

## LED Controller
LEDTYPE=`cat /opt/rpi/ledtype.txt`
systemctl set-environment rpi_ledtype=$LEDTYPE
systemctl restart rpi-fcserver
systemctl restart rpi-fcproxy
systemctl restart rpi-mp4toleds

## Network Watchdog
systemctl restart rpi-netwatchdog

exit
