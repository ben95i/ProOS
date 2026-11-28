#!/bin/bash
###########################################################
###########################################################
### runs as root ###
## The backup jobs the website's backup wizard runs: attaches the external
## backup pools listed in /opt/bkpdrives.txt (import, unlock, mount), backs the
## tank datastore (Data, Regions, Media) up to them with rsync, backs every
## Proxmox guest up (pve-backup.sh), detaches the pools, and reports their state.
##
## Usage: tank-backup.sh <command> <RAM disk path>
##   bkp_attach            attaches the connected pools with the passphrase in <RAM disk>/bkppwd.save.txt
##   bkp_standard          rsyncs tank to every attached pool
##   bkp_checksum          the same with a checksum compare (slow)
##   bkp_standard_eject    either backup, then detaches the pools (unless cancelled)
##   bkp_checksum_eject
##   bkp_vms               vzdump of every guest into the repo (pve-backup.sh)
##   bkp_detach            unmounts, locks and exports every imported pool
##   bkp_cancel            stops the running backup or VM backup
##   bkp_status            rewrites <RAM disk>/bkpstatus.save.txt, printing nothing
## e.g.   tank-backup.sh bkp_status /mnt/ramdisk
## Run by apicmds.sh, which file-monitor runs with its output going to bkplog.save.txt;
## nothing here writes to any other command's output file.
##
## RAM disk files:
##   bkpjob.lock         "<job>|<pid>" while a job runs: one job at a time
##   bkpworker.pid       the running rsync or pve-backup.sh, for bkp_cancel
##   bkpcancel.flag      cancel asked for
##   bkpstatus.save.txt  every pool's state (see WRITE_STATUS)
##   bkplog.save.txt     the current job's log: begun afresh by each job, plain text lines
##                       and event lines starting "@", fields split by "|":
##     @JOB|<job>|<start, epoch ms>             first line: attach, standard, checksum, vms, detach
##     @PHASE|<phase>                           attach, backup, record, eject, detach
##     @STEP|<n>|<count>|<group>|<item>         a unit of the phase starts: pool and share,
##                                              guest id and name, or pool and attach/detach
##     @PROGRESS|<step %>|<overall %>|<transferred>|<rate>|<eta>
##                                              step % is - while the worker gives none;
##                                              overall % counts steps of the phase
##     @STEP_END|<n>|<ok, warn, failed or cancelled>|<detail>
##     @POOL|<pool>|<outcome>                   attached, already, wrongpass, failed, done,
##                                              incomplete, skipped, detached, busy
##     @CANCEL|<epoch ms>                       bkp_cancel was run
##     @REFUSED|<job>|<running job>             asked for while another job ran
##     @DONE|<job>|<ok, failed or cancelled>|<summary>   last line of the job

cmd="${1}"
ramDisk="${2}"

extBkps="/mnt/extbkps"
statusFile="$extBkps/status.txt"
drivesPath="/opt/bkpdrives.txt"
tankRoot="/mnt/datastore"
regionsData="$tankRoot/.regions"
tankData="$tankRoot/data"
tankMedia="$tankRoot/media"
proosDir="$tankData/Projects/ProOS"
pveBackupScript="/usr/bin/pve-backup.sh"
## stdbuf keeps rsync's progress flowing line by line into the filter
rsyncBin="stdbuf -oL nice -n 19 ionice -c 3 rsync"
## --no-inc-recursive scans the whole tree first, so progress2's percent is of the whole share
rsyncOptions="-a --partial --delete --delete-excluded --no-inc-recursive --info=progress2 --stats"
checksumOption=""
## the host's own pools, never imported or exported here
protectedPools=" tank datastore rpool "
## a @PROGRESS line at most this often (seconds) while the percent stands still
progressEvery=5

## RAM disk files
jobLock="$ramDisk/bkpjob.lock"
workerPidPath="$ramDisk/bkpworker.pid"
cancelFlag="$ramDisk/bkpcancel.flag"
drivesStatusPath="$ramDisk/bkpstatus.save.txt"
logPath="$ramDisk/bkplog.save.txt"
passwordPath="$ramDisk/bkppwd.save.txt"

## the running job: its name, whether this process holds the lock, its FIFO folder
jobName=""
lockHeld="no"
doneWritten="no"
jobDir=""

## Prints the date and host under a job's log, then exits.
function END_ROUTINE() {
  local trailerText
  trailerText=$(date)
  trailerText+=" ("
  trailerText+=$(hostname)
  trailerText+=")"
  echo "$trailerText"
  echo " "
  exit
}

## Ends the command unless argument 2 named a RAM disk folder.
function REQUIRE_RAMDISK() {
  if [ -z "$ramDisk" ] || [ ! -d "$ramDisk" ]; then
    echo "invalid or missing RAM disk path (argument 2)!"
    echo "aborting before operating on '/'."
    exit 1
  fi
}

## Prints one event line for the log: EVENT STEP 2 5 hdd4tb-03 Media -> @STEP|2|5|hdd4tb-03|Media
function EVENT() {
  local IFS='|'
  echo "@$*"
}

## Prints the time now in epoch milliseconds, the API's own clock (the automate LXC
## shares the host's), so the page can tell a job it started from an older one.
function EPOCH_MS() {
  date +%s%3N
}

## Job Lock and Log ##

## Prints the job holding the lock ("standard"), or nothing when none does.
function LOCKED_JOB() {
  local lockLine=""
  if [ -e "$jobLock" ]; then
    lockLine=$(cat "$jobLock" 2>/dev/null)
    if [ -z "$lockLine" ]; then
      lockLine="busy"
    fi
  fi
  echo "${lockLine%%|*}"
}

