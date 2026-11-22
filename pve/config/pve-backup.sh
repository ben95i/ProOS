#!/bin/bash
###########################################################
###########################################################
### runs as root ###
## Backs up every Proxmox guest with vzdump and copies each guest's backup
## and live config into its own folder of the ProOS repo.
## Usage: pve-backup.sh pve_vmsbkp <RAM disk path>
## e.g.   pve-backup.sh pve_vmsbkp /mnt/ramdisk
## Run by apicmds.sh for the website's pve_vmsbkp command.

CMD="${1}"
RAMDISK="${2}"

TANK_ROOT="/mnt/datastore"
TANK_DATA="$TANK_ROOT/data"
PROOS_DIR="$TANK_DATA/Projects/ProOS"

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

## Backs up one guest with vzdump, then copies its live config and every dump
## of it in /var/lib/vz/dump into its own folder of this repo.
## Usage: BACKUP_GUEST <lxc|qemu> <id> <label> <folder> <config name> [vzdump options...]
## e.g.   BACKUP_GUEST lxc 101 "Files LXC" files lxc.conf --exclude-path /mnt
##        writes files/lxc.conf and files/vzdump-lxc-101-<date>.{tar.zst,log}
function BACKUP_GUEST() {
  local guestType="$1"
  local guestId="$2"
  local guestLabel="$3"
  local guestDir="$PROOS_DIR/$4"
  local configName="$5"
  shift 5
  local configSource="/etc/pve/qemu-server/$guestId.conf"
  if [ "$guestType" == "lxc" ]; then
    configSource="/etc/pve/lxc/$guestId.conf"
  fi
  echo " "
  echo "Backing-up $guestLabel $guestId..."
  vzdump "$guestId" --mode snapshot --compress zstd --node pve --storage local --remove 1 "$@"
  ## Folder for a guest with no host folder of its own (router, unifi)
  if [ ! -d "$guestDir" ]; then
    mkdir -p "$guestDir"
    chown tank:shared "$guestDir"
  fi
  cp "$configSource" "$guestDir/$configName"
  chown -v tank:shared "$guestDir/$configName"
  ## The dash after the id keeps guest 100 from matching guest 1000
  local dumpFiles=( /var/lib/vz/dump/vzdump-"$guestType"-"$guestId"-* )
  if [ ! -e "${dumpFiles[0]}" ]; then
    echo "no backup of $guestLabel $guestId found in /var/lib/vz/dump!"
    return 1
  fi
  echo "Copying $guestLabel $guestId backup to $guestDir..."
  rsync --progress -a "${dumpFiles[@]}" "$guestDir/"
  chown -v tank:shared "$guestDir"/vzdump-"$guestType"-"$guestId"-*
}

###### Server VM Backup Script ######################
if [ "$CMD" == "pve_vmsbkp" ]; then
  REQUIRE_RAMDISK
  ### Lock file -- atomic create via noclobber (as tank-backup.sh does)
  if ! ( set -o noclobber; : > "$RAMDISK/pvebackup.lock" ) 2>/dev/null; then
    echo "PVE backup already running!"
    echo "delete '$RAMDISK/pvebackup.lock' to remove lock if program aborted."
    END_ROUTINE
  fi
  trap 'rm -f "$RAMDISK/pvebackup.lock"' EXIT
  trap 'exit 143' TERM INT
  ## Each guest's backup and config go to its own folder in the repo.
  ## The router and UniFi keep their config as vzdump-qemu-<id>.conf, not
  ## qemu.conf, so login.sh does not take their folders for servers.
  BACKUP_GUEST lxc 101 "Files LXC" files lxc.conf --exclude-path /mnt --exclude-path /home/ben
  BACKUP_GUEST lxc 102 "Management LXC" mgmt lxc.conf --exclude-path /mnt
  BACKUP_GUEST lxc 104 "CIFS LXC" cifs lxc.conf --exclude-path /mnt
  BACKUP_GUEST lxc 106 "Automate LXC" automate lxc.conf --exclude-path /mnt --exclude-path /var/www/html
  BACKUP_GUEST lxc 108 "Media LXC" media lxc.conf --exclude-path /mnt
  BACKUP_GUEST qemu 100 "Router KVM" router vzdump-qemu-100.conf --exclude-path /mnt
  BACKUP_GUEST qemu 103 "AppleTalk KVM" appletalk qemu.conf --exclude-path /mnt
  BACKUP_GUEST qemu 105 "Xana KVM" xana qemu.conf --exclude-path /mnt
  BACKUP_GUEST qemu 107 "Unifi KVM" unifi vzdump-qemu-107.conf
  echo " "
  echo "Backup Complete."
  rm -f "$RAMDISK/pvebackup.lock"
  END_ROUTINE
fi

### end ###

echo "unknown command!"
END_ROUTINE