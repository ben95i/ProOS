#!/bin/bash
########################################################
## LEDgrid boot script by Ben Provenzano III ###########
########################################################
########################################################

## Setup LEDs
systemctl stop rpi-fcleds
systemctl stop rpi-opcserver
systemctl set-environment rpi_ledtype=`cat /opt/rpi/ledtype.txt`
systemctl set-environment rpi_ledgridrotate=180 ## 0, 90, 180, 270 degrees
systemctl set-environment rpi_ledmaxbright=40 ## 0 thru 255 levels
systemctl start rpi-opcserver

## Auto Wi-Fi Hotspot
systemctl start rpi-netdetect

exit