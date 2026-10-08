#!/bin/bash
#
# camcleanup.sh - Cameras Cleanup Script for Pi
#
# Keeps the Google Drive sync folder and the USB flash drive camera folder
# below a configurable size limit (in GB) by deleting the OLDEST files first.
#
# Both folders share the same cleanup method (enforceFolderLimit):
#   - a cleanup starts when the folder uses more than (limit - reserve),
#     or when the disk holding it has less than "reserve" free space
#   - the oldest files are deleted until the folder is at least
#     FREE_SPACE_RESERVE_GB (default 2 GB) below its limit and the disk
#     has at least FREE_SPACE_RESERVE_GB free
#   - files modified in the last MIN_FILE_AGE_MINUTES are never deleted,
#     because the camera or rclone may still be writing them
#   - a location is only cleaned when its mount point is mounted and its
#     camera folder exists (no internet check is needed)
#
# Run as root user only! (started by rpi-camcleanup.timer / rpi-camcleanup.service)
#
# All settings are configured in rpi-camcleanup.service (Environment= lines).
# The defaults below are only used when the script is run by hand.

set -o nounset
set -o pipefail

## Force a predictable number format for find (%T@) and sort -n
export LC_ALL=C

###############################################################################
## Configuration
###############################################################################

## Mount points of the two storage locations
readonly FLASH_PATH="${FLASH_PATH:-/media/usb0}"
readonly GDRIVE_PATH="${GDRIVE_PATH:-/mnt/gsync}"

## Folders that are managed. Only files inside these folders are ever deleted.
## (Write them without a trailing slash.)
readonly FLASH_CAMERA_FOLDER="${FLASH_CAMERA_FOLDER:-$FLASH_PATH/cameras}"
readonly GDRIVE_CAMERA_FOLDER="${GDRIVE_CAMERA_FOLDER:-$GDRIVE_PATH/Current}"

## Maximum size of each managed folder in GB (decimals allowed, e.g. 14.5)
readonly FLASH_LIMIT_GB="${FLASH_LIMIT_GB:-28}"
readonly GDRIVE_LIMIT_GB="${GDRIVE_LIMIT_GB:-14}"

## Space (GB, decimals allowed) left free below the limit, and on the disk,
## after a cleanup
readonly FREE_SPACE_RESERVE_GB="${FREE_SPACE_RESERVE_GB:-2}"

## Files modified more recently than this are never deleted
readonly MIN_FILE_AGE_MINUTES="${MIN_FILE_AGE_MINUTES:-10}"

## rclone service that provides the Google Drive mount (restarted when it hangs)
readonly RCLONE_SERVICE_NAME="${RCLONE_SERVICE_NAME:-rpi-rclone}"

## Maximum seconds to wait for the mount check (a hung rclone mount blocks it)
readonly MOUNT_CHECK_TIMEOUT_SECONDS="${MOUNT_CHECK_TIMEOUT_SECONDS:-10}"

## Maximum run time for commands that scan the (possibly hung) mounts
readonly COMMAND_TIMEOUT_SECONDS="${COMMAND_TIMEOUT_SECONDS:-500}"

## Log files and their maximum sizes
readonly FLASH_LOG_FILE="${FLASH_LOG_FILE:-$FLASH_CAMERA_FOLDER/camcleanup.log}"
readonly FALLBACK_LOG_FILE="${FALLBACK_LOG_FILE:-/tmp/camcleanup.log}"
readonly FLASH_LOG_MAX_SIZE="${FLASH_LOG_MAX_SIZE:-5MB}"
readonly FALLBACK_LOG_MAX_SIZE="${FALLBACK_LOG_MAX_SIZE:-128KB}"

## Lock file that stops two cleanups from running at the same time
readonly LOCK_FILE="${LOCK_FILE:-/run/camcleanup.lock}"
readonly LOCK_FILE_DESCRIPTOR=9

## Fixed values (not configurable)
readonly BYTES_PER_GB=1000000000
## 1 GB = 10^9 bytes, so 9 decimal places of a GB value are exactly 1 byte
readonly GB_DECIMAL_PLACES=9
readonly SECONDS_PER_MINUTE=60
readonly TIMEOUT_EXIT_CODE=124
readonly RESULT_OK=0
readonly RESULT_ERROR=1
readonly RESULT_TIMEOUT=2

###############################################################################
## Global state
###############################################################################

## Log file chosen at start-up (flash drive log, or the /tmp fallback)
logFile=""

