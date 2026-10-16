#!/bin/bash
# Note: For many Dell monitors, PBP mode changes the 'mh' or 'sl' values.
# On the U3223QE, 'sl' is often the mode indicator (0x00=Single, 0x24=PBP).
SL_VAL=$(ddcutil --sleep-multiplier .5 getvcp E9 | sed -n 's/.*sl=\([^,]*\).*/\1/p')
if [ "$SL_VAL" == "0x00" ]; then
    echo "Status: Single (Full Screen)"
    ## Set to split screen
    ddcutil --sleep-multiplier .5 setvcp E9 0x24
elif [ "$SL_VAL" == "0x24" ] || [ "$SL_VAL" == "0x23" ]; then
    echo "Status: PBP (Split Screen)"
    ## Set to single input
    ddcutil --sleep-multiplier .5 setvcp E9 0x00
else
    echo "Status: Unknown Mode (sl=$SL_VAL)"
fi
exit
