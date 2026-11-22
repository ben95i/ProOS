#!/bin/bash
######################################################
## Living Room Pi boot script by Ben Provenzano III ##
######################################################

## AirPlay Support
systemctl start rpi-airplay

## Bluetooth Audio
systemctl start hciuart
systemctl start bluetooth
systemctl start bt-agent@hci0
systemctl start bluealsa-aplay

exit
