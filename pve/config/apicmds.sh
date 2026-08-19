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

function PATHMOUNTED() { 
  findmnt --mountpoint "$1" >/dev/null 2>&1
}

## FIX 6: guard every code path that builds a path from $RAMDISK.
## An empty $2 would otherwise turn "$RAMDISK/pwd.txt" into "/pwd.txt"
## and hand root-owned shred/truncate/mount operations the filesystem root.
function REQUIRE_RAMDISK() {
  if [ -z "$RAMDISK" ] || [ ! -d "$RAMDISK" ]; then
    echo "invalid or missing RAM disk path (argument 2)!"
    echo "aborting before operating on '/'."
    END_ROUTINE
  fi
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
  if [ ! -e "$FUSEPWD" ]; then
    echo "password file not found!"
    return 1
  fi
  mkdir -p "$FUSEPTH"
  gocryptfs -quiet -allow_other \
    "$REGDATA/$VOLNME" \
    "$FUSEPTH" -passfile "$FUSEPWD"
  ## FIX 7: capture the exit status before anything else clobbers $?
  local _FUSERC=$?
  ## destroy key material on every path (shred, to match pwd.txt handling)
  shred -n 2 -z -u "$FUSEPWD" 2>/dev/null || rm -f "$FUSEPWD"
  if [ $_FUSERC -ne 0 ]; then
    echo "failed to attach $UNCAPSTR (gocryptfs exit $_FUSERC)."
    ## don't leave a bare empty directory posing as a mounted region
    rmdir "$FUSEPTH" 2>/dev/null
    return $_FUSERC
  fi
  echo "attached $UNCAPSTR."
}

function MOUNT_SNAP(){
  if ! PATHMOUNTED "$BINDMNTS/Snapshots"; then
    echo "attaching ZFS snapshots region..."
    mkdir -p "$BINDMNTS/Snapshots"
    ## FIX 4: each snapshot under .zfs/snapshot is its own automounted
    ## filesystem; a plain --bind exposes only the empty parent directory.
    mount --rbind "$TANK_SNAPS" "$BINDMNTS/Snapshots"
  else
    echo "snapshots already attached."
  fi
}
function UMOUNT_SNAP(){
  if PATHMOUNTED "$BINDMNTS/Snapshots"; then
    echo "detaching ZFS snapshots region..."
    ## FIX 4: -R detaches the whole bound subtree, not just the top mount
    umount -R -l "$BINDMNTS/Snapshots"
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
  touch "$EXTBKPS/$_POOL/LastSynced.txt"
  local _CURDATE=$(date '+%F %r')
  CURBKPDTES+=( "$_POOL|${_CURDATE// /_}" )
}

if [ "$CMD" == "clearlog" ]; then
  REQUIRE_RAMDISK
  truncate -s 0 "$RAMDISK/sysout.txt"
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
  REQUIRE_RAMDISK
  ## Seconds to wait for drives to settle before triggering detach
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
  ## Turn on indicator LED
  curl -s -f 'http://ledwall.home/api?action=main&arg=whtledon' > /dev/null 2>&1 &
  ## Read Backup Drive Names
  ## FIX 2: without this guard a missing drives file syncs nothing,
  ## yet still prints "backup complete" and fires the detach trigger.
  if [ ! -e "$DRIVES_PATH" ]; then
    echo "drives file '$DRIVES_PATH' not found, aborting backup!"
    ## EXIT trap releases the lock; clear the indicator LED first
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
      ## FIX 3: unquoted, a glob character in the drives file makes this
      ## expand against the cwd and abort the test with "too many arguments"
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
  ################################
  ## Write backup dates to file ##
  ## FIX 1: on a fresh install (or if $EXTBKPS wasn't mounted) the file is
  ## absent, the merge block below was skipped, and every collected date was
  ## discarded silently -- leaving the file absent forever. Create it here.
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
  ################################
  ################################
  ## Wait for drives to settle after backup complete ##
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
  REQUIRE_RAMDISK
  MNT_FUSEFS "Archive"
  END_ROUTINE
fi
if [ "$CMD" == "unmnt_arch_region" ]; then
  UMNT_FUSEFS "Archive"
  END_ROUTINE
fi

## RAM Disk Region
if [ "$CMD" == "mnt_ram_region" ]; then
  REQUIRE_RAMDISK
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
  REQUIRE_RAMDISK
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
          ## FIX 5: attach mounts the dataset ($POOL/extbkp), so unmount the
          ## dataset too. Passing a mountpoint path only worked while the
          ## dataset's mountpoint property happened to match $EXTBKPS/$POOL;
          ## when it didn't, unmount failed and the export failed busy.
          if ! zfs unmount "$POOL/extbkp"; then
            echo "WARNING: could not unmount $POOL/extbkp -- skipping export."
            echo " "
            continue
          fi
          if ! zpool export "$POOL"; then
            echo "WARNING: export of $POOL failed (pool still in use?)."
          fi
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
  REQUIRE_RAMDISK
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
  rsync --progress -a /var/lib/vz/dump/vzdump-* $PROOS_DIR/pve/vmbkps/
  chmod -R 777 $PROOS_DIR/pve/vmbkps/*.vma.zst
  chmod -R 777 $PROOS_DIR/pve/vmbkps/*.tar.zst
  chmod -R 777 $PROOS_DIR/pve/vmbkps/*.conf
  chmod -R 777 $PROOS_DIR/pve/vmbkps/*.log
  echo "Backup Complete."
  rm -f "$RAMDISK/pvebackup.lock"
  END_ROUTINE
fi

### end ###

echo "unknown command!"
END_ROUTINE