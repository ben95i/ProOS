#!/bin/bash
########################################################
## Swap Space Script by Ben Provenzano III  ############
########################################################
## Usage: /opt/rpi/swapspace.sh /path/to/swapfile
##
## Enables a swap file at the given path:
##  - Refuses paths on the root filesystem (/) or in RAM (tmpfs/ramfs).
##  - Creates the file if it does not exist (needs SWAP_SIZE_MB free).
##  - Reuses an existing file if it is the right size, otherwise
##    recreates it. A damaged swap signature is rebuilt once.
##  - Does nothing if the file is already in use as swap.
## Exit status: 0 = swap enabled, 1 = swap not enabled.

SWAP_SIZE_MB=2048
## Lower = less swapping (fewer writes to flash storage). Linux default is 60.
SWAP_SWAPPINESS=10

if [ -z "$1" ]; then
  echo "Usage: $0 /path/to/swapfile"
  exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
  echo "swapspace: must be run as root."
  exit 1
fi

## Resolve the full path (follows symlinks and '..') so the checks below
## see where the file really lives
SWAP_FILE=$(realpath -m "$1")
SWAP_DIR=$(dirname "$SWAP_FILE")
if [ ! -d "$SWAP_DIR" ]; then
  echo "swapspace: folder $SWAP_DIR does not exist, swap not enabled."
  exit 1
fi

## Which filesystem holds the folder. If several mounts are stacked on
## the same folder, the last one listed is the one in effect.
read -r MOUNT_POINT FSTYPE < <(findmnt -n -o TARGET,FSTYPE --target "$SWAP_DIR" | tail -n 1)
if [ "$MOUNT_POINT" = "/" ]; then
  echo "swapspace: $SWAP_FILE is on the root filesystem (/), swap not enabled."
  echo "swapspace: is the drive mounted?"
  exit 1
fi
case "$FSTYPE" in
  tmpfs|ramfs)
    echo "swapspace: $SWAP_FILE is in RAM ($FSTYPE), swap not enabled."
    exit 1
    ;;
esac

## Already active (script run again)
if swapon --show=NAME --noheadings | grep -qxF "$SWAP_FILE"; then
  echo "swapspace: swap already active on $SWAP_FILE"
  exit 0
fi

## Existing file of the wrong size (e.g. SWAP_SIZE_MB changed) - recreate it
if [ -f "$SWAP_FILE" ]; then
  SIZE=$(stat -c %s "$SWAP_FILE")
  if [ "$SIZE" -ne $((SWAP_SIZE_MB * 1024 * 1024)) ]; then
    echo "swapspace: swap file is the wrong size, recreating it"
    rm -f "$SWAP_FILE"
  fi
fi

## Create the swap file (one time only)
if [ ! -f "$SWAP_FILE" ]; then
  AVAIL=$(df --output=avail -m "$SWAP_DIR" | tail -n 1 | tr -d ' ')
  if [ "$AVAIL" -lt "$SWAP_SIZE_MB" ]; then
    echo "swapspace: not enough space on $MOUNT_POINT (${AVAIL}MB free), swap not enabled."
    exit 1
  fi
  echo "swapspace: creating ${SWAP_SIZE_MB}MB swap file $SWAP_FILE (one time, takes a few minutes)..."
  ## Written out in full with dd: swap files must not be sparse
  if ! dd if=/dev/zero of="$SWAP_FILE" bs=1M count="$SWAP_SIZE_MB" status=none; then
    echo "swapspace: could not create swap file, swap not enabled."
    rm -f "$SWAP_FILE"
    exit 1
  fi
  chmod 600 "$SWAP_FILE" 2>/dev/null
  mkswap "$SWAP_FILE" > /dev/null
fi
chmod 600 "$SWAP_FILE" 2>/dev/null

## Enable it. If the swap signature is missing or damaged, rebuild it once.
if ! swapon "$SWAP_FILE" 2>/dev/null; then
  if ! { mkswap "$SWAP_FILE" > /dev/null && swapon "$SWAP_FILE"; }; then
    echo "swapspace: swapon failed - the $FSTYPE filesystem may not support swap files, swap not enabled."
    exit 1
  fi
fi
sysctl -q vm.swappiness="$SWAP_SWAPPINESS"
echo "swapspace: swap enabled on $SWAP_FILE (${SWAP_SIZE_MB}MB, swappiness $SWAP_SWAPPINESS)"
exit 0
