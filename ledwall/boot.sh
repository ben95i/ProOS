#!/bin/bash
########################################################
## LEDwall boot script by Ben Provenzano III ###########
########################################################
########################################################

## External Serial COM
ARDUINO_PORT="ttyAMA0"
ln -sf /dev/$ARDUINO_PORT /dev/zterm-tty

## GPIO Services
FRONT_BTN_PIN="13"
systemctl set-environment rpi_gpipin_1=$FRONT_BTN_PIN
systemctl set-environment rpi_gpicmd_1=front_btn
systemctl start rpi-pinmon-1
systemctl start rpi-gposerver

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