## Prints the PID of the process holding the lock, or nothing.
function LOCKED_PID() {
  local lockLine=""
  if [ -e "$jobLock" ]; then
    lockLine=$(cat "$jobLock" 2>/dev/null)
  fi
  if [[ "$lockLine" == *"|"* ]]; then
    echo "${lockLine#*|}"
  fi
}

## Starts a job: takes the job lock (or prints @REFUSED and answers 1, the running
## job's log getting that line), begins a fresh bkplog.save.txt with its @JOB line and sends
## all further output into it. A new file rather than an emptied one, so the API can
## tell the page a new job's log began by its inode.
## Usage: START_JOB <job>   e.g. START_JOB standard || exit 0
function START_JOB() {
  local newJob="$1"
  local tempLog="$ramDisk/bkplog.$$.tmp"
  local runningJob
  if ! ( set -o noclobber; printf '%s|%s\n' "$newJob" "$$" > "$jobLock" ) 2>/dev/null; then
    runningJob=$(LOCKED_JOB)
    EVENT REFUSED "$newJob" "$runningJob"
    echo "backup busy ($runningJob running), $newJob not started."
    return 1
  fi
  lockHeld="yes"
  jobName="$newJob"
  rm -f "$cancelFlag" "$workerPidPath"
  EVENT JOB "$jobName" "$(EPOCH_MS)" > "$tempLog"
  chmod 0644 "$tempLog"
  mv -f "$tempLog" "$logPath"
  exec >> "$logPath" 2>&1
  WRITE_STATUS
}

## Writes the job's last line. Usage: FINISH_JOB <ok|failed|cancelled> <summary>
function FINISH_JOB() {
  local jobResult="$1"
  local jobSummary="$2"
  echo " "
  EVENT DONE "$jobName" "$jobResult" "$jobSummary"
  doneWritten="yes"
}

## A job's EXIT trap: stops a worker the job left running (the job itself was killed),
## closes the log with @DONE when the job ended early, shreds the passphrase, drops the
## cancel flag, the FIFO and the lock, and rewrites the status.
function JOB_EXIT() {
  local workerPid
  if [ -e "$workerPidPath" ] && [ "$lockHeld" == "yes" ]; then
    workerPid=$(cat "$workerPidPath" 2>/dev/null)
    if [ -n "$workerPid" ]; then
      KILL_TREE "$workerPid"
    fi
    rm -f "$workerPidPath"
  fi
  if [ "$lockHeld" == "yes" ] && [ "$doneWritten" != "yes" ]; then
    FINISH_JOB failed "interrupted"
  fi
  SHRED_PASSWORD
  if [ -n "$jobDir" ] && [ -d "$jobDir" ]; then
    rm -f "$jobDir/worker.fifo"
    rmdir "$jobDir"
  fi
  if [ "$lockHeld" == "yes" ]; then
    rm -f "$cancelFlag" "$jobLock"
    lockHeld="no"
  fi
  WRITE_STATUS
}

## Shreds the passphrase file bkp_attach reads, so it never outlives the job.
function SHRED_PASSWORD() {
  if [ -e "$passwordPath" ]; then
    if ! shred -n 2 -z -u "$passwordPath" 2>/dev/null; then
      rm -f "$passwordPath"
    fi
  fi
}

## True once bkp_cancel has asked the running job to stop.
function CANCEL_REQUESTED() {
  [ -e "$cancelFlag" ]
}

## Sends SIGTERM to a process and all its descendants, the deepest first.
## Usage: KILL_TREE 12345
function KILL_TREE() {
  local parentPid="$1"
  local childPid
  for childPid in $(pgrep -P "$parentPid"); do
    KILL_TREE "$childPid"
  done
  kill -TERM "$parentPid" 2>/dev/null
}

## Pool State ##

## Reads the drives file's pool names into backupPools, skipping blank lines and the
## host's own pools; with "verbose" a missing file and each refused name are reported.
## Returns 1 when the drives file is missing.
## Usage: READ_POOLS [verbose]   e.g. READ_POOLS verbose || return 1
backupPools=()
function READ_POOLS() {
  local reportMode="$1"
  local poolLine
  local poolName
  backupPools=()
  if [ ! -e "$drivesPath" ]; then
    if [ "$reportMode" == "verbose" ]; then
      echo "drives file '$drivesPath' not found!"
    fi
    return 1
  fi
  while IFS= read -r poolLine || [ -n "$poolLine" ]; do
    ## pool names hold no spaces; this also drops a DOS line end
    poolName="${poolLine//[[:space:]]/}"
    if [ -z "$poolName" ]; then
      continue
    fi
    if [[ "$protectedPools" == *" $poolName "* ]]; then
      if [ "$reportMode" == "verbose" ]; then
        echo "invalid pool specified: $poolName."
      fi
      continue
    fi
    backupPools+=( "$poolName" )
  done < "$drivesPath"
}

## Lists the pools connected but not imported into importablePools (" pool1 pool2 ").
## zpool import reads every disk's label, so it runs once per pass, not once per pool.
importablePools=" "
function LIST_IMPORTABLE() {
  local scannedNames
  scannedNames=$(zpool import 2>/dev/null | awk '$1 == "pool:" { print $2 }' | tr '\n' ' ')
  importablePools=" $scannedNames "
}

## True when a pool is connected but not imported, as the last LIST_IMPORTABLE saw it.
## Usage: POOL_IMPORTABLE hdd4tb-03
function POOL_IMPORTABLE() {
  local poolName="$1"
  [[ "$importablePools" == *" $poolName "* ]]
}

## True when a pool is imported. Usage: POOL_IMPORTED hdd4tb-03
function POOL_IMPORTED() {
  local poolName="$1"
  zpool list -H -o name "$poolName" >/dev/null 2>&1
}

## True when a pool's extbkp dataset has its key loaded. Usage: KEY_LOADED hdd4tb-03
function KEY_LOADED() {
  local poolName="$1"
  local keyStatus
  keyStatus=$(zfs get -H -o value keystatus "$poolName/extbkp" 2>/dev/null)
  [ "$keyStatus" == "available" ]
}

