#!/bin/bash

ARDUINO_PORT="ttyAMA0"
UNO_FRM="BedroomHub"
## Update Arduino over USB
rm -rf "/opt/rpi/$UNO_FRM/build"
mkdir -p "/opt/rpi/$UNO_FRM/build"
arduino-cli -v compile --fqbn arduino:avr:uno \
  "/opt/rpi/$UNO_FRM/$UNO_FRM.ino" --build-path "/opt/rpi/$UNO_FRM/build"
arduino-cli -v upload -p "/dev/$ARDUINO_PORT" \
  --fqbn arduino:avr:uno --input-dir "/opt/rpi/$UNO_FRM/build"

exit