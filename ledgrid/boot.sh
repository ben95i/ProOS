#!/bin/bash
########################################################
## LEDgrid boot script by Ben Provenzano III ###########
########################################################
########################################################

## LED Controller
MAX_BRIGHTNESS="50" ## Max is 255
LEDTYPE=`cat /opt/rpi/ledtype.txt`
systemctl stop rpi-fcleds
systemctl stop rpi-opcserver
systemctl set-environment rpi_ledtype=$LEDTYPE
systemctl set-environment rpi_ledmaxbright=$MAX_BRIGHTNESS
systemctl start rpi-opcserver

exit