#!/bin/bash
##
## Pi Web Server System Configuration API 
##

EXIT_ROUTINE() {
  TRAILER=$(date)
  TRAILER+=" ("
  TRAILER+=$(hostname)
  TRAILER+=")"
  echo "$TRAILER" &>> $LOGFILE
  echo " " &>> $LOGFILE
  exit
}

LOGFILE="/var/www/html/ram/sysout.txt"

ARG_1="$1"
ARG_2="$2"

if [ "$ARG_1" == "" ]; then
  echo "no argument." &>> $LOGFILE  
  EXIT_ROUTINE
fi

## Wi-Fi setup API
if [ "$ARG_1" == "confwpa" ]; then
  ## Decode base64
  BASE64_IN=$(echo "$ARG_2" | sed "s|-|+|g" | sed "s|_|/|g" | sed "s|@|=|g")
  WPADATA=$(openssl enc -base64 -d <<< "$BASE64_IN")
  rm -f /tmp/wifidata
  echo -e "$WPADATA" > /tmp/wifidata
  ## Copy configuration to boot partition
  /opt/rpi/rpi.sh cpwifi
  echo "set Wi-Fi configuration." &>> $LOGFILE
  echo " " &>> $LOGFILE
  EXIT_ROUTINE	
fi

## Server control
if [ "$ARG_1" == "clearlog" ]; then
  truncate -s 0 $LOGFILE
  fastfetch --pipe | sed 's/\x1B[@A-Z\\\]^_]\|\x1B\[[0-9:;<=>?]*[-!"#$%&'"'"'()*+,.\/]*[][\\@A-Z^_`a-z{|}~]//g' &>> $LOGFILE
  EXIT_ROUTINE
fi
if [ "$ARG_1" == "showwpa" ]; then
  echo "current Wi-Fi configuration:" &>> $LOGFILE
  cat /boot/firmware/wpa.conf &>> $LOGFILE
  echo " " &>> $LOGFILE
  echo "network configuration:" &>> $LOGFILE
  ifconfig &>> $LOGFILE
  EXIT_ROUTINE
fi

echo "sent $ARG_1 system command." &>> $LOGFILE
/opt/rpi/rpi.sh "$ARG_1" &>> $LOGFILE
EXIT_ROUTINE
