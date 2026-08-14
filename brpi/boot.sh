#!/bin/bash
#################################################
## BedroomPi boot script by Ben Provenzano III ##
#################################################

## AirPlay Support
systemctl restart rpi-airplay

## SMB file share client
systemctl restart autofs

## GPIO Services
systemctl restart rpi-gposerver

exit