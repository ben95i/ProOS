#!/bin/bash
#
# fallbackpart.sh - Camera Fallback Partition Setup by Ben Provenzano III
#
# Creates a 2 GiB ext4 partition (label camfallback) at the very end of the
# SD card the Pi boots from, for minimotion's fallback_dir, then leaves
# /etc/fallbackpart.enabled so it never runs again (and so "rpi expand" knows
# the root partition can no longer grow). Run only by the doorway module's
# installer, as root, in read/write mode:
#   bash /opt/rpi/fallbackpart.sh
#
# The 2 GiB must already be free at the end of the card. Pi OS grows the root
# partition over the whole card on its first boot, so on a card in use the
# root partition has to be shrunk first, on another machine. Without enough
# free space nothing is changed, and the next deploy checks again.
#
# The doorway's boot.sh repairs and mounts the partition at /mnt/fallback on
# every boot and creates minimotion's folder in it.

## Settings
readonly MARKER_FILE="/etc/fallbackpart.enabled"
readonly PARTITION_LABEL="camfallback"
readonly PARTITION_SIZE_MIB=2048
## The partition starts on a 4 MiB boundary, as the Pi OS partitions do
readonly ALIGN_MIB=4
readonly SECTOR_BYTES=512
readonly BYTES_PER_MIB=1048576
## A GPT disk keeps its backup table in its last 33 sectors
readonly GPT_BACKUP_SECTORS=33
## An MBR disk holds at most 4 primary partitions
readonly MBR_MAX_PARTITIONS=4
readonly REQUIRED_COMMANDS=(findmnt lsblk blkid blockdev sfdisk partx udevadm mkfs.ext4)

## Prints why the setup stopped and exits; the card is left as it was
##   reason  e.g. "the root filesystem is read-only"
ABORT() {
  local reason="$1"
  echo "Fallback partition not created: $reason"
  exit 1
}

## Stops unless every tool the setup uses is installed
CHECK_COMMANDS() {
  local commandName
  for commandName in "${REQUIRED_COMMANDS[@]}"; do
    if ! command -v "$commandName" > /dev/null; then
      ABORT "the command $commandName is not installed."
    fi
  done
}

## Prints the device of partition number N on a disk
##   PARTITION_DEVICE mmcblk0 3  ->  /dev/mmcblk0p3
##   PARTITION_DEVICE sda 3      ->  /dev/sda3
PARTITION_DEVICE() {
  local diskName="$1"
  local partitionNumber="$2"
  if [[ "$diskName" =~ [0-9]$ ]]; then
    echo "/dev/${diskName}p${partitionNumber}"
  else
    echo "/dev/${diskName}${partitionNumber}"
  fi
}

## Already set up
if [ -e "$MARKER_FILE" ]; then
  echo "Fallback partition already set up ($MARKER_FILE exists)."
  exit 0
fi

## Preconditions
if [ "$(id -u)" -ne 0 ]; then
  ABORT "it must be run as root."
fi
rootFilesystemType=$(findmnt -no FSTYPE /)
if [ "$rootFilesystemType" = "overlay" ]; then
  ABORT "the root filesystem is read-only (overlay), so $MARKER_FILE would not be kept. Run it in read/write mode (rpi rw)."
fi
CHECK_COMMANDS

## A partition left by an earlier setup (the marker was lost): keep it
existingDevice=$(blkid -L "$PARTITION_LABEL")
if [ -n "$existingDevice" ]; then
  echo "Fallback partition $existingDevice already exists, keeping it."
  echo "$existingDevice" > "$MARKER_FILE"
  chown root:root "$MARKER_FILE"
  chmod 644 "$MARKER_FILE"
  exit 0
fi

## The SD card the Pi boots from (the disk holding /boot/firmware)
bootPartition=$(findmnt -no SOURCE /boot/firmware)
if [ -z "$bootPartition" ]; then
  ABORT "the boot partition is not mounted at /boot/firmware."
fi
diskName=$(lsblk -no PKNAME "$bootPartition")
if [ -z "$diskName" ]; then
  ABORT "the disk holding $bootPartition was not found."
