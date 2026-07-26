#!/bin/bash
###########################################################
###########################################################
### runs as root ###

CMD="${1}"
RAMDISK="${2}"

EXTBKPS="/mnt/extbkps"
STATUSFILE="$EXTBKPS/status.txt"
DRIVES_PATH="/opt/bkpdrives.txt"
TANK_ROOT="/mnt/datastore"
REGDATA="$TANK_ROOT/.regions"
BINDMNTS="/mnt/bindmnts"
TANK_DATA="$TANK_ROOT/data"
TANK_MEDIA="$TANK_ROOT/media"
TANK_SNAPS="$TANK_ROOT/.zfs/snapshot"
PROOS_DIR="$TANK_DATA/ProOS"

function END_ROUTINE() {
  TRAILER=$(date)
  TRAILER+=" ("
  TRAILER+=$(hostname)
  TRAILER+=")"
  echo "$TRAILER"
  echo " "
  exit
}

function PATHMOUNTED() { 
  findmnt --mountpoint "$1" >/dev/null 2>&1
}

function UMNT_FUSEFS () {
  VOLNME="$1"
  FUSEPTH="$BINDMNTS/$VOLNME"
  if PATHMOUNTED "$FUSEPTH"
  then
    UNCAPSTR="${VOLNME,,}"
    echo "detaching $UNCAPSTR..."
    fusermount -u "$FUSEPTH"
    sleep 1
    rmdir "$FUSEPTH"
  else 
    echo "FUSE volume not attached."
  fi
}

function MNT_FUSEFS () {
  VOLNME="$1"
  FUSEPTH="$BINDMNTS/$VOLNME"
  FUSEPWD="$RAMDISK/fusearch.txt"
  UMNT_FUSEFS "$VOLNME"
  UNCAPSTR="${VOLNME,,}"
  echo "attaching $UNCAPSTR..."
  mkdir -p "$FUSEPTH"
  if [ -e "$FUSEPWD" ]; then
    gocryptfs -quiet -allow_other \
      "$REGDATA/$VOLNME" \
      "$FUSEPTH" -passfile "$FUSEPWD"
  else
    echo "password file not found!"
  fi
  if [ -e "$FUSEPWD" ]; then
    rm -f "$FUSEPWD"
  fi
}

function MOUNT_SNAP(){
  if ! PATHMOUNTED "$BINDMNTS/Snapshots"; then
    echo "attaching ZFS snapshots region..."
    mkdir -p "$BINDMNTS/Snapshots"
    mount --bind "$TANK_SNAPS" "$BINDMNTS/Snapshots"
  else
    echo "snapshots already attached."
  fi
}
function UMOUNT_SNAP(){
  if PATHMOUNTED "$BINDMNTS/Snapshots"; then
    echo "detaching ZFS snapshots region..."
    umount -l "$BINDMNTS/Snapshots"
    rmdir "$BINDMNTS/Snapshots"
  else
    echo "snapshots not attached." 
  fi
}

function MOUNT_RAMDSK(){
  if ! PATHMOUNTED "$BINDMNTS/RAM"; then
    echo "attaching RAM disk region..."
    mkdir -p "$BINDMNTS/RAM"
    mount --bind "$RAMDISK" "$BINDMNTS/RAM"
  else
    echo "RAM already attached."    
  fi
}
function UMOUNT_RAMDSK(){
  if PATHMOUNTED "$BINDMNTS/RAM"; then
    echo "detaching RAM disk region..."
    umount -l "$BINDMNTS/RAM"
    rmdir "$BINDMNTS/RAM"
  else
    echo "RAM not attached."    
  fi
}

## Store backup dates in array
CURBKPDTES=()
function SAVEBKPDATES () {
  local _POOL="$1"
  touch $EXTBKPS/$_POOL/LastSynced.txt
  local _LASTSYNC=$(date -r $EXTBKPS/$_POOL/LastSynced.txt '+%F %r')
  local _CURBKPLNE=$(echo "$_POOL|$_LASTSYNC" | sed -e 's/ /_/g')
  CURBKPDTES+=( $_CURBKPLNE )
}

