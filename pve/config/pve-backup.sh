#!/bin/bash
###########################################################
###########################################################
### runs as root ###
## Backs up every Proxmox guest with vzdump and copies each guest's backup
## and live config into its own folder of the ProOS repo.
## Usage: pve-backup.sh pve_vmsbkp <RAM disk path>
## e.g.   pve-backup.sh pve_vmsbkp /mnt/ramdisk
## Run by tank-backup.sh for the backup wizard's VM backup, whose filter reads these
## lines for each guest (keep their wording):
##   "Backing-up <label> <id> (<n> of <count>)..."
##   "Backed-up <label> <id> (<n> of <count>)."
##   "FAILED: <label> <id> (<n> of <count>): <reason>"
## and vzdump's own "INFO:  45% (...)" progress lines. Exits 1 when any guest failed.

cmd="${1}"
ramDisk="${2}"

tankRoot="/mnt/datastore"
tankData="$tankRoot/data"
proosDir="$tankData/Projects/ProOS"

## Every guest, one per line: type|id|label|repo folder|config name|vzdump options.
## The router and UniFi keep their config as vzdump-qemu-<id>.conf, not qemu.conf,
## so login.sh does not take their folders for servers.
guestList=(
  "lxc|101|Files LXC|files|lxc.conf|--exclude-path /mnt --exclude-path /home/ben"
  "lxc|102|Management LXC|mgmt|lxc.conf|--exclude-path /mnt"
  "lxc|104|CIFS LXC|cifs|lxc.conf|--exclude-path /mnt"
  "lxc|106|Automate LXC|automate|lxc.conf|--exclude-path /mnt --exclude-path /var/www/html"
  "lxc|108|Media LXC|media|lxc.conf|--exclude-path /mnt"
  "qemu|100|Router KVM|router|vzdump-qemu-100.conf|--exclude-path /mnt"
  "qemu|103|AppleTalk KVM|appletalk|qemu.conf|--exclude-path /mnt"
  "qemu|105|Xana KVM|xana|qemu.conf|--exclude-path /mnt"
  "qemu|107|Unifi KVM|unifi|vzdump-qemu-107.conf|"
)

## Prints the date and host under the output, then exits with the code given (0 by default).
## Usage: END_ROUTINE [exit code]
function END_ROUTINE() {
  local exitCode="${1:-0}"
  local trailerText
  trailerText=$(date)
  trailerText+=" ("
  trailerText+=$(hostname)
  trailerText+=")"
  echo "$trailerText"
  echo " "
  exit "$exitCode"
}

## Ends the command unless argument 2 named a RAM disk folder.
function REQUIRE_RAMDISK() {
  if [ -z "$ramDisk" ] || [ ! -d "$ramDisk" ]; then
    echo "invalid or missing RAM disk path (argument 2)!"
    echo "aborting before operating on '/'."
    END_ROUTINE 1
  fi
}

## Backs up one guest with vzdump, then copies its live config and every dump of it in
## /var/lib/vz/dump into its own folder of this repo. Answers 1, after its FAILED line,
## when vzdump fails or leaves no dump.
## Usage: BACKUP_GUEST <lxc|qemu> <id> <label> <folder> <config name> <n> <count> [vzdump options...]
## e.g.   BACKUP_GUEST lxc 101 "Files LXC" files lxc.conf 1 9 --exclude-path /mnt
##        writes files/lxc.conf and files/vzdump-lxc-101-<date>.{tar.zst,log}
function BACKUP_GUEST() {
  local guestType="$1"
  local guestId="$2"
  local guestLabel="$3"
  local guestDir="$proosDir/$4"
  local configName="$5"
  local guestIndex="$6"
  local guestCount="$7"
  shift 7
  local guestTitle="$guestLabel $guestId ($guestIndex of $guestCount)"
  local configSource="/etc/pve/qemu-server/$guestId.conf"
  local vzdumpCode
  if [ "$guestType" == "lxc" ]; then
    configSource="/etc/pve/lxc/$guestId.conf"
  fi
  echo " "
  echo "Backing-up $guestTitle..."
  vzdump "$guestId" --mode snapshot --compress zstd --node pve --storage local --remove 1 "$@"
  vzdumpCode=$?
  if [ $vzdumpCode -ne 0 ]; then
    echo "FAILED: $guestTitle: vzdump exit $vzdumpCode"
    return 1
  fi
  ## Folder for a guest with no host folder of its own (router, unifi)
  if [ ! -d "$guestDir" ]; then
    mkdir -p "$guestDir"
    chown tank:shared "$guestDir"
  fi
  cp "$configSource" "$guestDir/$configName"
  chown tank:shared "$guestDir/$configName"
  ## The dash after the id keeps guest 100 from matching guest 1000
  local dumpFiles=( /var/lib/vz/dump/vzdump-"$guestType"-"$guestId"-* )
  if [ ! -e "${dumpFiles[0]}" ]; then
    echo "FAILED: $guestTitle: no backup found in /var/lib/vz/dump"
    return 1
  fi
  echo "Copying $guestLabel $guestId backup to $guestDir..."
  if ! rsync -a "${dumpFiles[@]}" "$guestDir/"; then
    echo "FAILED: $guestTitle: copying the backup to $guestDir failed"
    return 1
  fi
  chown tank:shared "$guestDir"/vzdump-"$guestType"-"$guestId"-*
  echo "Backed-up $guestTitle."
}

###### Server VM Backup Script ######################
if [ "$cmd" == "pve_vmsbkp" ]; then
  REQUIRE_RAMDISK
  ### Lock file -- atomic create via noclobber
  if ! ( set -o noclobber; : > "$ramDisk/pvebackup.lock" ) 2>/dev/null; then
    echo "PVE backup already running!"
    echo "delete '$ramDisk/pvebackup.lock' to remove lock if program aborted."
    END_ROUTINE 1
  fi
  trap 'rm -f "$ramDisk/pvebackup.lock"' EXIT
  trap 'exit 143' TERM INT
  guestCount=${#guestList[@]}
  guestIndex=0
  failedGuests=0
  for guestEntry in "${guestList[@]}"; do
    guestIndex=$(( guestIndex + 1 ))
    IFS='|' read -r guestType guestId guestLabel guestFolder configName vzdumpOptions <<< "$guestEntry"
    ## vzdumpOptions split into words on purpose: "--exclude-path /mnt" is two
    if ! BACKUP_GUEST "$guestType" "$guestId" "$guestLabel" "$guestFolder" "$configName" \
      "$guestIndex" "$guestCount" $vzdumpOptions; then
      failedGuests=$(( failedGuests + 1 ))
    fi
  done
  echo " "
  rm -f "$ramDisk/pvebackup.lock"
  if [ $failedGuests -gt 0 ]; then
    echo "Backup finished, $failedGuests of $guestCount guest(s) failed."
    END_ROUTINE 1
  fi
  echo "Backup Complete."
  END_ROUTINE 0
fi

### end ###

echo "unknown command!"
END_ROUTINE 1
