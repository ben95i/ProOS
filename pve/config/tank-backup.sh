#!/bin/bash
###########################################################
###########################################################
### runs as root ###
## Backs up the tank datastore (Data, Regions, Media) to the external
## backup pools listed in /opt/bkpdrives.txt, then records each pool's date
## in the status file and triggers their detach.
## Usage: tank-backup.sh <backupstd|backupchk> <RAM disk path>
## e.g.   tank-backup.sh backupchk /mnt/ramdisk   (checksum compare)
## Run by apicmds.sh for the website's backupstd and backupchk commands.

CMD="${1}"
RAMDISK="${2}"

EXTBKPS="/mnt/extbkps"
STATUSFILE="$EXTBKPS/status.txt"
DRIVES_PATH="/opt/bkpdrives.txt"
TANK_ROOT="/mnt/datastore"
REGDATA="$TANK_ROOT/.regions"
TANK_DATA="$TANK_ROOT/data"
TANK_MEDIA="$TANK_ROOT/media"
PROOS_DIR="$TANK_DATA/Projects/ProOS"
RSYNC_BIN="nice -n 19 ionice -c 3 rsync"
RSYNC_OPTIONS="-aP --delete --delete-excluded"

function END_ROUTINE() {
  TRAILER=$(date)
  TRAILER+=" ("
  TRAILER+=$(hostname)
  TRAILER+=")"
  echo "$TRAILER"
  echo " "
  exit
}

function REQUIRE_RAMDISK() {
  if [ -z "$RAMDISK" ] || [ ! -d "$RAMDISK" ]; then
    echo "invalid or missing RAM disk path (argument 2)!"
    echo "aborting before operating on '/'."
    END_ROUTINE
  fi
}

## Store backup dates in array
CURBKPDTES=()
function SAVEBKPDATES () {
  local _POOL="$1"
  touch "$EXTBKPS/$_POOL/LastSynced.txt"
  local _CURDATE=$(date '+%F %r')
  CURBKPDTES+=( "$_POOL|${_CURDATE// /_}" )
}