## Private temporary file holding the file list of the folder being cleaned
fileListPath=""

## Fields of the most recently parsed file list record (see parseFileRecord)
recordModificationEpochSeconds=0
recordSizeBytes=0
recordFilePath=""

## Results of the most recent deleteOldestFiles call
deletedFileCount=0
deletedBytes=0

###############################################################################
## Logging helpers
###############################################################################

## Prints a status message to stdout (captured by journald under systemd)
printStatus() {
  local statusMessage="$1"
  echo "$statusMessage"
}

## Appends a message followed by the current date to the cleanup log file
writeLogEntry() {
  local logMessage="$1"
  local currentDate
  currentDate="$(date)"
  echo "$logMessage $currentDate" >> "$logFile"
}

## Shrinks a log file to at most the given size, keeping its start
trimLogFile() {
  local logFilePath="$1"
  local maximumSize="$2"
  truncate -s "<$maximumSize" -- "$logFilePath"
}

###############################################################################
## Number helpers
###############################################################################

## Succeeds when the value is a whole, non-negative number
isWholeNumber() {
  local candidateValue="$1"
  [[ "$candidateValue" =~ ^[0-9]+$ ]]
}

## Succeeds when the value is a GB amount: digits with an optional decimal
## part of up to 9 places (e.g. "14", "14.5", "0.25")
isGbAmount() {
  local candidateValue="$1"
  [[ "$candidateValue" =~ ^[0-9]+(\.[0-9]{1,9})?$ ]]
}

