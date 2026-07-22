#!/bin/bash
## Save bookmarks file from RAM disk hourly

DS_PATH="/mnt/datastore/data/Documents"
RAM_BMKS="/mnt/ramdisk/bookmarks.txt"

if [ -f "$RAM_BMKS" ]; then
  echo "saving bookmarks to disk..."
  mv -f $DS_PATH/Bookmarks.txt $DS_PATH/.Bookmarks.bak
  rsync -a $RAM_BMKS $DS_PATH/Bookmarks.txt
  chown tank:shared $DS_PATH/Bookmarks.txt
  chmod 666 $DS_PATH/Bookmarks.txt
else
  echo "bookmarks file not found in RAM disk, restoring..."
  if [ -f "/mnt/ramdisk/sysout.txt" ]; then ## check if RAM disk exists
  	if [ -f "$DS_PATH/Bookmarks.txt" ]; then 
	  cp -fv $DS_PATH/Bookmarks.txt $RAM_BMKS
	  chown tank:shared $RAM_BMKS
    chmod 666 $RAM_BMKS
	  echo "restore complete."
    fi
  fi	
fi

exit