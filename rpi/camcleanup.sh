#!/bin/bash
#
# camcleanup.sh - Cameras Cleanup Script for Pi
#
# Keeps the Google Drive sync folder, the USB flash drive camera folder and
# the camera fallback partition's folder (made by fallbackpart.sh) below a
# configurable size limit (in GB) by deleting the OLDEST files first.
#
# All three folders share the same cleanup method (enforceFolderLimit):
#   - a cleanup starts when the folder uses more than (limit - reserve),
#     or when the disk holding it has less than "reserve" free space
#   - the oldest files are deleted until the folder is at least
#     FREE_SPACE_RESERVE_GB below its limit and the disk has at least
#     FREE_SPACE_RESERVE_GB free (the fallback partition uses its own
#     FALLBACKPART_RESERVE_GB, as it is smaller than the shared reserve)
#   - files modified in the last MIN_FILE_AGE_MINUTES are never deleted,
#     because the camera or rclone may still be writing them
#   - a location is only cleaned when its mount point is mounted and its
#     camera folder exists (no internet check is needed)
#
# Run as root user only! (started by rpi-camcleanup.timer / rpi-camcleanup.service)
#
# Usage: camcleanup.sh [settings file]   (default /etc/camcleanup.conf)
#
# Every setting comes from the settings file, which a module's installer
# copies from its config/camcleanup.conf. Without the file nothing is cleaned.
# A location is only cleaned when its *_PATH setting is set, e.g.
#   GDRIVE_PATH=/mnt/smb/GSync
#   GDRIVE_CAMERA_FOLDER=/mnt/smb/GSync/Cameras/Doorway
#   GDRIVE_LIMIT_GB=14.5

set -o nounset
set -o pipefail

## Force a predictable number format for find (%T@) and sort -n
export LC_ALL=C

###############################################################################
## Settings (read from the settings file by loadConfiguration)
###############################################################################

## Settings file used when none is given on the command line
readonly DEFAULT_CONFIG_FILE="/etc/camcleanup.conf"

## Every setting the file may hold. Paths are written without a trailing slash.
##   FLASH_PATH                   flash drive mount point (empty = not cleaned)
##   FLASH_CAMERA_FOLDER          flash drive folder kept under the limit
##   FLASH_LIMIT_GB               its maximum size in GB (decimals allowed)
##   FLASH_LOG_FILE               cleanup log kept on the flash drive
##   FLASH_LOG_MAX_SIZE           its maximum size (truncate size, e.g. 5MB)
##   GDRIVE_PATH                  Google Drive mount point (empty = not cleaned)
##   GDRIVE_CAMERA_FOLDER         Google Drive folder kept under the limit
##   GDRIVE_LIMIT_GB              its maximum size in GB (decimals allowed)
##   RCLONE_SERVICE_NAME          rclone service restarted when the Drive mount
##                                hangs (empty = the mount is not this Pi's own
##                                rclone, e.g. an SMB share; nothing is restarted)
##   FALLBACKPART_PATH            fallback partition mount point (empty = not cleaned)
##   FALLBACKPART_CAMERA_FOLDER   fallback partition folder kept under the limit
##   FALLBACKPART_LIMIT_GB        its maximum size in GB (decimals allowed)
##   FALLBACKPART_RESERVE_GB      GB left free below its limit, and on the partition
##   FREE_SPACE_RESERVE_GB        GB left free below each other limit, and on each disk
##   MIN_FILE_AGE_MINUTES         files modified within this time are never deleted
##   MOUNT_CHECK_TIMEOUT_SECONDS  wait for a mount check before treating it as hung
##   COMMAND_TIMEOUT_SECONDS      time allowed for scanning a folder / free space
##   FALLBACK_LOG_FILE            cleanup log used without a flash drive
##   FALLBACK_LOG_MAX_SIZE        its maximum size (truncate size, e.g. 128KB)
readonly SETTING_NAMES=(
  FLASH_PATH FLASH_CAMERA_FOLDER FLASH_LIMIT_GB FLASH_LOG_FILE FLASH_LOG_MAX_SIZE
  GDRIVE_PATH GDRIVE_CAMERA_FOLDER GDRIVE_LIMIT_GB RCLONE_SERVICE_NAME
  FALLBACKPART_PATH FALLBACKPART_CAMERA_FOLDER FALLBACKPART_LIMIT_GB
  FALLBACKPART_RESERVE_GB FREE_SPACE_RESERVE_GB MIN_FILE_AGE_MINUTES MOUNT_CHECK_TIMEOUT_SECONDS
  COMMAND_TIMEOUT_SECONDS FALLBACK_LOG_FILE FALLBACK_LOG_MAX_SIZE
)

