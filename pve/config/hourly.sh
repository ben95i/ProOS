#!/bin/bash
## Save bookmarks file from RAM disk hourly

RAM_USER="tank:shared"
RAM_DISK="/mnt/ramdisk"
DS_PATH="/mnt/datastore/data/Documents"
RAM_SYSOUT="$RAM_DISK/sysout.txt"

BKP_BMKS="$DS_PATH/Bookmarks.txt"
OLD_BMKS="$DS_PATH/.Bookmarks.bak"
RAM_BMKS="$RAM_DISK/bookmarks.save.txt"
LCK_BMKS="$RAM_DISK/bookmarks-lock.save.txt"

LISTS_BKP="$DS_PATH/Lists.txt"
LISTS_OLD="$DS_PATH/.Lists.bak"
RAM_LISTS="$RAM_DISK/lists.save.txt"
LCK_LISTS="$RAM_DISK/lists-lock.save.txt"
RAM_LISTS_PWD="$RAM_DISK/lists-pwd.save.txt"
BKP_LISTS_PWD="$DS_PATH/.ListsPassword.txt"

if [ -f "$RAM_SYSOUT" ]; then
  ## Bookmarks
  if [ -f "$RAM_BMKS" ]; then
    echo "saving bookmarks to disk..."
    cp -f $BKP_BMKS $OLD_BMKS
    rsync -a $RAM_BMKS $BKP_BMKS
    echo "setting permissions..."
    chown $RAM_USER $BKP_BMKS
  else
    echo "bookmarks file not found in RAM disk, restoring..."
    if [ -f "$BKP_BMKS" ]; then
      echo "creating lock file..."
      touch $LCK_BMKS
      echo "restoring bookmarks from backup..."
      cp -f $BKP_BMKS $RAM_BMKS
      echo "setting permissions..."
      chmod 644 $RAM_BMKS $LCK_BMKS
      chown $RAM_USER $RAM_BMKS $LCK_BMKS
    else
      echo "bookmarks backup not found, restore aborted!"
    fi
  fi
  ## Lists
  if [ -f "$RAM_LISTS" ]; then
    echo "saving lists password file to disk..."
    rsync -a $RAM_LISTS_PWD $BKP_LISTS_PWD
    echo "saving lists to disk..."
    cp -f $LISTS_BKP $LISTS_OLD
    rsync -a $RAM_LISTS $LISTS_BKP
    echo "setting permissions..."
    chown $RAM_USER $LISTS_BKP $BKP_LISTS_PWD
  else
    echo "lists file not found in RAM disk, restoring..."
    if [ -f "$LISTS_BKP" ]; then
      echo "creating lock file..."
      touch $LCK_LISTS
      if [ -f "$BKP_LISTS_PWD" ]; then
        echo "restoring lists passing from backup..."
        cp -f $BKP_LISTS_PWD $RAM_LISTS_PWD
      else
        echo "creating empty lists password file..."
        touch $RAM_LISTS_PWD
      fi
      echo "restoring lists from backup..."
      cp -f $LISTS_BKP $RAM_LISTS
      echo "setting permissions..."
      chmod 644 $RAM_LISTS $LCK_LISTS $RAM_LISTS_PWD 
      chown $RAM_USER $RAM_LISTS $LCK_LISTS $RAM_LISTS_PWD 
    else
      echo "lists backup not found, restore aborted!"
    fi
  fi
else
  echo "RAM disk not setup or file-monitor is not running, not executing!" 
fi

exit