## True only when ZFS has <pool>/extbkp mounted at /mnt/extbkps/<pool>: its own
## mounted and mountpoint properties, and the mount on that folder being that
## dataset. A folder left on the boot pool under an unmounted mountpoint never passes,
## so a backup never writes there.
## Usage: POOL_MOUNTED hdd4tb-03
function POOL_MOUNTED() {
  local poolName="$1"
  local datasetName="$poolName/extbkp"
  local expectedPath="$extBkps/$poolName"
  local mountedValue
  local mountpointValue
  local mountSource
  mountedValue=$(zfs get -H -o value mounted "$datasetName" 2>/dev/null)
  if [ "$mountedValue" != "yes" ]; then
    return 1
  fi
  mountpointValue=$(zfs get -H -o value mountpoint "$datasetName" 2>/dev/null)
  if [ "$mountpointValue" != "$expectedPath" ]; then
    return 1
  fi
  mountSource=$(findmnt -n -o SOURCE --mountpoint "$expectedPath" 2>/dev/null)
  [ "$mountSource" == "$datasetName" ]
}

## Prints a pool's state: attached (mounted where the backup writes), unmounted
## (imported but not mounted there), connected (importable) or absent.
## Needs LIST_IMPORTABLE first. Usage: POOL_STATE hdd4tb-03
function POOL_STATE() {
  local poolName="$1"
  if POOL_MOUNTED "$poolName"; then
    echo "attached"
    return
  fi
  if POOL_IMPORTED "$poolName"; then
    echo "unmounted"
    return
  fi
  if POOL_IMPORTABLE "$poolName"; then
    echo "connected"
    return
  fi
  echo "absent"
}

## Prints the shares a pool gets, by its name's type: "Data Regions" for usb*,
## "Data Regions Media" for hdd*, nothing for any other.
## Usage: POOL_SHARES hdd4tb-03
function POOL_SHARES() {
  local poolName="$1"
  local poolType="${poolName::3}"
  if [ "$poolType" == "usb" ]; then
    echo "Data Regions"
  fi
  if [ "$poolType" == "hdd" ]; then
    echo "Data Regions Media"
  fi
}

## Prints a pool's last backup from the status file as "2026-11-20 15:12", or "never".
## Usage: LAST_BACKUP_DATE hdd4tb-03
function LAST_BACKUP_DATE() {
  local poolName="$1"
  local statusLine
  local savedDate=""
  local readableDate
  local sortableDate
  if [ -f "$statusFile" ]; then
    while IFS= read -r statusLine; do
      if [ "${statusLine%%|*}" == "$poolName" ]; then
        savedDate="${statusLine#*|}"
      fi
    done < "$statusFile"
  fi
  if [ -z "$savedDate" ]; then
    echo "never"
    return
  fi
  ## saved as '%F %r' with underscores for spaces: 2026-11-20_03:12:44_PM
  readableDate="${savedDate//_/ }"
  if ! sortableDate=$(date -d "$readableDate" '+%F %H:%M' 2>/dev/null); then
    sortableDate="$readableDate"
  fi
  echo "$sortableDate"
}

## Rewrites the RAM disk's bkpstatus.save.txt, one row a line, for the API:
##   STATE|idle                                (or the job holding the lock)
##   UPDATED|2026-11-27 14:03:22.123456789
##   POOL|hdd4tb-03|attached|2026-11-20 15:12|Data Regions Media
##        (a POOL_STATE, LAST_BACKUP_DATE and POOL_SHARES)
## Written to a temp file and moved into place, so a reader never sees half of it.
function WRITE_STATUS() {
  local tempPath="$ramDisk/bkpstatus.$$.tmp"
  local driveState
  local poolName
  local poolState
  local lastBackup
  local poolShares
  driveState=$(LOCKED_JOB)
  if [ -z "$driveState" ]; then
    driveState="idle"
  fi
  READ_POOLS
  LIST_IMPORTABLE
  {
    echo "STATE|$driveState"
    echo "UPDATED|$(date '+%F %T.%N')"
    for poolName in "${backupPools[@]}"; do
      poolState=$(POOL_STATE "$poolName")
      lastBackup=$(LAST_BACKUP_DATE "$poolName")
      poolShares=$(POOL_SHARES "$poolName")
      echo "POOL|$poolName|$poolState|$lastBackup|$poolShares"
    done
  } > "$tempPath"
  chmod 0644 "$tempPath"
  mv -f "$tempPath" "$drivesStatusPath"
}

## Attach and Detach ##

## Detaches one imported pool: unmounts its extbkp dataset, unloads its key and exports
## it. Returns 1, leaving it imported, when the dataset is busy or the export fails.
## Usage: EXPORT_POOL <pool>   e.g. EXPORT_POOL hdd4tb-03
function EXPORT_POOL() {
  local poolName="$1"
  local datasetName="$poolName/extbkp"
  local mountedValue
  mountedValue=$(zfs get -H -o value mounted "$datasetName" 2>/dev/null)
  if [ "$mountedValue" == "yes" ]; then
    if ! zfs unmount "$datasetName"; then
      echo "WARNING: could not unmount $datasetName (in use?), skipping export."
      return 1
    fi
  fi
  if KEY_LOADED "$poolName"; then
    zfs unload-key "$datasetName"
  fi
  if ! zpool export "$poolName"; then
    echo "WARNING: export of $poolName failed (pool still in use?)."
    return 1
  fi
}

