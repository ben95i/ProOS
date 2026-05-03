#!/bin/bash

ARG_1="$1"
if [ "$ARG_1" == "stop" ]; then
  systemctl set-environment rpi_relaxmode=""
  systemctl stop rpi-relaxloop
else
  systemctl set-environment rpi_relaxmode="$ARG_1"
  systemctl start rpi-relaxloop
fi