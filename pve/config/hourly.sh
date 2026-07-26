#!/bin/bash
## Save bookmarks file from RAM disk hourly

RAM_DISK="/mnt/ramdisk"
DS_PATH="/mnt/datastore/data/Documents"
BMK_BKP="$DS_PATH/Bookmarks.txt"
RAM_BMKS="$RAM_DISK/bookmarks.txt"

if [ -f "$RAM_BMKS" ]; then
  echo "saving bookmarks to disk..."
  cp -fv $BMK_BKP $DS_PATH/.Bookmarks.bak
  rsync -a $RAM_BMKS $BMK_BKP
  chown tank:shared $BMK_BKP
  chmod 666 $BMK_BKP
else
  echo "bookmarks file not found in RAM disk, restoring..."
  if [ -f "$RAM_DISK/sysout.txt" ]; then
  	if [ -f "$BMK_BKP" ]; then
    	echo "restoring bookmarks from backup..."
	    cp -fv $BMK_BKP $RAM_BMKS
	    chown tank:shared $RAM_BMKS
      chmod 666 $RAM_BMKS
    else
      echo "bookmarks backup not found, restore aborted!"
    fi
  else
    echo "RAM disk not connected, restore aborted!"
  fi	
fi

exit