## Attaches one pool: imports it unless it is, loads its key from the passphrase file
## and mounts its extbkp dataset at /mnt/extbkps/<pool>. A pool whose key or mount fails
## is exported again, so a wrong passphrase leaves nothing half attached. Sets
## attachOutcome: attached, already, wrongpass or failed.
## Usage: ATTACH_POOL <pool>   e.g. ATTACH_POOL hdd4tb-03; echo "$attachOutcome"
function ATTACH_POOL() {
  local poolName="$1"
  if POOL_MOUNTED "$poolName"; then
    echo "$poolName already attached."
    attachOutcome="already"
    return
  fi
  if ! POOL_IMPORTED "$poolName"; then
    echo "importing ZFS backup volume $poolName."
    if ! zpool import "$poolName"; then
      echo "WARNING: import of $poolName failed."
      attachOutcome="failed"
      return
    fi
  fi
  if ! KEY_LOADED "$poolName"; then
    if ! zfs load-key -L "file://$passwordPath" "$poolName/extbkp"; then
      echo "WARNING: wrong passphrase for $poolName, exporting it again."
      EXPORT_POOL "$poolName"
      attachOutcome="wrongpass"
      return
    fi
  fi
  zfs mount "$poolName/extbkp"
  if ! POOL_MOUNTED "$poolName"; then
    echo "WARNING: $poolName/extbkp is not mounted at $extBkps/$poolName, exporting it again."
    EXPORT_POOL "$poolName"
    attachOutcome="failed"
    return
  fi
  zpool status "$poolName"
  echo "attached $poolName."
  attachOutcome="attached"
}

## Attaches every connected or imported pool, one @STEP each, printing each one's @POOL
## outcome; pools not connected are only listed. Sets attachedCount and failedCount, or
## answers 1 with poolsError set when it cannot start.
## Usage: ATTACH_POOLS   (in a job, with the passphrase in bkppwd.save.txt)
attachOutcome=""
attachedCount=0
failedCount=0
poolsError=""
function ATTACH_POOLS() {
  local poolName
  local candidatePools=()
  local absentPools=""
  local stepIndex=0
  local stepResult
  EVENT PHASE attach
  if [ ! -e "$passwordPath" ]; then
    echo "password file not found!"
    poolsError="passphrase not received"
    return 1
  fi
  if ! READ_POOLS verbose; then
    poolsError="drives file not found"
    return 1
  fi
  LIST_IMPORTABLE
  for poolName in "${backupPools[@]}"; do
    if POOL_IMPORTED "$poolName" || POOL_IMPORTABLE "$poolName"; then
      candidatePools+=( "$poolName" )
    else
      absentPools+=" $poolName"
    fi
  done
  for poolName in "${candidatePools[@]}"; do
    stepIndex=$(( stepIndex + 1 ))
    EVENT STEP "$stepIndex" "${#candidatePools[@]}" "$poolName" attach
    ATTACH_POOL "$poolName"
    EVENT POOL "$poolName" "$attachOutcome"
    stepResult="ok"
    if [ "$attachOutcome" == "attached" ] || [ "$attachOutcome" == "already" ]; then
      attachedCount=$(( attachedCount + 1 ))
    else
      failedCount=$(( failedCount + 1 ))
      stepResult="failed"
    fi
    EVENT STEP_END "$stepIndex" "$stepResult" "$attachOutcome"
    echo " "
  done
  zfs list
  echo " "
  if [ -n "$absentPools" ]; then
    echo "not connected:$absentPools"
  fi
}

