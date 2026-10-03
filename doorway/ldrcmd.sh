#!/bin/bash

CURL_ARGS="--fail --no-buffer --max-time 3 --silent --retry 3 --retry-all-errors --retry-delay 1"
LOCALHOST="127.0.0.1"

IR_FILTER_PIN="17"

## Read Command-Line Argument
ARG_1="${1}"

case "$ARG_1" in

ir_on)
## IR LEDs Enable Command 
/usr/bin/curl $CURL_ARGS "$LOCALHOST:8770/gpio?pin=$IR_FILTER_PIN&state=HIGH"
exit
;;

ir_off)
## IR LEDs Disable Command 
/usr/bin/curl $CURL_ARGS "$LOCALHOST:8770/gpio?pin=$IR_FILTER_PIN&state=LOW"
exit
;;

*)
echo "enter a valid command argument."
exit 1
;;
esac   
