#!/bin/bash
########################################################
## LEDstrip boot script by Ben Provenzano III ##########
########################################################
########################################################

## LED Controller
usbreset 1d50:607a
systemctl stop rpi-fcleds
systemctl stop rpi-fcserver
systemctl stop rpi-fcproxy
LEDTYPE=`cat /opt/rpi/ledtype.txt`
systemctl set-environment rpi_ledtype=$LEDTYPE
systemctl restart rpi-fcserver
systemctl restart rpi-fcproxy

exit