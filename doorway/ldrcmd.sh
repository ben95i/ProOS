#!/bin/bash

CURL_ARGS="--fail --no-buffer --max-time 3 --silent --retry 3 --retry-all-errors --retry-delay 1"
LOCALHOST="127.0.0.1"

IR_FILTER_PIN="17"

## rtspcam day/night control port (rtspcam -c, loopback only)
RTSPCAM_CONTROL_PORT="8556"

## Read Command-Line Argument
ARG_1="${1}"

## Sets the IR filter / IR LED pin (HIGH or LOW) through the local GPIO API
setIrPin() {
    /usr/bin/curl $CURL_ARGS "$LOCALHOST:8770/gpio?pin=$IR_FILTER_PIN&state=$1"
}

## Switches rtspcam's picture mode: night = greyscale, day = no correction.
## rtspcam saves the mode itself, so it survives an rtspcam restart.
setCameraMode() {
    /usr/bin/curl $CURL_ARGS "$LOCALHOST:$RTSPCAM_CONTROL_PORT/mode?state=$1"
}

case "$ARG_1" in

ir_on)
## Night: greyscale first, so the IR colour cast never reaches the stream,
## then remove the IR cut filter / enable the IR LEDs
setCameraMode night
setIrPin HIGH
exit
;;

ir_off)
## Day: restore the IR cut filter first, then colour, so the brief overlap
## shows grey rather than magenta
setIrPin LOW
IR_PIN_STATUS=$?
setCameraMode day
exit $IR_PIN_STATUS
;;

*)
echo "enter a valid command argument."
exit 1
;;
esac
