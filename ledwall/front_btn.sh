#!/bin/bash
LOCALHOST="127.0.0.1"
CURL_ARGS="--fail --no-buffer --max-time 3 --silent --retry 3 --retry-all-errors --retry-delay 1"
/usr/bin/curl $CURL_ARGS "$LOCALHOST:3000/api?var=&arg=front_btn&action=main"
exit