## External Drive Backups ##
if [[ "$CMD" == "backupstd" || "$CMD" == "backupchk" ]]; then
  ## Standard or Checksum RSYNC
  if [ "$CMD" == "backupstd" ]; then
    CHECKSUM=""
  fi
  if [ "$CMD" == "backupchk" ]; then
    echo "checksum compare option selected, this backup will take awhile!"
    CHECKSUM="--checksum"
  fi
  REQUIRE_RAMDISK
  WAIT_TIME=60
  if ! ( set -o noclobber; : > "$RAMDISK/backupsvr.lock" ) 2>/dev/null; then
    echo "already running!"
    echo "delete '$RAMDISK/backupsvr.lock' to remove lock if program failed"
    END_ROUTINE
  fi
  trap 'rm -f "$RAMDISK/backupsvr.lock"' EXIT
  trap 'exit 143' TERM INT
  echo "****************** starting backup **********************"
  echo " "
  ## Turn on indicator LED
  curl -s -f 'http://ledwall.home/api?action=main&arg=whtledon' > /dev/null 2>&1 &
  ## Read Backup Drive Names
  if [ ! -e "$DRIVES_PATH" ]; then
    echo "drives file '$DRIVES_PATH' not found, aborting backup!"
    curl -s -f 'http://ledwall.home/api?action=main&arg=whtledoff' > /dev/null 2>&1
    END_ROUTINE
  fi
  readarray -t ZFSPOOLS < "$DRIVES_PATH"
  #######################
  ## RSYNC ##############
  for _POOL in "${ZFSPOOLS[@]}"; do
    ## Remove Invalid Characters
    POOL=$(echo $_POOL | sed -e 's/\r//g')
    if [ ! "$POOL" == "" ]; then
      ## USB Flash Drives
      if [ "${POOL::3}" == "usb" ]; then
        #################################
        if [ ! -e "$EXTBKPS/$POOL/Data" ]; then
          echo "flash drive not connected $POOL"
        else
          #### Data Share ####
          if [ ! -e "$PROOS_DIR" ]; then
            echo "Data' share not found!"
          else
            echo "syncing 'Data' share to $POOL drive..."
            $RSYNC_BIN $CHECKSUM --exclude="**CDs.zip" --exclude="**CDs.7z" --exclude="**USB.img" --exclude="**USB.7z" --exclude="**USB.zip" \
              --exclude="**HD.img" --exclude="**HD.7z" --exclude="**HD.zip" --exclude="**VM.img" --exclude="**VM.7z" --exclude="**VM.zip" \
              $TANK_DATA/ $EXTBKPS/$POOL/Data/ $RSYNC_OPTIONS
          fi
          #### Regions Share ####
          if [ ! -e "$REGDATA/DB" ]; then
            echo "'Regions' share not found!"
          else
            echo "syncing 'Regions' share to $POOL drive..."
            $RSYNC_BIN $CHECKSUM --exclude="Archive/ALUTqMiuxVtjfuair7WIgQ/" --exclude="Games/" $REGDATA/ $EXTBKPS/$POOL/.Regions/ $RSYNC_OPTIONS
          fi
          ##### END BACKUP #####
          SAVEBKPDATES "$POOL"
        fi
      fi
      ## Hard Drives
      if [ "${POOL::3}" == "hdd" ]; then
        #################################
        if [ ! -e "$EXTBKPS/$POOL/Data" ]; then
          echo "hard drive not connected $POOL"
        else      
          #### Data Share ####
          if [ ! -e "$PROOS_DIR" ]; then
            echo "Data' share not found!"
          else
            echo "syncing 'Data' share to $POOL drive..."
            $RSYNC_BIN $CHECKSUM $TANK_DATA/ $EXTBKPS/$POOL/Data/ $RSYNC_OPTIONS
          fi
          #### Regions Share ####
          if [ ! -e "$REGDATA/DB" ]; then
            echo "'Regions' share not found!"
          else
            echo "syncing 'Regions' share to $POOL drive..."
            $RSYNC_BIN $CHECKSUM $REGDATA/ $EXTBKPS/$POOL/.Regions/ $RSYNC_OPTIONS
          fi
          #### Media Share ####
          if [ ! -e "$TANK_MEDIA/Music" ]; then
            echo "'Media' share not found!"
          else
            echo "syncing 'Media' share to $POOL drive..."
            $RSYNC_BIN $CHECKSUM $TANK_MEDIA/ $EXTBKPS/$POOL/Media/ $RSYNC_OPTIONS
          fi
          ##### END BACKUP #####
          SAVEBKPDATES "$POOL"
        fi
      fi
      echo "" 
    fi  
  done
  ################################
  ## Write backup dates to file ##
  if [ ! -f "$STATUSFILE" ]; then
    if : > "$STATUSFILE" 2>/dev/null; then
      echo "created new status file '$STATUSFILE'."
    else
      echo "WARNING: cannot create '$STATUSFILE' -- backup dates not recorded!"
    fi
  fi
  if [ -f "$STATUSFILE" ]; then
    ## Read status file into array
    echo " " 
    echo "last backup dates: "
    readarray -t STATFILEARR < "$STATUSFILE"
    for STATFILE_LAST in "${STATFILEARR[@]}"; do
      echo "$STATFILE_LAST"
    done
    ## Backup last file
    cp -f "$STATUSFILE" "$STATUSFILE.bak"
    ## Delete status files content
    truncate -s 0 "$STATUSFILE"
    ## Read through each item in current drive status array
    for CURBKP_ITEM in "${CURBKPDTES[@]}"; do
      ## skip blank or malformed entries (must be DRIVE|DATE)
      [[ "$CURBKP_ITEM" == *"|"* ]] || continue
      ENTRY_FOUND=""
      CURBKP_DRIVE=${CURBKP_ITEM%|*}
      CURBKP_DATE=${CURBKP_ITEM#*|}
      ## Read through each item in status file array
      for STATIDX in "${!STATFILEARR[@]}"; do
        STATFILE_ITEM=${STATFILEARR[$STATIDX]}
        [[ "$STATFILE_ITEM" == *"|"* ]] || continue
        STATFILE_DRIVE=${STATFILE_ITEM%|*}
        STATFILE_DATE=${STATFILE_ITEM#*|}
        ## Scan for matching drive entry
        if [ "$STATFILE_DRIVE" == "$CURBKP_DRIVE" ]; then
          ## Update existing entry
          STATFILEARR[$STATIDX]="$STATFILE_DRIVE|$CURBKP_DATE"
          ENTRY_FOUND="yes"
          break
        fi
      done
      ## Add new entry if no exisiting data found
      if [ "$ENTRY_FOUND" != "yes" ]; then
        STATFILEARR+=("$CURBKP_DRIVE|$CURBKP_DATE")
      fi
    done
    ## Write array to new file
    echo " " 
    echo "current backup dates: "
    for NEWSTATFILE_ITEM in "${STATFILEARR[@]}"; do
      echo "$NEWSTATFILE_ITEM" >> "$STATUSFILE"
      echo "$NEWSTATFILE_ITEM"
    done
    echo " "
  fi
  echo "triggering drive detach in $WAIT_TIME second(s)."
  sleep $WAIT_TIME
  ## Unlock State File
  rm -f "$RAMDISK/backupsvr.lock"
  ## Write drive detach trigger file
  touch "$RAMDISK/detach_bkps.txt"
  ## Turn off indicator LED
  curl -s -f 'http://ledwall.home/api?action=main&arg=whtledoff' > /dev/null 2>&1 &
  echo "****************** backup complete **********************"
  END_ROUTINE
fi

### end ###

echo "unknown command!"
END_ROUTINE