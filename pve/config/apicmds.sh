#!/bin/bash
###########################################################
###########################################################
### runs as root ###

CMD="${1}"
RAMDISK="${2}"

TANK_ROOT="/mnt/datastore"
REGDATA="$TANK_ROOT/.regions"
BINDMNTS="/mnt/bindmnts"
TANK_SNAPS="$TANK_ROOT/.zfs/snapshot"

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
  local _FUSERC=$?
  shred -n 2 -z -u "$FUSEPWD" 2>/dev/null || rm -f "$FUSEPWD"
  if [ $_FUSERC -ne 0 ]; then
    echo "failed to attach $UNCAPSTR (gocryptfs exit $_FUSERC)."
    rmdir "$FUSEPTH" 2>/dev/null
    return $_FUSERC
  fi
  echo "attached $UNCAPSTR."
}

function MOUNT_SNAP(){
  if ! PATHMOUNTED "$BINDMNTS/Snapshots"; then
    echo "attaching ZFS snapshots region..."
    mkdir -p "$BINDMNTS/Snapshots"
    mount --rbind "$TANK_SNAPS" "$BINDMNTS/Snapshots"
  else
    echo "snapshots already attached."
  fi
}
function UMOUNT_SNAP(){
  if PATHMOUNTED "$BINDMNTS/Snapshots"; then
    echo "detaching ZFS snapshots region..."
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

## Hands the command over to one of the host's own scripts, which prints its
## own trailer. exec keeps this process, so file-monitor still sees one job.
## Usage: RUN_SCRIPT <script path> [arguments...]
## e.g.   RUN_SCRIPT /usr/bin/tank-backup.sh "$CMD" "$RAMDISK"
function RUN_SCRIPT() {
  local scriptPath="$1"
  shift
  if [ ! -x "$scriptPath" ]; then
    echo "'$scriptPath' not installed!"
    END_ROUTINE
  fi
  exec "$scriptPath" "$@"
}

if [ "$CMD" == "clearlog" ]; then
  REQUIRE_RAMDISK
  truncate -s 0 "$RAMDISK/sysout.txt"
  timeout 30 bash -c "printf '%s\n' \"\$(fastfetch --pipe -s Title:Separator:OS:Host:Kernel:Uptime:CPU:Memory:Swap:Disk | sed -E 's/\x1b\[[0-9;]*[a-zA-Z]//g' | tr -cd '\11\12\15\40-\176' | sed -E 's/[[:space:]]+$//')\""
  END_ROUTINE
fi

## Backup wizard jobs (tank-backup.sh): every bkp_* command. file-monitor sends
## their output to bkplog.txt, never to sysout.txt.
case "$CMD" in
  bkp_*)
    RUN_SCRIPT /usr/bin/tank-backup.sh "$CMD" "$RAMDISK"
    ;;
esac

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

## List ZFS Snapshots
if [ "$CMD" == "pve_listsnaps" ]; then
  echo "Snapshots on ZFS pool (tank/datastore):"
  zfs list -t snapshot tank/datastore | grep -o '^\S*'
  END_ROUTINE
fi

### START LXC/VMs #####################################
if [ "$CMD" == "startcifs" ]; then
  echo "starting legacy CIFS server..."
  lxc-start -n 104
  END_ROUTINE
fi
if [ "$CMD" == "stopcifs" ]; then
  echo "shutting down legacy CIFS server..."
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
  echo "starting legacy AppleTalk server..."
  qm start 103
  END_ROUTINE
fi
if [ "$CMD" == "stopatalk" ]; then
  echo "shutting down legacy AppleTalk server..."
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
### end ###

echo "unknown command!"
END_ROUTINE