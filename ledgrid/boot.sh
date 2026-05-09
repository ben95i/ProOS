#!/bin/bash
########################################################
## LEDgrid boot script by Ben Provenzano III ###########
########################################################
########################################################

MAX_BRIGHTNESS="40" ## 0 thru 255 levels
LED_ROTATION="180" ## 0, 90, 180, 270 degrees
LED_TYPE=`cat /opt/rpi/ledtype.txt`

systemctl stop rpi-fcleds
systemctl stop rpi-opcserver
systemctl set-environment rpi_ledtype=$LED_TYPE
systemctl set-environment rpi_ledgridrotate=$LED_ROTATION
systemctl set-environment rpi_ledmaxbright=$MAX_BRIGHTNESS
systemctl start rpi-opcserver

exit