## Prints the number of bytes in the given number of GB (e.g. "14.5").
## Bash only does whole-number maths, so the whole part and the decimal part
## are converted separately: because 1 GB = 10^9 bytes, the decimal part
## padded to 9 digits is already the number of extra bytes
## ("14.5" -> 14 * 10^9 + 500000000).
convertGbToBytes() {
  local gigabytes="$1"

  local wholeGbText="${gigabytes%%.*}"
  local decimalText=""
  if [[ "$gigabytes" == *.* ]]; then
    decimalText="${gigabytes#*.}"
  fi

  ## Pad the decimal part with zeros on the right to exactly 9 digits
  local zeroPadding="000000000"
  local paddedDecimalText="${decimalText}${zeroPadding}"
  local decimalBytesText="${paddedDecimalText:0:GB_DECIMAL_PLACES}"

  ## 10# forces base 10, so values like "08" are not read as octal
  local wholeGbValue=$(( 10#$wholeGbText ))
  local decimalBytes=$(( 10#$decimalBytesText ))
  local wholeBytes=$(( wholeGbValue * BYTES_PER_GB ))
  local totalBytes=$(( wholeBytes + decimalBytes ))
  echo "$totalBytes"
}

## Prints a byte count as GB with two decimals, for readable log messages
formatBytesAsGb() {
  local bytes="$1"
  awk -v byteCount="$bytes" -v bytesPerGb="$BYTES_PER_GB" \
    'BEGIN { printf "%.2f", byteCount / bytesPerGb }'
}

## Prints the larger of two whole numbers
largerOf() {
  local firstValue="$1"
  local secondValue="$2"
  if (( firstValue > secondValue )); then
    echo "$firstValue"
  else
    echo "$secondValue"
  fi
}

###############################################################################
## Start-up checks
###############################################################################

## Stops the script unless it runs as root (it deletes files owned by others)
requireRootUser() {
  local currentUserId
  currentUserId="$(id -u)"
  if [[ "$currentUserId" -ne 0 ]]; then
    echo "camcleanup.sh must be run as root" >&2
    return "$RESULT_ERROR"
  fi
  return "$RESULT_OK"
}

## Checks that one folder limit is a GB amount larger than the reserve
validateFolderLimit() {
  local limitName="$1"
  local limitGb="$2"
  if ! isGbAmount "$limitGb"; then
    echo "$limitName must be a number of GB like 14 or 14.5, got '$limitGb'" >&2
    return "$RESULT_ERROR"
  fi
  local limitBytes
  limitBytes="$(convertGbToBytes "$limitGb")"
  local reserveBytes
  reserveBytes="$(convertGbToBytes "$FREE_SPACE_RESERVE_GB")"
  if (( limitBytes <= reserveBytes )); then
    echo "$limitName ($limitGb GB) must be larger than FREE_SPACE_RESERVE_GB ($FREE_SPACE_RESERVE_GB GB)" >&2
    return "$RESULT_ERROR"
  fi
  return "$RESULT_OK"
}

## Checks all numeric settings before anything is deleted
validateConfiguration() {
  if ! isGbAmount "$FREE_SPACE_RESERVE_GB"; then
    echo "FREE_SPACE_RESERVE_GB must be a number of GB like 2 or 1.5, got '$FREE_SPACE_RESERVE_GB'" >&2
    return "$RESULT_ERROR"
  fi
  if ! isWholeNumber "$MIN_FILE_AGE_MINUTES"; then
    echo "MIN_FILE_AGE_MINUTES must be a whole number, got '$MIN_FILE_AGE_MINUTES'" >&2
    return "$RESULT_ERROR"
  fi
  if ! isWholeNumber "$MOUNT_CHECK_TIMEOUT_SECONDS"; then
    echo "MOUNT_CHECK_TIMEOUT_SECONDS must be a whole number, got '$MOUNT_CHECK_TIMEOUT_SECONDS'" >&2
    return "$RESULT_ERROR"
  fi
  if ! isWholeNumber "$COMMAND_TIMEOUT_SECONDS"; then
    echo "COMMAND_TIMEOUT_SECONDS must be a whole number, got '$COMMAND_TIMEOUT_SECONDS'" >&2
    return "$RESULT_ERROR"
  fi
  validateFolderLimit "FLASH_LIMIT_GB" "$FLASH_LIMIT_GB" || return "$RESULT_ERROR"
  validateFolderLimit "GDRIVE_LIMIT_GB" "$GDRIVE_LIMIT_GB" || return "$RESULT_ERROR"
  return "$RESULT_OK"
}

## Takes an exclusive lock so a manual run and a timer run never overlap.
## The lock is released automatically when the script exits.
acquireSingleInstanceLock() {
  eval "exec $LOCK_FILE_DESCRIPTOR>\"\$LOCK_FILE\"" || return "$RESULT_ERROR"
  if ! flock --nonblock "$LOCK_FILE_DESCRIPTOR"; then
    return "$RESULT_ERROR"
  fi
  return "$RESULT_OK"
}

## Creates a private temporary file (unique name, mode 600) for file lists
createFileListFile() {
  fileListPath="$(mktemp /tmp/camcleanup.XXXXXX)" || return "$RESULT_ERROR"
  return "$RESULT_OK"
}

## Removes the temporary file list; runs automatically on exit (trap)
# shellcheck disable=SC2329
removeTemporaryFiles() {
  if [[ -n "$fileListPath" ]]; then
    rm -f -- "$fileListPath"
  fi
}

###############################################################################
## Storage availability
###############################################################################

## Checks that a managed folder is usable: its mount point is mounted and the
## camera folder exists. Both checks run under a timeout, because looking at a
## hung rclone (FUSE) mount blocks forever.
## Returns RESULT_OK, RESULT_ERROR (not mounted / folder missing) or
## RESULT_TIMEOUT (the mount did not answer in time).
isStorageAvailable() {
  local mountPath="$1"
  local folderPath="$2"

  timeout "$MOUNT_CHECK_TIMEOUT_SECONDS" mountpoint -q -- "$mountPath"
  local mountCheckStatus=$?
  if [[ "$mountCheckStatus" -eq "$TIMEOUT_EXIT_CODE" ]]; then
    return "$RESULT_TIMEOUT"
  fi
  if [[ "$mountCheckStatus" -ne 0 ]]; then
    return "$RESULT_ERROR"
  fi

  ## External "test" (not the shell builtin) so that timeout can stop it
  timeout "$MOUNT_CHECK_TIMEOUT_SECONDS" test -d "$folderPath"
  local folderCheckStatus=$?
  if [[ "$folderCheckStatus" -eq "$TIMEOUT_EXIT_CODE" ]]; then
    return "$RESULT_TIMEOUT"
  fi
  if [[ "$folderCheckStatus" -ne 0 ]]; then
    return "$RESULT_ERROR"
  fi
  return "$RESULT_OK"
}

## Checks the USB flash drive camera folder (same return values as above)
isFlashDriveConnected() {
  isStorageAvailable "$FLASH_PATH" "$FLASH_CAMERA_FOLDER"
}

## Checks the Google Drive camera folder (same return values as above)
isGoogleDriveMounted() {
  isStorageAvailable "$GDRIVE_PATH" "$GDRIVE_CAMERA_FOLDER"
}

## Restarts the rclone service that provides the Google Drive mount
restartRcloneService() {
  systemctl restart "$RCLONE_SERVICE_NAME"
}

## Logs to the flash drive when it is connected, otherwise to /tmp
selectLogFile() {
  if isFlashDriveConnected; then
    logFile="$FLASH_LOG_FILE"
  else
    logFile="$FALLBACK_LOG_FILE"
  fi
}

###############################################################################
## Folder measurement
###############################################################################

## Prints the free bytes of the disk holding the folder
readDiskFreeBytes() {
  local folderPath="$1"
  local dfOutput
  dfOutput="$(timeout "$COMMAND_TIMEOUT_SECONDS" df -P -B1 -- "$folderPath")"
  local dfStatus=$?
  if [[ "$dfStatus" -ne 0 ]]; then
    return "$dfStatus"
  fi
  local freeBytes
  freeBytes="$(echo "$dfOutput" | awk 'NR==2 {print $4}')"
  if ! isWholeNumber "$freeBytes"; then
    return "$RESULT_ERROR"
  fi
  echo "$freeBytes"
  return "$RESULT_OK"
}

## Lists every file in the folder ONCE into the temporary file list.
## Each record is "<modification time> <size in bytes> <path>" ended by a NUL
## byte, so any file name (spaces, newlines) is handled safely.
## Returns the find exit status (124 = timed out).
scanFolderFiles() {
  local folderPath="$1"
  timeout "$COMMAND_TIMEOUT_SECONDS" \
    find "$folderPath" -type f -printf '%T@ %s %p\0' > "$fileListPath" 2> /dev/null
  local findStatus=$?
  return "$findStatus"
}

## Splits one file list record into recordModificationEpochSeconds,
## recordSizeBytes and recordFilePath
parseFileRecord() {
  local fileRecord="$1"
  local modificationTime="${fileRecord%% *}"
  local sizeAndPath="${fileRecord#* }"
  recordModificationEpochSeconds="${modificationTime%%.*}"
  recordSizeBytes="${sizeAndPath%% *}"
  recordFilePath="${sizeAndPath#* }"
}

## Prints the total size in bytes of every file in the file list
sumListedFileSizes() {
  local totalBytes=0
  local fileRecord
  while IFS= read -r -d '' fileRecord; do
    parseFileRecord "$fileRecord"
    totalBytes=$(( totalBytes + recordSizeBytes ))
  done < "$fileListPath"
  echo "$totalBytes"
}

###############################################################################
## Deletion
###############################################################################

## Removes the folders that held a deleted file once they are empty, walking
## up towards (but never removing) the managed folder. rmdir only removes
## empty folders, so a folder the camera has just written into is kept.
removeEmptyParentFolders() {
  local deletedFilePath="$1"
  local managedFolderPath="$2"
  local parentFolderPath
  parentFolderPath="$(dirname -- "$deletedFilePath")"
  while [[ "$parentFolderPath" != "$managedFolderPath" && "$parentFolderPath" == "$managedFolderPath"/* ]]; do
    if ! rmdir -- "$parentFolderPath" 2> /dev/null; then
      break
    fi
    parentFolderPath="$(dirname -- "$parentFolderPath")"
  done
}

## Deletes files from the file list, oldest first, until at least
## bytesToFree bytes are gone. Never deletes the protected file or files newer
## than MIN_FILE_AGE_MINUTES. Sets deletedFileCount and deletedBytes.
##   bytesToFree        number of bytes that must be freed
##   protectedFilePath  file that must never be deleted (the log file)
##   managedFolderPath  folder being cleaned (never removed itself)
deleteOldestFiles() {
  local bytesToFree="$1"
  local protectedFilePath="$2"
  local managedFolderPath="$3"

  local currentEpochSeconds
  currentEpochSeconds="$(date +%s)"
  local minimumAgeSeconds=$(( MIN_FILE_AGE_MINUTES * SECONDS_PER_MINUTE ))
  local newestDeletableEpochSeconds=$(( currentEpochSeconds - minimumAgeSeconds ))

  deletedFileCount=0
  deletedBytes=0

  local fileRecord
  local currentModificationEpochSeconds
  local removeStatus
  while IFS= read -r -d '' fileRecord; do
    ## Stop as soon as enough space has been freed
    if (( deletedBytes >= bytesToFree )); then
      break
    fi

    parseFileRecord "$fileRecord"

    ## Never delete the cleanup log itself
    if [[ "$recordFilePath" == "$protectedFilePath" ]]; then
      continue
    fi

    ## The list is sorted oldest first, so every remaining file is too new
    if (( recordModificationEpochSeconds > newestDeletableEpochSeconds )); then
      break
    fi

    ## Re-check right before deleting: the file may have been rewritten
    ## (or removed) by the camera / rclone since the folder was scanned
    currentModificationEpochSeconds="$(stat -c %Y -- "$recordFilePath" 2> /dev/null)"
    if [[ -n "$currentModificationEpochSeconds" ]]; then
      if (( currentModificationEpochSeconds > newestDeletableEpochSeconds )); then
        continue
      fi
    fi

    ## rm -f also succeeds when the file already vanished; it no longer uses
    ## space either way, so it counts as freed
    rm -f -- "$recordFilePath"
    removeStatus=$?
    if [[ "$removeStatus" -eq 0 ]]; then
      deletedFileCount=$(( deletedFileCount + 1 ))
      deletedBytes=$(( deletedBytes + recordSizeBytes ))
      removeEmptyParentFolders "$recordFilePath" "$managedFolderPath"
    else
      writeLogEntry "Could not delete $recordFilePath"
    fi
  done < <(sort -z -n -k1,1 -- "$fileListPath")
}

## Removes any other empty sub-folders (e.g. left by older runs). Recently
## changed folders are kept, because the camera may have just created them.
removeEmptySubfolders() {
  local folderPath="$1"
  timeout "$COMMAND_TIMEOUT_SECONDS" \
    find "$folderPath" -mindepth 1 -type d -empty -mmin "+$MIN_FILE_AGE_MINUTES" -delete 2> /dev/null
}

###############################################################################
## Shared cleanup method (used by both Google Drive and the flash drive)
###############################################################################

## Keeps one folder under its limit by deleting its oldest files.
##   folderLabel        name used in messages ("Google Drive", "Flash drive")
##   folderPath         folder to clean
##   limitGb            maximum folder size in GB
##   protectedFilePath  file that must never be deleted (the log file)
## Returns RESULT_OK, RESULT_ERROR, or RESULT_TIMEOUT when the folder could
## not be read in time (a hung mount).
enforceFolderLimit() {
  local folderLabel="$1"
  local folderPath="$2"
  local limitGb="$3"
  local protectedFilePath="$4"

  ## Target: the folder ends FREE_SPACE_RESERVE_GB below its limit
  local limitBytes
  limitBytes="$(convertGbToBytes "$limitGb")"
  local reserveBytes
  reserveBytes="$(convertGbToBytes "$FREE_SPACE_RESERVE_GB")"
  local targetUsageBytes=$(( limitBytes - reserveBytes ))

  ## Step 1: list all files once (sizes and ages come from this one scan)
  scanFolderFiles "$folderPath"
  local scanStatus=$?
  if [[ "$scanStatus" -eq "$TIMEOUT_EXIT_CODE" ]]; then
    return "$RESULT_TIMEOUT"
  fi
  if [[ "$scanStatus" -ne 0 ]]; then
    ## Usually files vanished during the scan; the list is still usable
    printStatus "$folderLabel scan reported errors (status $scanStatus), continuing"
  fi

  local folderUsageBytes
  folderUsageBytes="$(sumListedFileSizes)"

  local diskFreeBytes
  diskFreeBytes="$(readDiskFreeBytes "$folderPath")"
  local diskFreeStatus=$?
  if [[ "$diskFreeStatus" -eq "$TIMEOUT_EXIT_CODE" ]]; then
    return "$RESULT_TIMEOUT"
  fi
  if [[ "$diskFreeStatus" -ne 0 ]]; then
    writeLogEntry "$folderLabel free space could not be read, cleanup not ran"
    return "$RESULT_ERROR"
  fi

  ## Step 2: how much must be deleted to satisfy both the folder limit and
  ## the free space needed on the disk
  local bytesOverTarget=$(( folderUsageBytes - targetUsageBytes ))
  local bytesMissingOnDisk=$(( reserveBytes - diskFreeBytes ))
  local bytesToFree
  bytesToFree="$(largerOf "$bytesOverTarget" "$bytesMissingOnDisk")"

  local folderUsageGb
  folderUsageGb="$(formatBytesAsGb "$folderUsageBytes")"
  local diskFreeGb
  diskFreeGb="$(formatBytesAsGb "$diskFreeBytes")"

  if (( bytesToFree <= 0 )); then
    printStatus "$folderLabel using $folderUsageGb GB of $limitGb GB limit, $diskFreeGb GB disk free, no cleanup needed"
    return "$RESULT_OK"
  fi

  ## Step 3: delete oldest files first
  local bytesToFreeGb
  bytesToFreeGb="$(formatBytesAsGb "$bytesToFree")"
  writeLogEntry "$folderLabel using $folderUsageGb GB of $limitGb GB limit, $diskFreeGb GB disk free, deleting oldest files to free $bytesToFreeGb GB"

  deleteOldestFiles "$bytesToFree" "$protectedFilePath" "$folderPath"
  removeEmptySubfolders "$folderPath"

  local deletedGb
  deletedGb="$(formatBytesAsGb "$deletedBytes")"
  writeLogEntry "$folderLabel deleted $deletedFileCount files, freed $deletedGb GB"

  if (( deletedBytes < bytesToFree )); then
    writeLogEntry "$folderLabel WARNING: only files newer than $MIN_FILE_AGE_MINUTES minutes are left, could not free $bytesToFreeGb GB"
  fi
  return "$RESULT_OK"
}

###############################################################################
## Per-location cleanups
###############################################################################

## Cleans the Google Drive folder when its rclone mount is available;
## restarts rclone when the mount hangs
cleanupGoogleDrive() {
  isGoogleDriveMounted
  local mountStatus=$?
  if [[ "$mountStatus" -eq "$RESULT_TIMEOUT" ]]; then
    printStatus "Timeout exceeded restarting rclone"
    writeLogEntry "Google Drive mount did not respond within $MOUNT_CHECK_TIMEOUT_SECONDS seconds, restarting rclone"
    restartRcloneService
    return
  fi
  if [[ "$mountStatus" -ne "$RESULT_OK" ]]; then
    writeLogEntry "Google Drive not mounted ($GDRIVE_CAMERA_FOLDER), cleanup not ran"
    return
  fi
  printStatus "Google Drive connected"

  enforceFolderLimit "Google Drive" "$GDRIVE_CAMERA_FOLDER" "$GDRIVE_LIMIT_GB" ""
  local cleanupStatus=$?
  if [[ "$cleanupStatus" -eq "$RESULT_TIMEOUT" ]]; then
    printStatus "Timeout exceeded restarting rclone"
    writeLogEntry "Google Drive did not respond within $COMMAND_TIMEOUT_SECONDS seconds, restarting rclone"
    restartRcloneService
  fi
}

## Cleans the flash drive camera folder when the drive is connected
cleanupFlashDrive() {
  isFlashDriveConnected
  local mountStatus=$?
  if [[ "$mountStatus" -eq "$RESULT_TIMEOUT" ]]; then
    printStatus "Flash drive not responding"
    writeLogEntry "Flash drive mount did not respond within $MOUNT_CHECK_TIMEOUT_SECONDS seconds, cleanup not ran"
    return
  fi
  if [[ "$mountStatus" -ne "$RESULT_OK" ]]; then
    printStatus "Flash drive not connected"
    return
  fi
  printStatus "Flash drive connected"
  trimLogFile "$FLASH_LOG_FILE" "$FLASH_LOG_MAX_SIZE"

  enforceFolderLimit "Flash drive" "$FLASH_CAMERA_FOLDER" "$FLASH_LIMIT_GB" "$FLASH_LOG_FILE"
  local cleanupStatus=$?
  if [[ "$cleanupStatus" -eq "$RESULT_TIMEOUT" ]]; then
    writeLogEntry "Flash drive did not respond within $COMMAND_TIMEOUT_SECONDS seconds, cleanup not ran"
  fi
}

###############################################################################
## Main
###############################################################################

main() {
  if ! requireRootUser; then
    exit "$RESULT_ERROR"
  fi
  if ! validateConfiguration; then
    exit "$RESULT_ERROR"
  fi
  if ! acquireSingleInstanceLock; then
    echo "Another camcleanup is already running, exiting"
    exit "$RESULT_OK"
  fi
  trap removeTemporaryFiles EXIT
  if ! createFileListFile; then
    echo "Could not create a temporary file, exiting" >&2
    exit "$RESULT_ERROR"
  fi

  selectLogFile
  cleanupGoogleDrive
  cleanupFlashDrive

  ## Log file trim
  trimLogFile "$FALLBACK_LOG_FILE" "$FALLBACK_LOG_MAX_SIZE"
  exit "$RESULT_OK"
}

## Run main only when executed, so the tests can source the functions
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  main "$@"
fi