## Detaches every imported backup pool, mounted or not, so a pool left imported by a
## failed attach is exported too, one @STEP each with its @POOL outcome (detached or
## busy). Sets detachedCount and busyCount, or answers 1 with poolsError set when it
## cannot start.
## Usage: DETACH_POOLS   (in a job)
detachedCount=0
busyCount=0
function DETACH_POOLS() {
  local poolName
  local importedPools=()
  local stepIndex=0
  EVENT PHASE detach
  if ! READ_POOLS verbose; then
    poolsError="drives file not found"
    return 1
  fi
  for poolName in "${backupPools[@]}"; do
    if POOL_IMPORTED "$poolName"; then
      importedPools+=( "$poolName" )
    fi
  done
  if [ ${#importedPools[@]} -eq 0 ]; then
    echo "no backup drives attached."
  fi
  for poolName in "${importedPools[@]}"; do
    stepIndex=$(( stepIndex + 1 ))
    EVENT STEP "$stepIndex" "${#importedPools[@]}" "$poolName" detach
    echo "detaching ZFS backup volume $poolName."
    if EXPORT_POOL "$poolName"; then
      EVENT POOL "$poolName" detached
      EVENT STEP_END "$stepIndex" ok detached
      detachedCount=$(( detachedCount + 1 ))
    else
      EVENT POOL "$poolName" busy
      EVENT STEP_END "$stepIndex" failed busy
      busyCount=$(( busyCount + 1 ))
    fi
    echo " "
  done
  zpool list
  echo " "
}

## Workers and Progress ##

## True when a @PROGRESS line is due for this percent: the percent changed, or the last
## one is progressEvery seconds old. Works on the calling filter's lastPercent and
## lastProgressTime. Usage: PROGRESS_DUE 45 && EMIT_PROGRESS ...
function PROGRESS_DUE() {
  local stepPercent="$1"
  local secondsSince=$(( SECONDS - lastProgressTime ))
  if [ "$stepPercent" == "$lastPercent" ] && [ $secondsSince -lt $progressEvery ]; then
    return 1
  fi
  lastPercent="$stepPercent"
  lastProgressTime=$SECONDS
}

## Prints a @PROGRESS line, the overall percent counting each step of the phase as an
## equal share. Usage: EMIT_PROGRESS <step n> <step count> <step % or -> <transferred> <rate> <eta>
## e.g.   EMIT_PROGRESS 2 4 50 1.2GiB 10.39MB/s 0:01:53 -> @PROGRESS|50|37|1.2GiB|10.39MB/s|0:01:53
function EMIT_PROGRESS() {
  local stepIndex="$1"
  local stepCount="$2"
  local stepPercent="$3"
  local stepShare=0
  local doneSteps=$(( stepIndex - 1 ))
  local overallPercent=0
  if [ "$stepPercent" != "-" ]; then
    stepShare=$stepPercent
  fi
  if [ "$stepCount" -gt 0 ]; then
    overallPercent=$(( (doneSteps * 100 + stepShare) / stepCount ))
  fi
  EVENT PROGRESS "$stepPercent" "$overallPercent" "$4" "$5" "$6"
}

## Prints a worker's text line as log text, an "@" at its start escaped with a space so
## it never reads as an event. Usage: LOG_LINE "$outputLine"
function LOG_LINE() {
  local outputLine="$1"
  if [ -z "$outputLine" ]; then
    return
  fi
  if [ "${outputLine::1}" == "@" ]; then
    outputLine=" $outputLine"
  fi
  echo "$outputLine"
}

## Filters rsync --info=progress2 output for one step: its progress lines
## ("  1,238,099,968  45%   10.39MB/s    0:01:53 (xfr#123, to-chk=456/7890)") become
## @PROGRESS lines, everything else (the --stats block, errors) is logged.
## Usage: <rsync output> | RSYNC_FILTER <step n> <step count>
function RSYNC_FILTER() {
  local stepIndex="$1"
  local stepCount="$2"
  local outputLine
  local lastPercent=""
  local lastProgressTime=0
  local byteCount
  local transferred
  local progressPattern='^[[:space:]]*([0-9,]+)[[:space:]]+([0-9]+)%[[:space:]]+([^[:space:]]+)[[:space:]]+([0-9]+:[0-9]{2}:[0-9]{2})'
  PROGRESS_DUE 0
  EMIT_PROGRESS "$stepIndex" "$stepCount" 0 0B - -
  while IFS= read -r outputLine; do
    if [[ "$outputLine" =~ $progressPattern ]]; then
      if PROGRESS_DUE "${BASH_REMATCH[2]}"; then
        byteCount="${BASH_REMATCH[1]//,/}"
        if ! transferred=$(numfmt --to=iec-i --suffix=B "$byteCount" 2>/dev/null); then
          transferred="${byteCount}B"
        fi
        EMIT_PROGRESS "$stepIndex" "$stepCount" "${BASH_REMATCH[2]}" "$transferred" \
          "${BASH_REMATCH[3]}" "${BASH_REMATCH[4]}"
      fi
      continue
    fi
    LOG_LINE "$outputLine"
  done
}

## Filters pve-backup.sh's output for a VM backup, whose steps it announces itself:
##   "Backing-up Files LXC 101 (1 of 9)..."      -> @STEP|1|9|101|Files LXC
##   "INFO:  45% (1.2 GiB of 10.0 GiB) in 5s, read: 245.8 MiB/s, write: 240.2 MiB/s"
##                                               -> @PROGRESS (a KVM guest; an LXC gives none)
##   "Backed-up Files LXC 101 (1 of 9)."         -> @STEP_END|1|ok|
##   "FAILED: Files LXC 101 (1 of 9): <reason>"  -> @STEP_END|1|failed|<reason>
## Everything else is logged; a step the output ends inside (a cancel, or pve-backup.sh
## killed) is closed as cancelled or failed. Usage: <pve-backup.sh output> | VZDUMP_FILTER
function VZDUMP_FILTER() {
  local outputLine
  local lastPercent=""
  local lastProgressTime=0
  local stepIndex=0
  local stepCount=0
  local stepOpen="no"
  local guestName
  local startPattern='^Backing-up (.+) ([0-9]+) \(([0-9]+) of ([0-9]+)\)\.\.\.$'
  local okPattern='^Backed-up .+ \(([0-9]+) of [0-9]+\)\.$'
  local failPattern='^FAILED: .+ \(([0-9]+) of [0-9]+\): (.*)$'
  local percentPattern='^INFO:[[:space:]]+([0-9]+)% \((.+)\) in [^,]+, read: ([^,]+), write: '
  while IFS= read -r outputLine; do
    if [[ "$outputLine" =~ $startPattern ]]; then
      guestName="${BASH_REMATCH[1]}"
      stepIndex="${BASH_REMATCH[3]}"
      stepCount="${BASH_REMATCH[4]}"
      LOG_LINE "$outputLine"
      EVENT STEP "$stepIndex" "$stepCount" "${BASH_REMATCH[2]}" "$guestName"
      stepOpen="yes"
      lastPercent=""
      PROGRESS_DUE -
      EMIT_PROGRESS "$stepIndex" "$stepCount" - - - -
      continue
    fi
    if [[ "$outputLine" =~ $percentPattern ]]; then
      if PROGRESS_DUE "${BASH_REMATCH[1]}"; then
        EMIT_PROGRESS "$stepIndex" "$stepCount" "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" \
          "${BASH_REMATCH[3]}" -
      fi
      continue
    fi
    if [[ "$outputLine" =~ $okPattern ]]; then
      LOG_LINE "$outputLine"
      EVENT STEP_END "${BASH_REMATCH[1]}" ok ""
      stepOpen="no"
      continue
    fi
    if [[ "$outputLine" =~ $failPattern ]]; then
      LOG_LINE "$outputLine"
      EVENT STEP_END "${BASH_REMATCH[1]}" failed "${BASH_REMATCH[2]}"
      stepOpen="no"
      continue
    fi
    LOG_LINE "$outputLine"
  done
  if [ "$stepOpen" == "yes" ]; then
    if CANCEL_REQUESTED; then
      EVENT STEP_END "$stepIndex" cancelled "cancelled"
    else
      EVENT STEP_END "$stepIndex" failed "pve-backup.sh stopped"
    fi
  fi
}

## Runs a worker (rsync or pve-backup.sh) in the background, its output through a FIFO
## into a filter, its PID in bkpworker.pid so bkp_cancel can stop it. Answers the
## worker's exit code (rsync answers 20 when it was stopped).
## Usage: RUN_WORKER <filter> <step n> <step count> <command...>
## e.g.   RUN_WORKER RSYNC_FILTER 2 5 rsync -a /src/ /dst/
function RUN_WORKER() {
  local filterName="$1"
  local stepIndex="$2"
  local stepCount="$3"
  shift 3
  local fifoPath="$jobDir/worker.fifo"
  local workerPid
  local workerCode
  rm -f "$fifoPath"
  mkfifo "$fifoPath"
  "$@" > "$fifoPath" 2>&1 &
  workerPid=$!
  echo "$workerPid" > "$workerPidPath"
  stdbuf -oL tr '\r' '\n' < "$fifoPath" | "$filterName" "$stepIndex" "$stepCount"
  wait "$workerPid"
  workerCode=$?
  rm -f "$workerPidPath" "$fifoPath"
  return $workerCode
}

## Makes the job's FIFO folder outside the RAM disk, where file-monitor never sees it.
function MAKE_JOB_DIR() {
  jobDir=$(mktemp -d /tmp/proos-bkp.XXXXXX)
}

## Drive Backup ##

## Sets syncSource, syncSanity, syncFolder and syncExcludes for one share of a pool:
## where it comes from on tank, a path it always holds (so an unmounted datastore is
## never mirrored over a backup with --delete), its folder on the pool, and what a usb*
## pool leaves out (disk images, the Archive region's files, Games).
## Usage: SHARE_SETTINGS <pool> <Data|Regions|Media>
function SHARE_SETTINGS() {
  local poolName="$1"
  local shareName="$2"
  local poolType="${poolName::3}"
  syncExcludes=()
  if [ "$shareName" == "Data" ]; then
    syncSource="$tankData"
    syncSanity="$proosDir"
    syncFolder="Data"
    if [ "$poolType" == "usb" ]; then
      syncExcludes=( --exclude="**CDs.zip" --exclude="**CDs.7z" --exclude="**USB.img" \
        --exclude="**USB.7z" --exclude="**USB.zip" --exclude="**HD.img" --exclude="**HD.7z" \
        --exclude="**HD.zip" --exclude="**VM.img" --exclude="**VM.7z" --exclude="**VM.zip" )
    fi
  fi
  if [ "$shareName" == "Regions" ]; then
    syncSource="$regionsData"
    syncSanity="$regionsData/DB"
    syncFolder=".Regions"
    if [ "$poolType" == "usb" ]; then
      syncExcludes=( --exclude="Archive/ALUTqMiuxVtjfuair7WIgQ/" --exclude="Games/" )
    fi
  fi
  if [ "$shareName" == "Media" ]; then
    syncSource="$tankMedia"
    syncSanity="$tankMedia/Music"
    syncFolder="Media"
  fi
}

## Syncs one share to a pool as one step, its progress logged by RSYNC_FILTER, and prints
## the @STEP_END: ok, warn (rsync's 24, files that vanished during the copy), failed or
## cancelled. Answers 0 for ok and warn.
## Usage: SYNC_SHARE <pool> <share> <step n> <step count>
syncSource=""
syncSanity=""
syncFolder=""
syncExcludes=()
function SYNC_SHARE() {
  local poolName="$1"
  local shareName="$2"
  local stepIndex="$3"
  local stepCount="$4"
  local rsyncCode
  EVENT STEP "$stepIndex" "$stepCount" "$poolName" "$shareName"
  SHARE_SETTINGS "$poolName" "$shareName"
  if [ ! -e "$syncSanity" ]; then
    echo "'$shareName' share not found on tank!"
    EVENT STEP_END "$stepIndex" failed "share not found on tank"
    return 1
  fi
  echo "syncing '$shareName' share to $poolName drive..."
  RUN_WORKER RSYNC_FILTER "$stepIndex" "$stepCount" \
    $rsyncBin $checksumOption "${syncExcludes[@]}" \
    "$syncSource/" "$extBkps/$poolName/$syncFolder/" $rsyncOptions
  rsyncCode=$?
  if CANCEL_REQUESTED; then
    echo "syncing '$shareName' to $poolName cancelled."
    EVENT STEP_END "$stepIndex" cancelled "rsync exit $rsyncCode"
    return 1
  fi
  if [ $rsyncCode -eq 0 ]; then
    EVENT STEP_END "$stepIndex" ok ""
    return 0
  fi
  if [ $rsyncCode -eq 24 ]; then
    echo "WARNING: some '$shareName' files vanished during the copy (rsync exit 24)."
    EVENT STEP_END "$stepIndex" warn "rsync exit 24: files vanished"
    return 0
  fi
  echo "WARNING: syncing '$shareName' to $poolName failed (rsync exit $rsyncCode)."
  EVENT STEP_END "$stepIndex" failed "rsync exit $rsyncCode"
  return 1
}

## Collects one pool's backup date for RECORD_BACKUP_DATES and touches its LastSynced.txt.
## Usage: SAVEBKPDATES hdd4tb-03   adds "hdd4tb-03|2026-11-20_03:12:44_PM"
currentBackupDates=()
function SAVEBKPDATES() {
  local poolName="$1"
  local currentDate
  touch "$extBkps/$poolName/LastSynced.txt"
  currentDate=$(date '+%F %r')
  currentBackupDates+=( "$poolName|${currentDate// /_}" )
}

## Merges the dates SAVEBKPDATES collected into the status file, one "pool|date" line per
## pool (replacing a pool's old date, adding a new pool), keeping the old file as .bak,
## and prints the dates before and after.
function RECORD_BACKUP_DATES() {
  local statusLines=()
  local statusLine
  local statusIndex
  local statusPool
  local backupItem
  local backupPool
  local backupDate
  local entryFound
  EVENT PHASE record
  if [ ! -f "$statusFile" ]; then
    if : > "$statusFile" 2>/dev/null; then
      echo "created new status file '$statusFile'."
    else
      echo "WARNING: cannot create '$statusFile' -- backup dates not recorded!"
      return 1
    fi
  fi
  echo "last backup dates: "
  readarray -t statusLines < "$statusFile"
  for statusLine in "${statusLines[@]}"; do
    echo "$statusLine"
  done
  cp -f "$statusFile" "$statusFile.bak"
  truncate -s 0 "$statusFile"
  for backupItem in "${currentBackupDates[@]}"; do
    ## skip blank or malformed entries (must be POOL|DATE)
    if [[ "$backupItem" != *"|"* ]]; then
      continue
    fi
    entryFound="no"
    backupPool="${backupItem%|*}"
    backupDate="${backupItem#*|}"
    for statusIndex in "${!statusLines[@]}"; do
      statusLine="${statusLines[$statusIndex]}"
      if [[ "$statusLine" != *"|"* ]]; then
        continue
      fi
      statusPool="${statusLine%|*}"
      if [ "$statusPool" == "$backupPool" ]; then
        statusLines[$statusIndex]="$statusPool|$backupDate"
        entryFound="yes"
        break
      fi
    done
    if [ "$entryFound" != "yes" ]; then
      statusLines+=( "$backupPool|$backupDate" )
    fi
  done
  echo " "
  echo "current backup dates: "
  for statusLine in "${statusLines[@]}"; do
    echo "$statusLine" >> "$statusFile"
    echo "$statusLine"
  done
  echo " "
}

## Turns the LEDwall's white LED on or off, the backup's indicator.
## Usage: BACKUP_LED <on|off>
function BACKUP_LED() {
  local ledState="$1"
  curl -s -f "http://ledwall.home/api?action=main&arg=whtled$ledState" > /dev/null 2>&1 &
}

## Backs every attached pool up, one step per share, then records the dates of the pools
## whose every share copied. A pool not mounted where the backup writes, of no known
## type, or without its Data folder is skipped. Stops at a cancel.
## Usage: BACKUP_POOLS   (in a job; checksumOption set for a checksum compare)
function BACKUP_POOLS() {
  local poolName
  local shareName
  local poolShares
  local stepPools=()
  local stepShares=()
  local stepIndex
  local stepCount
  local donePools=""
  local incompletePools=""
  local skippedPools=""
  local cancelled="no"
  local notStarted
  declare -A poolFailed=()
  EVENT PHASE backup
  if ! READ_POOLS verbose; then
    FINISH_JOB failed "drives file not found"
    return
  fi
  ## the plan: one step per share of each attached pool
  for poolName in "${backupPools[@]}"; do
    if ! POOL_MOUNTED "$poolName"; then
      if POOL_IMPORTED "$poolName"; then
        echo "$poolName is imported but not mounted at $extBkps/$poolName, skipping."
        EVENT POOL "$poolName" skipped
        skippedPools+=" $poolName"
      fi
      continue
    fi
    poolShares=$(POOL_SHARES "$poolName")
    if [ -z "$poolShares" ]; then
      echo "unknown drive type for $poolName (its name must start usb or hdd), skipping."
      EVENT POOL "$poolName" skipped
      skippedPools+=" $poolName"
      continue
    fi
    if [ ! -e "$extBkps/$poolName/Data" ]; then
      echo "'Data' folder missing on $poolName, skipping."
      EVENT POOL "$poolName" skipped
      skippedPools+=" $poolName"
      continue
    fi
    for shareName in $poolShares; do
      stepPools+=( "$poolName" )
      stepShares+=( "$shareName" )
    done
  done
  stepCount=${#stepPools[@]}
  if [ $stepCount -eq 0 ]; then
    echo "no backup drives attached."
    FINISH_JOB failed "no backup drives attached"
    return
  fi
  echo "****************** starting backup **********************"
  BACKUP_LED on
  MAKE_JOB_DIR
  for stepIndex in $(seq 1 "$stepCount"); do
    poolName="${stepPools[$(( stepIndex - 1 ))]}"
    shareName="${stepShares[$(( stepIndex - 1 ))]}"
    if ! SYNC_SHARE "$poolName" "$shareName" "$stepIndex" "$stepCount"; then
      poolFailed[$poolName]="yes"
    fi
    if CANCEL_REQUESTED; then
      cancelled="yes"
      poolFailed[$poolName]="yes"
      break
    fi
    ## a pool's last share done: its outcome
    if [ $stepIndex -eq $stepCount ] || [ "${stepPools[$stepIndex]}" != "$poolName" ]; then
      if [ "${poolFailed[$poolName]}" == "yes" ]; then
        echo "$poolName: backup incomplete, its last backup date is kept."
        EVENT POOL "$poolName" incomplete
        incompletePools+=" $poolName"
      else
        SAVEBKPDATES "$poolName"
        EVENT POOL "$poolName" done
        donePools+=" $poolName"
      fi
    fi
    echo " "
  done
  BACKUP_LED off
  ## a cancel: the pool it stopped in is incomplete, the pools after it never started
  if [ "$cancelled" == "yes" ]; then
    EVENT POOL "$poolName" incomplete
    incompletePools+=" $poolName"
    for notStarted in "${stepPools[@]:$stepIndex}"; do
      if [[ "$skippedPools " != *" $notStarted "* ]] && [ "$notStarted" != "$poolName" ]; then
        EVENT POOL "$notStarted" skipped
        skippedPools+=" $notStarted"
      fi
    done
  fi
  RECORD_BACKUP_DATES
  if [ -z "$donePools" ]; then
    backupSummary="backed up: none"
  else
    backupSummary="backed up:$donePools"
  fi
  if [ -n "$incompletePools" ]; then
    backupSummary+="; incomplete:$incompletePools"
  fi
  if [ -n "$skippedPools" ]; then
    backupSummary+="; skipped:$skippedPools"
  fi
  echo "$backupSummary"
  echo "****************** backup complete **********************"
  if [ "$cancelled" == "yes" ]; then
    backupResult="cancelled"
  elif [ -n "$incompletePools" ] || [ -n "$skippedPools" ]; then
    backupResult="failed"
  else
    backupResult="ok"
  fi
}

## Commands ##

REQUIRE_RAMDISK

## Backup Status: the status file only, nothing printed into the running job's log
if [ "$cmd" == "bkp_status" ]; then
  WRITE_STATUS
  exit 0
fi

## Cancel the running backup or VM backup
if [ "$cmd" == "bkp_cancel" ]; then
  runningJob=$(LOCKED_JOB)
  if [ "$runningJob" != "standard" ] && [ "$runningJob" != "checksum" ] && [ "$runningJob" != "vms" ]; then
    EVENT REFUSED cancel "${runningJob:-idle}"
    exit 0
  fi
  touch "$cancelFlag"
  EVENT CANCEL "$(EPOCH_MS)"
  echo "cancel requested, stopping $runningJob..."
  ## only a worker the job itself started, never a PID reused since
  workerPid=$(cat "$workerPidPath" 2>/dev/null)
  jobPid=$(LOCKED_PID)
  if [ -n "$workerPid" ] && [ -n "$jobPid" ]; then
    workerParent=$(ps -o ppid= -p "$workerPid" 2>/dev/null)
    if [ "${workerParent//[[:space:]]/}" == "$jobPid" ]; then
      KILL_TREE "$workerPid"
    fi
  fi
  exit 0
fi

## Attach Backup Drives
if [ "$cmd" == "bkp_attach" ]; then
  trap 'JOB_EXIT' EXIT
  trap 'exit 143' TERM INT
  if ! START_JOB attach; then
    SHRED_PASSWORD
    trap - EXIT
    exit 0
  fi
  if ! ATTACH_POOLS; then
    FINISH_JOB failed "$poolsError"
    END_ROUTINE
  fi
  SHRED_PASSWORD
  if [ $attachedCount -gt 0 ] && [ $failedCount -eq 0 ]; then
    FINISH_JOB ok "$attachedCount drive(s) attached"
  elif [ $attachedCount -gt 0 ]; then
    FINISH_JOB failed "$attachedCount drive(s) attached, $failedCount failed"
  elif [ $failedCount -gt 0 ]; then
    FINISH_JOB failed "no drives attached, $failedCount failed"
  else
    FINISH_JOB failed "no backup drives connected"
  fi
  END_ROUTINE
fi

## Detach Backup Drives
if [ "$cmd" == "bkp_detach" ]; then
  trap 'JOB_EXIT' EXIT
  trap 'exit 143' TERM INT
  if ! START_JOB detach; then
    trap - EXIT
    exit 0
  fi
  if ! DETACH_POOLS; then
    FINISH_JOB failed "$poolsError"
    END_ROUTINE
  fi
  if [ $busyCount -gt 0 ]; then
    FINISH_JOB failed "$detachedCount drive(s) detached, $busyCount still attached (busy)"
  else
    FINISH_JOB ok "$detachedCount drive(s) detached"
  fi
  END_ROUTINE
fi

## External Drive Backups: standard or checksum, optionally ejecting after
if [[ "$cmd" =~ ^bkp_(standard|checksum)(_eject)?$ ]]; then
  backupMode="${BASH_REMATCH[1]}"
  ejectAfter="${BASH_REMATCH[2]}"
  trap 'JOB_EXIT' EXIT
  trap 'exit 143' TERM INT
  if ! START_JOB "$backupMode"; then
    trap - EXIT
    exit 0
  fi
  if [ "$backupMode" == "checksum" ]; then
    echo "checksum compare option selected, this backup will take awhile!"
    checksumOption="--checksum"
  fi
  backupResult=""
  backupSummary=""
  BACKUP_POOLS
  if [ "$doneWritten" == "yes" ]; then
    END_ROUTINE
  fi
  if [ -n "$ejectAfter" ] && [ "$backupResult" != "cancelled" ]; then
    DETACH_POOLS
    backupSummary+="; detached $detachedCount"
    if [ $busyCount -gt 0 ]; then
      backupSummary+=", $busyCount still attached (busy)"
      backupResult="failed"
    fi
  fi
  FINISH_JOB "$backupResult" "$backupSummary"
  END_ROUTINE
fi

## Proxmox Guest Backups (pve-backup.sh)
if [ "$cmd" == "bkp_vms" ]; then
  trap 'JOB_EXIT' EXIT
  trap 'exit 143' TERM INT
  if ! START_JOB vms; then
    trap - EXIT
    exit 0
  fi
  EVENT PHASE backup
  if [ ! -x "$pveBackupScript" ]; then
    echo "'$pveBackupScript' not installed!"
    FINISH_JOB failed "pve-backup.sh not installed"
    END_ROUTINE
  fi
  MAKE_JOB_DIR
  RUN_WORKER VZDUMP_FILTER 0 0 "$pveBackupScript" pve_vmsbkp "$ramDisk"
  vmsCode=$?
  if CANCEL_REQUESTED; then
    FINISH_JOB cancelled "VM backup cancelled"
  elif [ $vmsCode -ne 0 ]; then
    FINISH_JOB failed "VM backup finished with errors (exit $vmsCode)"
  else
    FINISH_JOB ok "every guest backed up"
  fi
  END_ROUTINE
fi

### end ###

echo "unknown command '$cmd'!"
exit 1