## Every setting starts empty, so a setting missing from the file stays unset
FLASH_PATH=""
FLASH_CAMERA_FOLDER=""
FLASH_LIMIT_GB=""
FLASH_LOG_FILE=""
FLASH_LOG_MAX_SIZE=""
GDRIVE_PATH=""
GDRIVE_CAMERA_FOLDER=""
GDRIVE_LIMIT_GB=""
RCLONE_SERVICE_NAME=""
FALLBACKPART_PATH=""
FALLBACKPART_CAMERA_FOLDER=""
FALLBACKPART_LIMIT_GB=""
FALLBACKPART_RESERVE_GB=""
FREE_SPACE_RESERVE_GB=""
MIN_FILE_AGE_MINUTES=""
MOUNT_CHECK_TIMEOUT_SECONDS=""
COMMAND_TIMEOUT_SECONDS=""
FALLBACK_LOG_FILE=""
FALLBACK_LOG_MAX_SIZE=""

## Lock file that stops two cleanups from running at the same time
readonly LOCK_FILE="/run/camcleanup.lock"
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

## Checks that one folder limit is a GB amount larger than its reserve,
## FREE_SPACE_RESERVE_GB unless another is named
##   validateFolderLimit "FLASH_LIMIT_GB" "$FLASH_LIMIT_GB"
##   validateFolderLimit "FALLBACKPART_LIMIT_GB" "$FALLBACKPART_LIMIT_GB" \
##     "FALLBACKPART_RESERVE_GB" "$FALLBACKPART_RESERVE_GB"
validateFolderLimit() {
  local limitName="$1"
  local limitGb="$2"
  local reserveName="${3:-FREE_SPACE_RESERVE_GB}"
  local reserveGb="${4:-$FREE_SPACE_RESERVE_GB}"
  if ! isGbAmount "$limitGb"; then
    echo "$limitName must be a number of GB like 14 or 14.5, got '$limitGb'" >&2
    return "$RESULT_ERROR"
  fi
  local limitBytes
  limitBytes="$(convertGbToBytes "$limitGb")"
  local reserveBytes
  reserveBytes="$(convertGbToBytes "$reserveGb")"
  if (( limitBytes <= reserveBytes )); then
    echo "$limitName ($limitGb GB) must be larger than $reserveName ($reserveGb GB)" >&2
    return "$RESULT_ERROR"
  fi
  return "$RESULT_OK"
}

## Prints the text without its leading and trailing whitespace
##   "  GDRIVE_LIMIT_GB " -> "GDRIVE_LIMIT_GB"
trimWhitespace() {
  local text="$1"
  local leadingWhitespace="${text%%[![:space:]]*}"
  text="${text#"$leadingWhitespace"}"
  local trailingWhitespace="${text##*[![:space:]]}"
  text="${text%"$trailingWhitespace"}"
  echo "$text"
}

## Succeeds when the name is one of SETTING_NAMES
isSettingName() {
  local candidateName="$1"
  local settingName
  for settingName in "${SETTING_NAMES[@]}"; do
    if [[ "$settingName" == "$candidateName" ]]; then
      return "$RESULT_OK"
    fi
  done
  return "$RESULT_ERROR"
}

