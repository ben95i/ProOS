#!/bin/bash
#######################################################
####### LED parser script by Ben Provenzano III #######
#######################################################

## Read Input Arguments
VARA=${1}
VARB=${2}
CURARGS="--no-progress-meter --silent --fail --max-time 5 --retry 1"
MENUFILE="/var/www/html/ram/mainmenu.txt"
LEVEL_HOST=""

if [[ -e "$MENUFILE" ]]; then
  ## Read file into array
  readarray LED_ARRAY < "$MENUFILE"
  NEWLINE=$'\n'
  for ELM in ${LED_ARRAY[@]}; do
    ## Split data by delimiter
    LINE=( ${ELM//|/ } )
    HOST="${LINE[0]}"  
    LEVEL_HOST="$HOST"
    STATE="${LINE[1]}"
    ## Append active hosts to array
    DATA+="${HOST}"
    DATA+="${NEWLINE}"
  done
  if [[ "$VARA" == "level" ]]; then
    ## Show brightness of last in list
    if [[ "$LEVEL_HOST" != "" ]]; then
      /usr/bin/curl $CURLARGS \
        "$LEVEL_HOST/api?var=&arg=level&action=leds"
    fi
  else
    ## Dispatch command to active hosts
    if [[ "$DATA" != "" ]]; then
      echo "$DATA" | xargs -P 5 -I % \
        /usr/bin/curl $CURLARGS \
          %"/api?var=$VARB&arg=$VARA&action=leds" > /dev/null 2>&1 &
    fi
  fi
else
  echo "mainmenu.txt not found."    
fi
exit