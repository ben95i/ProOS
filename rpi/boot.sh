#!/bin/bash
##

## Start X11 Server
#systemctl start rpi-x11
## AirPlay Support
#systemctl start rpi-airplay
## Bluetooth Audio
#systemctl start hciuart
#systemctl start bluetooth
#systemctl start bt-agent@hci0
#systemctl start bluealsa-aplay
## Reset Bluetooth Connections
#sleep 5
#/opt/rpi/rpi.sh resetbt
## Disable HDMI (saves 30mA of power draw)
#tvservice -o

exit