if [ "$CMD" == "clearlog" ]; then
  truncate -s 0 $RAMDISK/sysout.txt
  timeout 30 bash -c "fastfetch --pipe | sed -E 's/\x1b\[[0-9;]*[a-zA-Z]//g' | tr -cd '\11\12\15\40-\176'"
  END_ROUTINE
fi

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
  ## Time to wait for PVE response
  WAIT_TIME=60
  ### Lock file -- noclobber makes create-if-absent atomic, so two
  ### backup triggers arriving together cannot both pass the check.
  if ! ( set -o noclobber; : > "$RAMDISK/backupsvr.lock" ) 2>/dev/null; then
    echo "already running!"
    echo "delete '$RAMDISK/backupsvr.lock' to remove lock if program failed"
    END_ROUTINE
  fi
  ## release the lock on any exit path; convert TERM/INT into a normal
  ## exit so the EXIT trap fires instead of swallowing the signal
  trap 'rm -f "$RAMDISK/backupsvr.lock"' EXIT
  trap 'exit 143' TERM INT
  echo "****************** starting backup **********************"
  echo " "
  ## Read Backup Drive Names
  readarray -t ZFSPOOLS < "$DRIVES_PATH"
  #######################
  ## RSYNC ##############
  for _POOL in "${ZFSPOOLS[@]}"; do
    ## Remove Invalid Characters
    POOL=$(echo $_POOL | sed -e 's/\r//g')
    if [ ! "$POOL" == "" ]; then
      ## USB Flash Drives
      if [ ${POOL::3} == "usb" ]; then
        #################################
        if [ ! -e "$EXTBKPS/$POOL/Data" ]; then
          echo "flash drive not connected $POOL"
        else
          #### Data Share ####
          if [ ! -e "$TANK_DATA/ProOS" ]; then
            echo "Data' share not found!"
          else
            echo "syncing 'Data' share to $POOL drive..."
            nice -n 19 ionice -c 3 rsync $CHECKSUM -aP  \
              --exclude="**CDs.zip" --exclude="**CDs.7z" \
              --exclude="**USB.img" --exclude="**USB.7z" --exclude="**USB.zip" \
              --exclude="**HD.img" --exclude="**HD.7z" --exclude="**HD.zip" \
              --exclude="**VM.img" --exclude="**VM.7z" --exclude="**VM.zip" \
                $TANK_DATA/ $EXTBKPS/$POOL/Data/ --delete --delete-excluded
          fi
          #### Regions Share ####
          if [ ! -e "$REGDATA/DB" ]; then
            echo "'Regions' share not found!"
          else
            echo "syncing 'Regions' share to $POOL drive..."
            nice -n 19 ionice -c 3 rsync $CHECKSUM -aP \
              --exclude="Archive/ALUTqMiuxVtjfuair7WIgQ/" --exclude="Games/" \
                $REGDATA/ $EXTBKPS/$POOL/.Regions/ --delete --delete-excluded
          fi
          ##### END BACKUP #####
          SAVEBKPDATES "$POOL"
        fi
      fi
      ## Hard Drives
      if [ ${POOL::3} == "hdd" ]; then
        #################################
        if [ ! -e "$EXTBKPS/$POOL/Data" ]; then
          echo "hard drive not connected $POOL"
        else      
          #### Data Share ####
          if [ ! -e "$TANK_DATA/ProOS" ]; then
            echo "Data' share not found!"
          else
            echo "syncing 'Data' share to $POOL drive..."
            nice -n 19 ionice -c 3 rsync $CHECKSUM -aP \
              $TANK_DATA/ $EXTBKPS/$POOL/Data/ --delete --delete-excluded
          fi
          #### Regions Share ####
          if [ ! -e "$REGDATA/DB" ]; then
            echo "'Regions' share not found!"
          else
            echo "syncing 'Regions' share to $POOL drive..."
            nice -n 19 ionice -c 3 rsync $CHECKSUM -aP \
              $REGDATA/ $EXTBKPS/$POOL/.Regions/ --delete --delete-excluded
          fi
          #### Media Share ####
          if [ ! -e "$TANK_MEDIA/Music" ]; then
            echo "'Media' share not found!"
          else
            echo "syncing 'Media' share to $POOL drive..."
            nice -n 19 ionice -c 3 rsync -aP \
              $TANK_MEDIA/ $EXTBKPS/$POOL/Media/ --delete --delete-excluded
          fi
          ##### END BACKUP #####
          SAVEBKPDATES "$POOL"
        fi
      fi
      echo "" 
    fi  
  done
  ################################
  ################################
  ## Write backup dates to file ##
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
    for CURBKP_ITEM in ${CURBKPDTES[@]}; do
      ENTRY_FOUND=""
      CURBKP_DRIVE=${CURBKP_ITEM%|*}
      CURBKP_DATE=${CURBKP_ITEM#*|}
      ## Read through each item in status file array
      for STATIDX in "${!STATFILEARR[@]}"; do
        STATFILE_ITEM=${STATFILEARR[$STATIDX]}
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
  ################################
  ################################
  ## Wait for drives to settle after backup complete ##
  echo "triggering drive detach in $WAIT_TIME second(s)."
  sleep $WAIT_TIME
  ## Unlock State File
  rm -f $RAMDISK/backupsvr.lock
  ## Write drive detach trigger file
  touch $RAMDISK/detach_bkps.txt
  echo "****************** backup complete **********************"
  END_ROUTINE
fi

if [ "$CMD" == "last_backup_dates" ]; then
  echo " " 
  echo "last backup dates: "
  readarray -t LSTBKPARR < "$STATUSFILE"
  for LSTBKPLINE in "${LSTBKPARR[@]}"; do
    echo "$LSTBKPLINE"
  done
  END_ROUTINE
fi

## Detach All Regions
if [ "$CMD" == "unmnt_all" ]; then
  UMOUNT_RAMDSK
  UMOUNT_SNAP
  UMNT_FUSEFS "Archive"
  echo "detached all region(s)."
  END_ROUTINE
fi

## Archive Region
if [ "$CMD" == "mnt_arch_region" ]; then
  MNT_FUSEFS "Archive"
  END_ROUTINE
fi
if [ "$CMD" == "unmnt_arch_region" ]; then
  UMNT_FUSEFS "Archive"
  END_ROUTINE
fi

## RAM Disk Region
if [ "$CMD" == "mnt_ram_region" ]; then
  MOUNT_RAMDSK
  END_ROUTINE
fi
if [ "$CMD" == "unmnt_ram_region" ]; then
  UMOUNT_RAMDSK
  END_ROUTINE
fi

## Snapshots Region
if [ "$CMD" == "mnt_snap_region" ]; then
  MOUNT_SNAP
  END_ROUTINE
fi
if [ "$CMD" == "unmnt_snap_region" ]; then
  UMOUNT_SNAP
  END_ROUTINE
fi

### IMPORT ZFS ########################################   
if [ "$CMD" == "attach_bkps" ]; then
  if [ ! -e "$DRIVES_PATH" ]; then
	  echo "drives file not found, aborting!"
	else
    readarray -t ZFSPOOLS < "$DRIVES_PATH"
    for _POOL in "${ZFSPOOLS[@]}"; do
      POOL=$(echo $_POOL | sed -e 's/\r//g')
      if [ ! "$POOL" == "" ]; then
        if [[ "$POOL" == "tank" ]] || \
          [[ "$POOL" == "datastore" ]] || \
          [[ "$POOL" == "rpool" ]]; then
          echo "invalid pool specified."
        else
          echo "importing ZFS backup volume $POOL."
          zpool import $POOL
          zfs load-key -L file://$RAMDISK/pwd.txt $POOL/extbkp
          zfs mount $POOL/extbkp
          zpool status $POOL
        fi
        echo " "
      fi
    done
  fi
  zfs list
  echo " "
  if [ -e "$RAMDISK/pwd.txt" ]; then
    shred -n 2 -z -u "$RAMDISK/pwd.txt"
  fi
  END_ROUTINE
fi

### EXPORT ZFS ########################################
if [ "$CMD" == "detach_bkps" ]; then
  if [ ! -e "$DRIVES_PATH" ]; then
	  echo "drives file not found, aborting!"
  else
    readarray -t ZFSPOOLS < "$DRIVES_PATH"
    for _POOL in "${ZFSPOOLS[@]}"; do
      POOL=$(echo $_POOL | sed -e 's/\r//g')
      if [ ! "$POOL" == "" ]; then
        if [[ "$POOL" == "tank" ]] || \
           [[ "$POOL" == "datastore" ]] || \
           [[ "$POOL" == "rpool" ]] ; then
          echo "invalid pool specified."
        else
          echo "unmounting ZFS backup volume $POOL."
          zfs unmount "$EXTBKPS/$POOL"
          zpool export $POOL
        fi
        echo " "
      fi  
    done 
  fi
  zfs unload-key -a
  zpool status
  zfs list
  END_ROUTINE
fi

## List ZFS Snapshots
if [ "$CMD" == "pve_listsnaps" ]; then
  echo "Snapshots on ZFS pool (tank/datastore):"
  zfs list -t snapshot tank/datastore | grep -o '^\S*'
  END_ROUTINE
fi

### START LXC/VMs #####################################
if [ "$CMD" == "startlegacy" ]; then
  echo "starting legacy SMB server..."
  lxc-start -n 104
  END_ROUTINE
fi
if [ "$CMD" == "stoplegacy" ]; then
  echo "shutting down legacy SMB server..."
  lxc-stop -n 104
  END_ROUTINE
fi
#######################################################
if [ "$CMD" == "startxana" ]; then
  echo "starting xana..."
  qm start 105
  END_ROUTINE
fi
if [ "$CMD" == "stopxana" ]; then
  echo "shutting down xana..."
  qm stop 105
  END_ROUTINE
fi
if [ "$CMD" == "restorexana" ]; then
  echo "restoring xana..."
  qmrestore /opt/xana-restore-image.vma.zst \
    105 -force -storage scratch
  END_ROUTINE
fi
#######################################################
if [ "$CMD" == "startunifi" ]; then
  echo "starting unifi..."
  qm start 107
  END_ROUTINE
fi
if [ "$CMD" == "stopunifi" ]; then
  echo "shutting down unifi..."
  qm stop 107
  END_ROUTINE
fi
#######################################################
if [ "$CMD" == "startatalk" ]; then
  echo "starting AppleTalk server..."
  qm start 103
  END_ROUTINE
fi
if [ "$CMD" == "stopatalk" ]; then
  echo "shutting down AppleTalk server..."
  qm stop 103
  END_ROUTINE
fi
#######################################################
if [ "$CMD" == "rebootrouter" ]; then
  echo "rebooting router, may take up to 5-minutes."
  qm reboot 100
  END_ROUTINE
fi
### Write Server Log ##################################   
if [ "$CMD" == "syslog" ]; then
  /usr/bin/sys-check
  END_ROUTINE
fi
  ###### Server VM Backup Script ######################
if [ "$CMD" == "pve_vmsbkp" ]; then
  ### Lock file -- atomic create via noclobber (see backup lock above)
  if ! ( set -o noclobber; : > "$RAMDISK/pvebackup.lock" ) 2>/dev/null; then
    echo "PVE backup already running!"
    echo "delete '$RAMDISK/pvebackup.lock' to remove lock if program aborted."
    END_ROUTINE
  fi
  trap 'rm -f "$RAMDISK/pvebackup.lock"' EXIT
  trap 'exit 143' TERM INT
  echo " "
  echo "Backing-up Files LXC 101..."
  vzdump 101 --mode snapshot --compress zstd --node pve --storage local --remove 1 --exclude-path /mnt --exclude-path /home/ben
  cp /etc/pve/lxc/101.conf $PROOS_DIR/files/lxc.conf
  chmod 777 $PROOS_DIR/files/lxc.conf
  ###
  echo " "
  echo "Backing-up Management LXC 102..."
  vzdump 102 --mode snapshot --compress zstd --node pve --storage local --remove 1 --exclude-path /mnt
  cp /etc/pve/lxc/102.conf $PROOS_DIR/mgmt/lxc.conf
  chmod 777 $PROOS_DIR/mgmt/lxc.conf
  ### 
  echo " "
  echo "Backing-up Legacy LXC 104..."
  vzdump 104 --mode snapshot --compress zstd --node pve --storage local --remove 1 --exclude-path /mnt
  cp -v /etc/pve/lxc/104.conf $PROOS_DIR/legacy/lxc.conf
  chmod 777 $PROOS_DIR/legacy/lxc.conf
  ###
  echo " "
  echo "Backing-up Automate LXC 106..."
  vzdump 106 --mode snapshot --compress zstd --node pve --storage local --remove 1 --exclude-path /mnt --exclude-path /var/www/html
  cp -v /etc/pve/lxc/106.conf $PROOS_DIR/automate/lxc.conf
  chmod 777 $PROOS_DIR/automate/lxc.conf
  ###
  echo " "
  echo "Backing-up Media LXC 108..."
  vzdump 108 --mode snapshot --compress zstd --node pve --storage local --remove 1 --exclude-path /mnt
  cp /etc/pve/lxc/108.conf $PROOS_DIR/media/lxc.conf
  chmod 777 $PROOS_DIR/media/lxc.conf
  echo " "
  echo "Backing-up Router KVM 100..."
  vzdump 100 --mode snapshot --compress zstd --node pve --storage local --remove 1 --exclude-path /mnt
  cp /etc/pve/qemu-server/100.conf $PROOS_DIR/pve/vmbkps/vzdump-qemu-100.conf
  chmod 777 $PROOS_DIR/pve/vmbkps/vzdump-qemu-100.conf
  ###
  echo " "
  echo "Backing-up AppleTalk KVM 103..."
  vzdump 103 --mode snapshot --compress zstd --node pve --storage local --remove 1 --exclude-path /mnt
  cp /etc/pve/qemu-server/103.conf $PROOS_DIR/appletalk/qemu.conf
  chmod 777 $PROOS_DIR/appletalk/qemu.conf
  ###  
  echo " "
  echo "Backing-up Xana KVM 105..."
  vzdump 105 --mode snapshot --compress zstd --node pve --storage local --remove 1 --exclude-path /mnt
  cp /etc/pve/qemu-server/105.conf $PROOS_DIR/xana/qemu.conf
  chmod 777 $PROOS_DIR/xana/qemu.conf
  ###
  echo " "
  echo "Backing-up Unifi KVM 107..."
  vzdump 107 --mode snapshot --compress zstd --node pve --storage local --remove 1
  cp /etc/pve/qemu-server/107.conf $PROOS_DIR/pve/vmbkps/vzdump-qemu-107.conf
  chmod 777 $PROOS_DIR/pve/vmbkps/vzdump-qemu-107.conf
  ###
  ### Copy to ZFS Pool
  ###
  echo " "
  echo "Copying VM's to Datastore..."
  rsync --progress -a /var/lib/vz/dump/vzdump-* $TANK_DATA/ProOS/pve/vmbkps/
  chmod -R 777 $TANK_DATA/ProOS/pve/vmbkps/*.vma.zst
  chmod -R 777 $TANK_DATA/ProOS/pve/vmbkps/*.tar.zst
  chmod -R 777 $TANK_DATA/ProOS/pve/vmbkps/*.conf
  chmod -R 777 $TANK_DATA/ProOS/pve/vmbkps/*.log
  echo "Backup Complete."
  rm -f $RAMDISK/pvebackup.lock
  END_ROUTINE
fi

### end ###

echo "unknown command!"
END_ROUTINE
