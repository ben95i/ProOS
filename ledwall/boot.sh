#!/bin/bash
###############################################
## LEDwall boot script by Ben Provenzano III ##
###############################################

## GPIO Services
FRONT_BTN_PIN="13"
systemctl set-environment rpi_gpipin_1=$FRONT_BTN_PIN
systemctl set-environment rpi_gpicmd_1=front_btn
systemctl restart rpi-pinmon-1
systemctl restart rpi-gposerver

## LED Controller
LEDTYPE=`cat /opt/rpi/ledtype.txt`
systemctl set-environment rpi_ledtype=$LEDTYPE
systemctl restart rpi-fcserver
systemctl restart rpi-fcproxy
systemctl restart rpi-mp4toleds

exit