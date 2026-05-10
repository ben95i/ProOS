#!/bin/bash
###########################################################
###### LED Effects Loop ## by Ben Provenzano III ##########
###########################################################
### (triggered by systemd) ################################

## Main Path
BIN_PATH="/opt/rpi/effects"

## Split up input variable
LEDTYPE=${rpi_ledtype}
RPI_EFFECT=${rpi_fceffect}
PRGM_TYPE=${RPI_EFFECT%-*}
EFFECT=${RPI_EFFECT#*-}

cd $BIN_PATH

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

## C / C++ Effects
if [ "$PRGM_TYPE" == "c" ]; then
  if [ "$EFFECT" == "spectro" ]; then
    MIC_GAIN="40"
    MIN_FREQ="350" ## in Hz
    MAX_FREQ="3500" ## in Hz
    $BIN_PATH/spectro $LEDTYPE $MIC_GAIN $MIN_FREQ $MAX_FREQ 
  else
    $BIN_PATH/$EFFECT $LEDTYPE
  fi
  exit
fi

## Python Effects
if [ "$PRGM_TYPE" == "pyth" ]; then
  if [ "$EFFECT" == "rave"  ] || \\
     [ "$EFFECT" == "chase" ]; then
    /usr/bin/python3 $BIN_PATH/$EFFECT.py
    exit
  else
    /usr/bin/python3 $BIN_PATH/$EFFECT.py \
     --layout $BIN_PATH/wall.json -f 60
    exit
  fi
fi

exit