#!/bin/bash
###########################################################
###### LED Effects Loop ## by Ben Provenzano III ##########
###########################################################
### (triggered by systemd) ################################

## Main Path
BIN_PATH=/opt/rpi/effects

## Split up input variable
LEDTYPE=${rpi_ledtype}
PRGM_TYPE=${rpi_fceffect%-*}
EFFECT=${rpi_fceffect#*-}
#echo "Set LEDs to $PRGM_TYPE program $EFFECT"

## Solid Color
if [ "$PRGM_TYPE" == "color" ]; then
  HEX_VALUE=$($BIN_PATH/colorscan $EFFECT)
  $BIN_PATH/writecolor $HEX_VALUE
  exit
fi
if [ "$PRGM_TYPE" == "hex" ]; then
  $BIN_PATH/writecolor $EFFECT
  exit
fi

## Video Processor
if [ "$PRGM_TYPE" == "mp4" ]; then 
  $BIN_PATH/mp4toleds $LEDTYPE $BIN_PATH/videos/$EFFECT.mp4  
  exit
fi

## Perl Effects
if [ "$PRGM_TYPE" == "perl" ]; then
  export PERL5LIB=$BIN_PATH
  /usr/bin/perl $BIN_PATH/$EFFECT.pl
  exit
fi

## Python Effects
if [ "$PRGM_TYPE" == "pyth" ]; then
  if [ "$EFFECT" == "rave"  ] || [ "$EFFECT" == "chase" ]; then
    /usr/bin/python3 $BIN_PATH/$EFFECT.py
    exit
  else
    /usr/bin/python3 $BIN_PATH/$EFFECT.py \
     --layout $BIN_PATH/wall.json -f 60
    exit
  fi
fi

exit