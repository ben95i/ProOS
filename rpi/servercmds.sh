#!/bin/bash
##
## Pi Web Server System Configuration API
##
## Run by the node API (rpi/controller.js) through sudo for the Pi options
## panel: servercmds.sh <command> [argument]. Each command's output goes to
## its own file, ram/<command>.out.save.txt (wifiscan -> wifiscan.out.save.txt),
## emptied first so it holds only the newest run, which the website reads
## through /api/read?file=<command>.out.save. Two runs of one command take
## turns under a lock on that file. As on pve (file-monitor.sh), the run's
## output sits between two marker lines, by which the website's /api/svrcmd
## (shared/nodeapi/svrcmd.js) follows it:
##   @START|<command>|<epoch ms>
##   @END|<command>|<exit code>|<epoch ms>
##   confwpa <base64>   saves the Wi-Fi network and password (Setup WiFi)
##   showwpa            the saved Wi-Fi settings and the network interfaces
##   <rpi command>      anything else is run as rpi.sh <command> (client, apd,
##                      wifiscan, temp, resetbt ...)
##

ramPath="/var/www/html/ram"
outputOwner="www-data:www-data"
outputMode="0644"

commandName="$1"
commandArg="$2"

## Prints the date and host name after a command's output and ends the script.
## Usage: EXIT_ROUTINE
EXIT_ROUTINE() {
  local trailerText
  trailerText=$(date)
  trailerText+=" ("
  trailerText+=$(hostname)
  trailerText+=")"
  echo "$trailerText"
  echo " "
  exit
}

## Sends the rest of the script's output to the command's own file: creates
## it with the website's owner, waits for any other run of the same command,
## empties it and writes the @START line; the @END line is written as the
## script exits (FINISH_OUTPUT). lockFd stays open until the script ends.
## Usage: OUTPUT_TO_FILE <command>   e.g. OUTPUT_TO_FILE wifiscan
OUTPUT_TO_FILE() {
  local outputPath="$ramPath/$1.out.save.txt"
  local startTime
  : >> "$outputPath"
  chown "$outputOwner" "$outputPath" 2>/dev/null
  chmod "$outputMode"  "$outputPath" 2>/dev/null
  exec {lockFd}>>"$outputPath"
  flock "$lockFd"
  : > "$outputPath"
  exec >> "$outputPath" 2>&1
  startTime=$(date +%s%3N)
  printf '@START|%s|%s\n' "$1" "$startTime"
  trap FINISH_OUTPUT EXIT
  ## the API's timeout stops the script with TERM: exit, so the @END line is written
  trap 'exit 143' TERM
}

## The EXIT trap: writes the run's @END line with the script's exit code.
## Usage: trap FINISH_OUTPUT EXIT
FINISH_OUTPUT() {
  local exitCode=$?
  local endTime
  endTime=$(date +%s%3N)
  printf '@END|%s|%s|%s\n' "$commandName" "$exitCode" "$endTime"
}

## No command: nothing to name a file after, so the answer goes back to the API.
if [ "$commandName" == "" ]; then
  echo "no argument."
  exit
fi

OUTPUT_TO_FILE "$commandName"

## Wi-Fi setup API
if [ "$commandName" == "confwpa" ]; then
  ## Decode base64
  base64Text=$(echo "$commandArg" | sed "s|-|+|g" | sed "s|_|/|g" | sed "s|@|=|g")
  wpaData=$(openssl enc -base64 -d <<< "$base64Text")
  rm -f /tmp/wifidata
  echo -e "$wpaData" > /tmp/wifidata
  ## Copy configuration to boot partition
  /opt/rpi/rpi.sh cpwifi
  echo "set Wi-Fi configuration, select Client Mode to apply it."
  echo " "
  EXIT_ROUTINE
fi

if [ "$commandName" == "showwpa" ]; then
  echo "current Wi-Fi configuration:"
  cat /boot/firmware/wpa.conf
  echo " "
  echo "network configuration:"
  ifconfig
  EXIT_ROUTINE
fi

## rpi.sh gets the lock's descriptor closed ({lockFd}>&-): a service or
## process it starts keeps running after it, and must not keep holding the lock.
echo "sent $commandName system command."
/opt/rpi/rpi.sh "$commandName" {lockFd}>&-
EXIT_ROUTINE
