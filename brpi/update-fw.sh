#!/bin/bash

ARDUINO_PORT="ttyUSB0"
UNO_FRM="IntegratedAmp"
## Stop Serial Communication
rm -rfv /dev/zterm-tty
## Update Arduino over USB
rm -rf "/opt/rpi/$UNO_FRM/build"
mkdir -p "/opt/rpi/$UNO_FRM/build"
arduino-cli -v compile --fqbn arduino:avr:uno \
  "/opt/rpi/$UNO_FRM/$UNO_FRM.ino" --build-path "/opt/rpi/$UNO_FRM/build"
arduino-cli -v upload -p "/dev/$ARDUINO_PORT" \
  --fqbn arduino:avr:uno --input-dir "/opt/rpi/$UNO_FRM/build"
## Start amplifier communication
ln -sf "/dev/$ARDUINO_PORT" /dev/zterm-tty

exit