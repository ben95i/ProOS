#!/bin/bash
## Save bookmarks file from RAM disk hourly

RAM_DISK="/mnt/ramdisk"
DS_PATH="/mnt/datastore/data/Documents"

BKP_BMKS="$DS_PATH/Bookmarks.txt"
OLD_BMKS="$DS_PATH/.Bookmarks.bak"
RAM_BMKS="$RAM_DISK/bookmarks.save.txt"
LCK_BMKS="$RAM_DISK/bookmarks-lock.save.txt"
RAM_USER="tank:shared"

if [ -f "$RAM_BMKS" ]; then
  echo "saving bookmarks to disk..."
  cp -f $BKP_BMKS $OLD_BMKS
  rsync -a $RAM_BMKS $BKP_BMKS
  chown $RAM_USER $BKP_BMKS
else
  echo "bookmarks file not found in RAM disk, restoring..."
  if [ -f "$RAM_DISK/sysout.txt" ]; then
  	if [ -f "$BKP_BMKS" ]; then
      echo "creating lock file..."
      touch $LCK_BMKS
    	echo "restoring bookmarks from backup..."
	    cp -f $BKP_BMKS $RAM_BMKS
      chmod 644 $RAM_BMKS $LCK_BMKS
      chown $RAM_USER $RAM_BMKS $LCK_BMKS
    else
      echo "bookmarks backup not found, restore aborted!"
    fi
  else
    echo "RAM disk not connected, restore aborted!"
  fi	
fi

LISTS_BKP="$DS_PATH/Lists.txt"
LISTS_OLD="$DS_PATH/.Lists.bak"
RAM_LISTS="$RAM_DISK/lists.save.txt"
LCK_LISTS="$RAM_DISK/lists-lock.save.txt"

if [ -f "$RAM_LISTS" ]; then
  echo "saving lists to disk..."
  cp -f $LISTS_BKP $LISTS_OLD
  rsync -a $RAM_LISTS $LISTS_BKP
  chown $RAM_USER $LISTS_BKP
else
  echo "lists file not found in RAM disk, restoring..."
  if [ -f "$RAM_DISK/sysout.txt" ]; then
  	if [ -f "$LISTS_BKP" ]; then
      echo "creating lock file..."
      touch $LCK_LISTS
    	echo "restoring lists from backup..."
	    cp -f $LISTS_BKP $RAM_LISTS
      chmod 644 $RAM_LISTS $LCK_LISTS
      chown $RAM_USER $RAM_LISTS $LCK_LISTS
    else
      echo "lists backup not found, restore aborted!"
    fi
  else
    echo "RAM disk not connected, restore aborted!"
  fi	
fi

exit