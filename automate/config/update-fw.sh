#!/bin/bash
rm -rf /opt/fw-build
mkdir -p /opt/fw-build
arduino-cli -v compile --fqbn arduino:avr:uno /opt/BedroomHub/BedroomHub.ino --build-path /opt/fw-build
arduino-cli -v upload -p /dev/USB-Xmit0 --fqbn arduino:avr:uno --input-dir /opt/fw-build
exit