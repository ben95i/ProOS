#!/bin/bash
###########################################################

## AirPlay Support
systemctl start nqptp
systemctl start rpi-airplay
## Bluetooth Audio
systemctl start hciuart
systemctl start bluetooth
systemctl start bt-agent@hci0
systemctl start bluealsa-aplay
## Reset Bluetooth Connections
sleep 5
/opt/rpi/init resetbt
## Disable Activity LED
echo none | tee /sys/class/leds/led0/trigger
## Disable HDMI (saves 30mA)
tvservice -o
exit
