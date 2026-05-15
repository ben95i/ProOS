#!/bin/bash
###############################################
## LEDgrid Boot Script by Ben Provenzano III ##
###############################################

## LED Controller
LEDTYPE=`cat /opt/rpi/ledtype.txt`
systemctl set-environment rpi_ledtype=$LEDTYPE
systemctl set-environment rpi_ledgridrotate=180 ## 0, 90, 180, 270 degrees
systemctl set-environment rpi_ledmaxbright=25 ## 0 thru 255
systemctl restart rpi-opcserver
systemctl restart rpi-mp4toleds

## Auto Wi-Fi Hotspot
systemctl restart rpi-netdetect

exit