## Reads the settings file into the setting variables, one KEY=value per
## line. Blank lines and lines starting with # are skipped, and a value is
## taken as written (no quotes, nothing expanded), e.g.
##   GDRIVE_LIMIT_GB=14.5
##   RCLONE_SERVICE_NAME=
## Fails when the file is missing or holds a line it does not understand.
loadConfiguration() {
  local configPath="$1"
  if [[ ! -f "$configPath" ]]; then
    echo "No settings file $configPath, cleanup not run" >&2
    return "$RESULT_ERROR"
  fi

  local configLine
  local lineNumber=0
  local trimmedLine
  local settingName
  local settingValue
  while IFS= read -r configLine || [[ -n "$configLine" ]]; do
    lineNumber=$(( lineNumber + 1 ))
    trimmedLine="$(trimWhitespace "$configLine")"
    if [[ -z "$trimmedLine" || "$trimmedLine" == \#* ]]; then
      continue
    fi
    if [[ "$trimmedLine" != *=* ]]; then
      echo "$configPath line $lineNumber is not KEY=value: '$trimmedLine'" >&2
      return "$RESULT_ERROR"
    fi
    settingName="$(trimWhitespace "${trimmedLine%%=*}")"
    settingValue="$(trimWhitespace "${trimmedLine#*=}")"
    if ! isSettingName "$settingName"; then
      echo "$configPath line $lineNumber: unknown setting '$settingName'" >&2
      return "$RESULT_ERROR"
    fi
    printf -v "$settingName" '%s' "$settingValue"
  done < "$configPath"
  return "$RESULT_OK"
}

## Fails when the named setting is empty
requireSetting() {
  local settingName="$1"
  local settingValue="${!settingName}"
  if [[ -z "$settingValue" ]]; then
    echo "$settingName must be set in the settings file" >&2
    return "$RESULT_ERROR"
  fi
  return "$RESULT_OK"
}

## Succeeds when the settings file names a flash drive to clean
isFlashDriveEnabled() {
  [[ -n "$FLASH_PATH" ]]
}

## Succeeds when the settings file names a Google Drive mount to clean
isGoogleDriveEnabled() {
  [[ -n "$GDRIVE_PATH" ]]
}

## Succeeds when the settings file names a camera fallback partition to clean
isFallbackPartitionEnabled() {
  [[ -n "$FALLBACKPART_PATH" ]]
}

## Checks all settings before anything is deleted: the shared ones always,
## each location's own only when that location is enabled
validateConfiguration() {
  if ! isFlashDriveEnabled && ! isGoogleDriveEnabled && ! isFallbackPartitionEnabled; then
    echo "None of FLASH_PATH, GDRIVE_PATH and FALLBACKPART_PATH is set, nothing to clean" >&2
    return "$RESULT_ERROR"
  fi
  requireSetting "FALLBACK_LOG_FILE" || return "$RESULT_ERROR"
  requireSetting "FALLBACK_LOG_MAX_SIZE" || return "$RESULT_ERROR"
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
  if isFlashDriveEnabled; then
    requireSetting "FLASH_CAMERA_FOLDER" || return "$RESULT_ERROR"
    requireSetting "FLASH_LOG_FILE" || return "$RESULT_ERROR"
    requireSetting "FLASH_LOG_MAX_SIZE" || return "$RESULT_ERROR"
    validateFolderLimit "FLASH_LIMIT_GB" "$FLASH_LIMIT_GB" || return "$RESULT_ERROR"
  fi
  if isGoogleDriveEnabled; then
    requireSetting "GDRIVE_CAMERA_FOLDER" || return "$RESULT_ERROR"
    validateFolderLimit "GDRIVE_LIMIT_GB" "$GDRIVE_LIMIT_GB" || return "$RESULT_ERROR"
  fi
  if isFallbackPartitionEnabled; then
    requireSetting "FALLBACKPART_CAMERA_FOLDER" || return "$RESULT_ERROR"
    if ! isGbAmount "$FALLBACKPART_RESERVE_GB"; then
      echo "FALLBACKPART_RESERVE_GB must be a number of GB like 0.5, got '$FALLBACKPART_RESERVE_GB'" >&2
      return "$RESULT_ERROR"
    fi
    validateFolderLimit "FALLBACKPART_LIMIT_GB" "$FALLBACKPART_LIMIT_GB" \
      "FALLBACKPART_RESERVE_GB" "$FALLBACKPART_RESERVE_GB" || return "$RESULT_ERROR"
  fi
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

## Checks that a managed folder is usable: the camera folder exists and its
## mount point is mounted. Both checks run under a timeout, because looking at
## a hung rclone (FUSE) mount blocks forever. The folder is checked first:
## reaching it mounts an autofs share (e.g. /mnt/smb/GSync), which the mount
## point check alone would not, so a share idle-unmounted by autofs is found.
## Returns RESULT_OK, RESULT_ERROR (not mounted / folder missing) or
## RESULT_TIMEOUT (the mount did not answer in time).
isStorageAvailable() {
  local mountPath="$1"
  local folderPath="$2"

  ## External "test" (not the shell builtin) so that timeout can stop it
  timeout "$MOUNT_CHECK_TIMEOUT_SECONDS" test -d "$folderPath"
  local folderCheckStatus=$?
  if [[ "$folderCheckStatus" -eq "$TIMEOUT_EXIT_CODE" ]]; then
    return "$RESULT_TIMEOUT"
  fi
  if [[ "$folderCheckStatus" -ne 0 ]]; then
    return "$RESULT_ERROR"
  fi

  timeout "$MOUNT_CHECK_TIMEOUT_SECONDS" mountpoint -q -- "$mountPath"
  local mountCheckStatus=$?
  if [[ "$mountCheckStatus" -eq "$TIMEOUT_EXIT_CODE" ]]; then
    return "$RESULT_TIMEOUT"
  fi
  if [[ "$mountCheckStatus" -ne 0 ]]; then
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

## Checks the fallback partition camera folder (same return values as above)
isFallbackPartitionMounted() {
  isStorageAvailable "$FALLBACKPART_PATH" "$FALLBACKPART_CAMERA_FOLDER"
}

## Restarts the rclone service that provides the Google Drive mount, when one
## is set, and logs which it did
##   problemMessage  what went wrong, e.g. "Google Drive did not respond"
restartRcloneService() {
  local problemMessage="$1"
  if [[ -z "$RCLONE_SERVICE_NAME" ]]; then
    printStatus "$problemMessage, no rclone service to restart"
    writeLogEntry "$problemMessage, no rclone service to restart"
    return
  fi
  printStatus "$problemMessage, restarting $RCLONE_SERVICE_NAME"
  writeLogEntry "$problemMessage, restarting $RCLONE_SERVICE_NAME"
  systemctl restart "$RCLONE_SERVICE_NAME"
}

## Logs to the flash drive when one is set and connected, otherwise to
## FALLBACK_LOG_FILE
selectLogFile() {
  if isFlashDriveEnabled && isFlashDriveConnected; then
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
## Shared cleanup method (Google Drive, the flash drive, the fallback partition)
###############################################################################

## Keeps one folder under its limit by deleting its oldest files.
##   folderLabel        name used in messages ("Google Drive", "Flash drive")
##   folderPath         folder to clean
##   limitGb            maximum folder size in GB
##   protectedFilePath  file that must never be deleted (the log file)
##   reserveGb          GB left free below the limit and on the disk
##                      (optional, FREE_SPACE_RESERVE_GB when not given)
## Returns RESULT_OK, RESULT_ERROR, or RESULT_TIMEOUT when the folder could
## not be read in time (a hung mount).
enforceFolderLimit() {
  local folderLabel="$1"
  local folderPath="$2"
  local limitGb="$3"
  local protectedFilePath="$4"
  local reserveGb="${5:-$FREE_SPACE_RESERVE_GB}"

  ## Target: the folder ends reserveGb below its limit
  local limitBytes
  limitBytes="$(convertGbToBytes "$limitGb")"
  local reserveBytes
  reserveBytes="$(convertGbToBytes "$reserveGb")"
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

## Cleans the Google Drive folder when one is set and its mount is available;
## restarts rclone when the mount hangs
cleanupGoogleDrive() {
  if ! isGoogleDriveEnabled; then
    return
  fi
  isGoogleDriveMounted
  local mountStatus=$?
  if [[ "$mountStatus" -eq "$RESULT_TIMEOUT" ]]; then
    restartRcloneService "Google Drive mount did not respond within $MOUNT_CHECK_TIMEOUT_SECONDS seconds"
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
    restartRcloneService "Google Drive did not respond within $COMMAND_TIMEOUT_SECONDS seconds"
  fi
}

## Cleans the flash drive camera folder when one is set and the drive is
## connected
cleanupFlashDrive() {
  if ! isFlashDriveEnabled; then
    return
  fi
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

## Cleans the camera fallback partition's folder when one is set and the
## partition is mounted (boot.sh mounts it only when it is there and sound)
cleanupFallbackPartition() {
  if ! isFallbackPartitionEnabled; then
    return
  fi
  isFallbackPartitionMounted
  local mountStatus=$?
  if [[ "$mountStatus" -eq "$RESULT_TIMEOUT" ]]; then
    printStatus "Fallback partition not responding"
    writeLogEntry "Fallback partition mount did not respond within $MOUNT_CHECK_TIMEOUT_SECONDS seconds, cleanup not ran"
    return
  fi
  if [[ "$mountStatus" -ne "$RESULT_OK" ]]; then
    printStatus "Fallback partition not mounted"
    return
  fi
  printStatus "Fallback partition mounted"

  enforceFolderLimit "Fallback partition" "$FALLBACKPART_CAMERA_FOLDER" "$FALLBACKPART_LIMIT_GB" "" \
    "$FALLBACKPART_RESERVE_GB"
  local cleanupStatus=$?
  if [[ "$cleanupStatus" -eq "$RESULT_TIMEOUT" ]]; then
    writeLogEntry "Fallback partition did not respond within $COMMAND_TIMEOUT_SECONDS seconds, cleanup not ran"
  fi
}

###############################################################################
## Main
###############################################################################

main() {
  local configPath="${1:-$DEFAULT_CONFIG_FILE}"
  if ! requireRootUser; then
    exit "$RESULT_ERROR"
  fi
  if ! loadConfiguration "$configPath"; then
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
  cleanupFallbackPartition

  ## Log file trim
  trimLogFile "$FALLBACK_LOG_FILE" "$FALLBACK_LOG_MAX_SIZE"
  exit "$RESULT_OK"
}

## Run main only when executed, so the tests can source the functions
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  main "$@"
fi
