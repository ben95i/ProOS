#!/bin/bash
ARG="${1}"
if [ $ARG == "" ]; then
    systemctl set-environment rpi_fceffect=""
    systemctl stop rpi-fcleds
else
    systemctl set-environment rpi_fceffect=$ARG
    systemctl restart rpi-fcleds
fi
exit
