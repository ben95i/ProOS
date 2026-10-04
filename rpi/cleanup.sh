#!/bin/bash
####### RPi Cleanup by Ben Provenzano III #######
## This script runs 5hr after boot and every day

## Cleanup logs
truncate -s 0 /var/log/wtmp
truncate -s 0 /var/log/btmp
truncate -s 0 /var/log/lastlog
truncate -s 0 /var/log/faillog
truncate -s 0 /var/log/Xorg.0.log.old
truncate -s 0 /var/log/Xorg.0.log
truncate -s 0 /root/.xsession-errors
truncate -s 0 /root/.bash_history
truncate -s 0 /home/pi/.xsession-errors
truncate -s 0 /home/pi/.bash_history
truncate -s 128k /tmp/netstat.txt

## Clean cache folders
#rm -rf /home/pi/.config/chromium/hyphen-data/*
#rm -rf /home/pi/.config/chromium/OnDeviceHeadSuggestModel/*
rm -rf /home/pi/.config/chromium/*
rm -rf /var/www/sessions/*

exit