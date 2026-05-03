#!/bin/bash
###########################################################

## Arduino's Serial Port
ARDUINO_PORT="ttyUSB0"

## Start amplifier communication
ln -sf /dev/$ARDUINO_PORT /dev/zterm-tty
## Bluetooth audio
systemctl start hciuart
systemctl start bluetooth
systemctl start bt-agent@hci0
systemctl start bluealsa-aplay
## AirPlay Support
systemctl start rpi-airplay
## SMB file share client
systemctl start autofs
exit