fi
diskDevice="/dev/$diskName"
logicalSectorBytes=$(blockdev --getss "$diskDevice")
if [ "$logicalSectorBytes" -ne "$SECTOR_BYTES" ]; then
  ABORT "$diskDevice has $logicalSectorBytes-byte sectors, only $SECTOR_BYTES-byte sectors are supported."
fi

## The last sector a partition may use (exclusive), by partition table type
diskSectors=$(blockdev --getsz "$diskDevice")
tableType=$(blkid -o value -s PTTYPE "$diskDevice")
partitionCount=$(partx -g -o NR "$diskDevice" | wc -l)
if [ "$tableType" = "dos" ]; then
  if [ "$partitionCount" -ge "$MBR_MAX_PARTITIONS" ]; then
    ABORT "$diskDevice already has $partitionCount partitions, the most an MBR table holds."
  fi
  tableEndSector=$diskSectors
elif [ "$tableType" = "gpt" ]; then
  tableEndSector=$((diskSectors - GPT_BACKUP_SECTORS))
else
  ABORT "$diskDevice has an unknown partition table type ($tableType)."
fi

## The first free sector after the last partition (from the table on the card)
firstFreeSector=0
while read -r partitionStart partitionSectors; do
  partitionEndSector=$((partitionStart + partitionSectors))
  if [ "$partitionEndSector" -gt "$firstFreeSector" ]; then
    firstFreeSector=$partitionEndSector
  fi
done < <(partx -g -o START,SECTORS "$diskDevice")

## The new partition ends at the end of the card and starts on a boundary
newPartitionSectors=$((PARTITION_SIZE_MIB * BYTES_PER_MIB / SECTOR_BYTES))
alignSectors=$((ALIGN_MIB * BYTES_PER_MIB / SECTOR_BYTES))
latestStartSector=$((tableEndSector - newPartitionSectors))
alignedBlockCount=$((latestStartSector / alignSectors))
newPartitionStart=$((alignedBlockCount * alignSectors))

## Free space check
freeSectors=$((tableEndSector - firstFreeSector))
freeMib=$((freeSectors * SECTOR_BYTES / BYTES_PER_MIB))
if [ "$newPartitionStart" -lt "$firstFreeSector" ]; then
  ABORT "only $freeMib MiB is free at the end of $diskDevice, $PARTITION_SIZE_MIB MiB is needed. Shrink the root partition on another machine first."
fi
echo "Creating a $PARTITION_SIZE_MIB MiB fallback partition at the end of $diskDevice ($freeMib MiB free)..."

## Partition (the card is in use, so the kernel is told about the new
## partition alone, and the mounted partitions are not re-read)
partitionLine="start=$newPartitionStart, size=$newPartitionSectors, type=L"
if ! echo "$partitionLine" | sfdisk --append --no-reread --quiet "$diskDevice"; then
  ABORT "sfdisk could not add the partition to $diskDevice."
fi
newPartitionNumber=$(partx -g -o NR,START "$diskDevice" | awk -v startSector="$newPartitionStart" '$2 == startSector {print $1}')
if [ -z "$newPartitionNumber" ]; then
  ABORT "the new partition is missing from the table on $diskDevice."
fi
newPartitionDevice=$(PARTITION_DEVICE "$diskName" "$newPartitionNumber")
partx --add --nr "$newPartitionNumber" "$diskDevice"
udevadm settle
if [ ! -b "$newPartitionDevice" ]; then
  echo "Fallback partition: the kernel did not add $newPartitionDevice."
  echo "Delete it with: sfdisk --delete $diskDevice $newPartitionNumber, then deploy again."
  exit 1
fi

## Filesystem: no reserved blocks, and every table written now, so the card
## gets no background initialisation writes after the first mount
if ! mkfs.ext4 -F -q -L "$PARTITION_LABEL" -m 0 -E lazy_itable_init=0,lazy_journal_init=0 "$newPartitionDevice"; then
  echo "Fallback partition: formatting $newPartitionDevice failed."
  echo "Delete it with: sfdisk --delete $diskDevice $newPartitionNumber, then deploy again."
  exit 1
fi
udevadm settle

## Marker (holds the partition's device)
echo "$newPartitionDevice" > "$MARKER_FILE"
chown root:root "$MARKER_FILE"
chmod 644 "$MARKER_FILE"
echo "Fallback partition $newPartitionDevice created, mounted at boot by boot.sh."
exit 0
