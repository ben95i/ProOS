#!/bin/bash
### Raspberry Pi / Server Communication Script - ProOS
### by Ben Provenzano III
###
### Usage: login <host> [sync | reset | <rpi command>]
###        login <module> init <host>
###        login update-server | clearkeys | cmds | rmtmp

######## Constants ########

## DNS domain appended to host names, e.g. ".home" (empty uses the bare name)
DOMAIN=""
## repo root on mgmt, the SSH keys folder and the scratch folder for server archives
ROOT_DIR="/mnt/Projects/ProOS"
KEYS_DIR="/mnt/ssh/keys"
WORK_DIR="/opt/deploy"
TEMP_ROOT="$WORK_DIR/.ptmp"
## RAM-only folder holding one ssh-agent per terminal session (tmpfs, never on disk)
USER_ID=$(id -u)
if [ -n "$XDG_RUNTIME_DIR" ]; then
  RUNTIME_BASE="$XDG_RUNTIME_DIR"
else
  RUNTIME_BASE="/dev/shm/proos-$USER_ID"
fi
AGENT_ROOT="$RUNTIME_BASE/ssh-agents"
AGENT_LOCK="$AGENT_ROOT/.lock"
## lifetime of the unlocked keys in a session agent
AGENT_LIFE="8h"
## this script's own path, handed to ssh-add as its passphrase prompter
SCRIPT_PATH=$(readlink -f "$0")
## command line arguments: login <module> <command> <init host>
MODULE="$1"
CMD="$2"
INIT_HOST="$3"
## names that are folders in the repo but not hosts
DENIED_NAMES=("logon" "login" "rpi" "wkst" "shared" "router")
## connection settings
SSH_PORT="22"
SSH_TIMEOUT="450ms"
SSH_KEEPALIVE=(-o "ServerAliveInterval=5" -o "ServerAliveCountMax=5")
RSYNC_OPTIONS=(--stats --human-readable --recursive --times --checksum)
RSYNC_EXCLUDES=(--exclude=photos --exclude=sources)
## waits while a Pi reboots into read/write mode, and before a deploy starts
REBOOT_WAIT_SECONDS=25
RETRY_COUNT=4
RETRY_WAIT_SECONDS=10
CANCEL_WAIT_SECONDS=3
## remote commands
SPACER="*********************"
UPDATE_CMDS="apt update; apt upgrade; apt full-upgrade; apt autoremove; apt clean"
SERVER_UPDATE_CMD="apt-get update; apt-get upgrade"
PI_UPDATE_CMD="rpi update"
SVR_INSTALL_CMD="cd /tmp/; tar -xmf config.tar; rm -f config.tar; chmod +x /tmp/config/installer.sh; /tmp/config/installer.sh"
PI_INSTALL_CMD="cd /opt/rpi/config; echo $MODULE > ./hostname; chmod +x ./installer.sh; ./installer.sh"

######## Variables ########

## "pi" or "nonpi", set by PRGM_INIT
hostMode=""
## host name the commands go to, e.g. "pve" or "brpi"
targetHost=""
## "online" or "offline", set by CHECK_HOST_ONLINE
hostStatus="offline"
## this terminal session's agent folder, set by AGENT_START
sessionDir=""
## ssh options selecting the current host's key, set by SSH_IDENTITY
sshIdentity=()
## "1" once AGENT_LOAD_KEYS has run in this invocation
keysLoaded=""
## server deploy work folder, removed on exit by REMOVE_WORK_FOLDER
workFolder=""

######## General Helpers ########

## Exits the script. Unlocked keys stay in the terminal's agent ("login clearkeys" removes them).
EXIT_PRGM(){
  exit
}

## Prints the items joined by a separator. Example: JOIN_BY ", " files pve  ->  files, pve
JOIN_BY(){
  local separator="$1"
  shift
  local joined="$1"
  shift
  local item
  for item in "$@"; do
    joined="$joined$separator$item"
  done
  echo "$joined"
}

## Prints a key's host name from its file. Example: KEY_NAME /keys/pve.rsa  ->  pve
KEY_NAME(){
  local keyFile="$1"
  basename "$keyFile" .rsa
}

## Returns 0 when a module folder is a server (lxc.conf, qemu.conf or pc.conf marker).
## Example: IS_SERVER_MODULE pve  ->  0,  IS_SERVER_MODULE brpi  ->  1
IS_SERVER_MODULE(){
  local moduleName="$1"
  local moduleDir="$ROOT_DIR/$moduleName"
  if [ -e "$moduleDir/qemu.conf" ] || \
     [ -e "$moduleDir/lxc.conf" ] || \
     [ -e "$moduleDir/pc.conf" ]; then
    return 0
  fi
  return 1
}

## Returns 0 when a name is on the DENIED_NAMES list. Example: IS_DENIED_NAME shared  ->  0
IS_DENIED_NAME(){
  local moduleName="$1"
  local deniedName
  for deniedName in "${DENIED_NAMES[@]}"; do
    if [ "$moduleName" == "$deniedName" ]; then
      return 0
    fi
  done
  return 1
}

## Removes the server deploy work folder, if any. Runs on every exit through the EXIT trap.
REMOVE_WORK_FOLDER(){
  if [ -n "$workFolder" ] && [ -d "$workFolder" ]; then
    rm -r "$workFolder"
  fi
  workFolder=""
}

######## Terminal Session SSH Agent ########
## A key has a passphrase when its module folder holds an "ssh.pwd" marker
## ($ROOT_DIR/<module>/ssh.pwd); every marked key shares one passphrase.
## Marked keys are unlocked once per terminal into an ssh-agent whose socket
## is $AGENT_ROOT/<shell pid>/sock. Unmarked keys are used from their file.

## Prints a process's start time, empty when the process is gone. Example: PROC_STAMP 4199  ->  1234567
PROC_STAMP(){
  local processId="$1"
  local statLine
  local fieldsAfterName
  statLine=$(cat "/proc/$processId/stat" 2>/dev/null)
  if [ -z "$statLine" ]; then
    return
  fi
  ## The process name may hold spaces, so the fields are read after its closing ")"
  fieldsAfterName="${statLine##*) }"
  echo "$fieldsAfterName" | awk '{print $20}'
}

## Returns 0 when a PID is a running ssh-agent. Example: IS_SSH_AGENT 4213  ->  0
IS_SSH_AGENT(){
  local processId="$1"
  local processName
  processName=$(cat "/proc/$processId/comm" 2>/dev/null)
  if [ "$processName" == "ssh-agent" ]; then
    return 0
  fi
  return 1
}

## Returns the agent state at $SSH_AUTH_SOCK: 0 holds keys, 1 is empty, 2 is not running.
AGENT_STATUS(){
  ssh-add -l > /dev/null 2>&1
}

## Creates the agent root folder, exiting when it is not private to this user.
## Another run may create it at the same moment, so an existing folder is not an error.
AGENT_ROOT_INIT(){
  local agentDir
  for agentDir in "$RUNTIME_BASE" "$AGENT_ROOT"; do
    if [ ! -d "$agentDir" ]; then
      mkdir -m 700 "$agentDir" 2>/dev/null
    fi
    if [ ! -d "$agentDir" ]; then
      echo "Could not create agent folder $agentDir, exiting!"
      exit 1
    fi
    if [ -L "$agentDir" ] || [ ! -O "$agentDir" ]; then
      echo "Agent folder $agentDir is not owned by this user, exiting!"
      exit 1
    fi
  done
}

## Runs a command holding the agent lock, so terminals never start, reap or stop agents at once.
## Example: WITH_AGENT_LOCK AGENT_REAP
WITH_AGENT_LOCK(){
  local lockFd
  local result
  exec {lockFd}>"$AGENT_LOCK"
  flock "$lockFd"
  "$@"
  result=$?
  exec {lockFd}>&-
  return "$result"
}

## Stops one session agent and removes its folder. Example: AGENT_STOP "$AGENT_ROOT/3955"
AGENT_STOP(){
  local stopDir="$1"
  local agentPid
  if [ -S "$stopDir/sock" ]; then
    SSH_AUTH_SOCK="$stopDir/sock" ssh-add -D > /dev/null 2>&1
  fi
  if [ -r "$stopDir/pid" ]; then
    agentPid=$(cat "$stopDir/pid")
    if IS_SSH_AGENT "$agentPid"; then
      kill "$agentPid" 2>/dev/null
    fi
  fi
  rm -rf "${stopDir:?}"
}

## Stops every terminal session's agent.
AGENT_STOP_ALL(){
  local agentDir
  for agentDir in "$AGENT_ROOT"/*; do
    if [ -d "$agentDir" ]; then
      AGENT_STOP "$agentDir"
    fi
  done
}

## Stops the agents of terminal sessions that have ended, and agents whose folder was removed.
AGENT_REAP(){
  local agentDir
  local shellPid
  local savedStamp
  local currentStamp
  local agentPid
  local agentCommand
  local agentSocket
  for agentDir in "$AGENT_ROOT"/*; do
    if [ ! -d "$agentDir" ]; then
      continue
    fi
    shellPid=$(basename "$agentDir")
    savedStamp=$(cat "$agentDir/shell" 2>/dev/null)
    currentStamp=$(PROC_STAMP "$shellPid")
    if [ -z "$savedStamp" ] || [ "$currentStamp" != "$savedStamp" ]; then
      AGENT_STOP "$agentDir"
    fi
  done
  for agentPid in $(pgrep -u "$USER_ID" -f "ssh-agent -s -a $AGENT_ROOT/"); do
    if ! IS_SSH_AGENT "$agentPid"; then
      continue
    fi
    agentCommand=$(tr '\0' ' ' < "/proc/$agentPid/cmdline")
    agentSocket=$(echo "$agentCommand" | sed 's/.* -a \([^ ]*\).*/\1/')
    if [ ! -S "$agentSocket" ]; then
      kill "$agentPid" 2>/dev/null
    fi
  done
}

## Connects to this terminal's agent (keyed by the calling shell's PID), starting it when needed.
AGENT_START(){
  sessionDir="$AGENT_ROOT/$PPID"
  export SSH_AUTH_SOCK="$sessionDir/sock"
  AGENT_REAP
  AGENT_STATUS
  local agentState=$?
  if [ "$agentState" -ne 2 ]; then
    return
  fi
  rm -rf "${sessionDir:?}"
  mkdir -m 700 "$sessionDir"
  mkdir -m 700 "$sessionDir/pub"
  eval "$(ssh-agent -s -a "$SSH_AUTH_SOCK" -t "$AGENT_LIFE")" > /dev/null
  AGENT_STATUS
  agentState=$?
  if [ "$agentState" -eq 2 ]; then
    echo "Could not start the SSH agent for this terminal, exiting!"
    rm -rf "${sessionDir:?}"
    exit 1
  fi
  echo "$SSH_AGENT_PID" > "$sessionDir/pid"
  PROC_STAMP "$PPID" > "$sessionDir/shell"
}

## Prints a key file's fingerprint without its passphrase, empty for an old PEM key.
## Example: KEY_FINGERPRINT /keys/pve.rsa  ->  SHA256:AvuPdq...
KEY_FINGERPRINT(){
  local keyFile="$1"
  ssh-keygen -lf "$keyFile" 2>/dev/null | awk '{print $2}'
}

## Prints a key file's public key as loaded in the agent, empty when not loaded.
## New-format keys match by fingerprint; old PEM keys match by the file path the agent lists them under.
## Example: AGENT_PUBLIC_KEY /keys/pve.rsa  ->  ssh-ed25519 AAAA... pve
AGENT_PUBLIC_KEY(){
  local keyFile="$1"
  local fileFingerprint
  local agentKeys
  local agentFingerprints
  local keyIndex
  local agentKey
  local agentComment
  AGENT_STATUS
  local agentState=$?
  if [ "$agentState" -ne 0 ]; then
    return
  fi
  fileFingerprint=$(KEY_FINGERPRINT "$keyFile")
  mapfile -t agentKeys < <(ssh-add -L 2>/dev/null)
  mapfile -t agentFingerprints < <(printf '%s\n' "${agentKeys[@]}" | ssh-keygen -lf - | awk '{print $2}')
  for (( keyIndex = 0; keyIndex < ${#agentKeys[@]}; keyIndex++ )); do
    agentKey="${agentKeys[$keyIndex]}"
    agentComment=$(echo "$agentKey" | cut -d ' ' -f 3-)
    if [ -n "$fileFingerprint" ] && [ "${agentFingerprints[$keyIndex]}" == "$fileFingerprint" ]; then
      echo "$agentKey"
      return
    fi
    if [ -z "$fileFingerprint" ] && [ "$agentComment" == "$keyFile" ]; then
      echo "$agentKey"
      return
    fi
  done
}

## Returns 0 when a key's module is marked with ssh.pwd. rpi.rsa counts any Pi module's marker.
## Example: KEY_NEEDS_PASS /keys/pve.rsa  ->  0 when pve/ssh.pwd exists
KEY_NEEDS_PASS(){
  local keyName
  local markerFile
  local markerModule
  keyName=$(KEY_NAME "$1")
  if [ -f "$ROOT_DIR/$keyName/ssh.pwd" ]; then
    return 0
  fi
  if [ "$keyName" != "rpi" ]; then
    return 1
  fi
  for markerFile in "$ROOT_DIR"/*/ssh.pwd; do
    if [ ! -f "$markerFile" ]; then
      continue
    fi
    markerModule=$(basename "$(dirname "$markerFile")")
    if ! IS_SERVER_MODULE "$markerModule"; then
      return 0
    fi
  done
  return 1
}

## Passphrase prompt that ssh-add runs instead of its own (which names the key file).
## ssh-add passes its prompt as $1 and reads the answer from stdout.
ASKPASS(){
  local sshPrompt="$1"
  local shownPrompt
  local passphrase
  if [[ "$sshPrompt" == Bad* ]]; then
    shownPrompt="Bad passphrase, try again: "
  else
    shownPrompt="Enter passphrase: "
  fi
  printf '%s' "$shownPrompt" > /dev/tty
  read -r -s passphrase < /dev/tty
  echo > /dev/tty
  printf '%s\n' "$passphrase"
  unset passphrase
}

## Unlocks every marked key into the agent with one ssh-add call, so one prompt covers them all.
## A key skipped with Enter is not asked for again this session. Prints e.g. "key(s) files, pve unlocked."
AGENT_LOAD_KEYS(){
  local keyFile
  local missingKeys=()
  local unlockedNames=()
  local publicKey
  local keyName
  local unlockedList
  if [ "$keysLoaded" == "1" ]; then
    return
  fi
  keysLoaded="1"
  for keyFile in "$KEYS_DIR"/*.rsa; do
    if ! KEY_NEEDS_PASS "$keyFile"; then
      continue
    fi
    if grep -qxF "$keyFile" "$sessionDir/skipped" 2>/dev/null; then
      continue
    fi
    publicKey=$(AGENT_PUBLIC_KEY "$keyFile")
    if [ -z "$publicKey" ]; then
      missingKeys+=("$keyFile")
    fi
  done
  if [ ${#missingKeys[@]} -eq 0 ]; then
    return
  fi
  echo "Unlocking ${#missingKeys[@]} SSH key(s) for this terminal session (Enter skips a key)..."
  PROOS_ASKPASS=1 SSH_ASKPASS="$SCRIPT_PATH" SSH_ASKPASS_REQUIRE=force ssh-add "${missingKeys[@]}" 2>&1 | grep -v "^Identity added"
  for keyFile in "${missingKeys[@]}"; do
    publicKey=$(AGENT_PUBLIC_KEY "$keyFile")
    if [ -z "$publicKey" ]; then
      echo "$keyFile" >> "$sessionDir/skipped"
    else
      keyName=$(KEY_NAME "$keyFile")
      unlockedNames+=("$keyName")
    fi
  done
  if [ ${#unlockedNames[@]} -eq 0 ]; then
    echo "No keys unlocked."
    return
  fi
  unlockedList=$(JOIN_BY ", " "${unlockedNames[@]}")
  echo "key(s) $unlockedList unlocked."
}

## Sets sshIdentity to the options selecting one host's key. A marked key is taken from the agent
## (unlocked on first use) as its public key, so ssh offers no other loaded key; any other key is
## given as its file. Example: SSH_IDENTITY /keys/pve.rsa  ->  (-o IdentitiesOnly=yes -i <pve.pub>)
SSH_IDENTITY(){
  local keyFile="$1"
  local keyName
  local publicKey
  local pubFile
  local pubTemp
  sshIdentity=(-o "IdentitiesOnly=yes")
  if [ ! -f "$keyFile" ]; then
    return
  fi
  if ! KEY_NEEDS_PASS "$keyFile"; then
    sshIdentity+=(-i "$keyFile")
    return
  fi
  AGENT_ROOT_INIT
  WITH_AGENT_LOCK AGENT_START
  AGENT_LOAD_KEYS
  publicKey=$(AGENT_PUBLIC_KEY "$keyFile")
  if [ -z "$publicKey" ]; then
    sshIdentity+=(-i "$keyFile")
    return
  fi
  ## Written to a temp file and moved into place, so another run never reads a partial file
  keyName=$(KEY_NAME "$keyFile")
  pubFile="$sessionDir/pub/$keyName.pub"
  pubTemp=$(mktemp "$sessionDir/pub/.$keyName.XXXXXX")
  echo "$publicKey" > "$pubTemp"
  mv -f "$pubTemp" "$pubFile"
  sshIdentity+=(-i "$pubFile")
}

## Stops this terminal's agent and forgets its keys ("login clearkeys", also the profile's exit trap).
AGENT_CLEAR_SESSION(){
  local clearDir="$AGENT_ROOT/$PPID"
  if [ -d "$clearDir" ]; then
    AGENT_STOP "$clearDir"
    echo "SSH keys cleared from this terminal session."
  else
    echo "No SSH keys cached for this terminal session."
  fi
  AGENT_REAP
}

AGENT_CLEAR(){
  AGENT_ROOT_INIT
  WITH_AGENT_LOCK AGENT_CLEAR_SESSION
}

######## Remote Commands ########

## Sets hostStatus by testing the SSH port of targetHost. Example: CHECK_HOST_ONLINE "yes" exits when down.
CHECK_HOST_ONLINE(){
  local exitWhenDown="$1"
  echo "Testing connection..."
  if nc -zv -w "$SSH_TIMEOUT" "$targetHost" "$SSH_PORT" &> /dev/null; then
    echo "$targetHost's SSH port open, connecting..."
    hostStatus="online"
    return
  fi
  hostStatus="offline"
  if [ "$exitWhenDown" == "yes" ]; then
    echo "$targetHost's SSH port is unreachable, exiting..."
    EXIT_PRGM
  fi
  echo "$targetHost's SSH port is unreachable, skipping."
}

## Runs ssh to targetHost as a user, with an optional command. Example: SSH_AS root "df -h"
SSH_AS(){
  local remoteUser="$1"
  shift
  ssh -t "${sshIdentity[@]}" "${SSH_KEEPALIVE[@]}" "$remoteUser@$targetHost" "$@"
}

## Runs a command on targetHost as root. Example: REMOTE_RUN "rpi temp"
REMOTE_RUN(){
  local remoteCommand="$1"
  SSH_AS root "$remoteCommand"
}

## Copies a local folder to targetHost. Example: RSYNC_TO_HOST "$ROOT_DIR/rpi" /opt/ --exclude=photos
RSYNC_TO_HOST(){
  local sourcePath="$1"
  local destinationPath="$2"
  shift 2
  local sshCommand="ssh ${sshIdentity[*]} ${SSH_KEEPALIVE[*]}"
  rsync -e "$sshCommand" "${RSYNC_OPTIONS[@]}" "$@" "$sourcePath" "root@$targetHost:$destinationPath"
}

## Returns 0 when targetHost answers one ping.
HOST_PINGS(){
  ping -c 1 "$targetHost" &> /dev/null
}

## Waits for targetHost to come back after a reboot, exiting when it stays down.
WAIT_FOR_HOST(){
  local attempt
  echo "Waiting $REBOOT_WAIT_SECONDS seconds..."
  sleep "$REBOOT_WAIT_SECONDS"
  if HOST_PINGS; then
    echo "Connection established."
    return
  fi
  for (( attempt = 1; attempt <= RETRY_COUNT; attempt++ )); do
    if HOST_PINGS; then
      break
    fi
    echo "Host is down, waiting $RETRY_WAIT_SECONDS more seconds..."
    sleep "$RETRY_WAIT_SECONDS"
  done
  if HOST_PINGS; then
    echo "Connection established."
    return
  fi
  echo "Host is down, exiting..."
  EXIT_PRGM
}

## Counts down CANCEL_WAIT_SECONDS so a deploy can be cancelled with Ctrl+C.
CANCEL_COUNTDOWN(){
  local secondsLeft="$CANCEL_WAIT_SECONDS"
  while [ "$secondsLeft" -gt 0 ]; do
    printf "\rYou have %2d second(s), to hit Ctrl+C to cancel." "$secondsLeft"
    sleep 1
    secondsLeft=$(( secondsLeft - 1 ))
  done
  echo ""
}

## Reboots targetHost.
REBOOT_HOST(){
  echo "Rebooting $targetHost..."
  REMOTE_RUN "reboot"
}

## Opens a shell on targetHost, exiting when it is down.
SSH_LOGIN(){
  CHECK_HOST_ONLINE "yes"
  SSH_AS "root"
}

## Runs the apt upgrades on every server folder (one with an lxc.conf or qemu.conf), one at a time.
UPDATE_ALL_SVRS(){
  local serverNames
  local serverName
  mapfile -t serverNames < <(find "$ROOT_DIR" -type f \( -name "lxc.conf" -o -name "qemu.conf" \) -printf '%P\n' | sed 's#/[^/]*$##' | sort -u)
  for serverName in "${serverNames[@]}"; do
    echo " "
    echo "$SPACER Connecting to $serverName $SPACER"
    targetHost="$serverName$DOMAIN"
    CHECK_HOST_ONLINE "no"
    if [ "$hostStatus" == "online" ]; then
      SSH_IDENTITY "$KEYS_DIR/$serverName.rsa"
      echo "$SPACER Running APT on $serverName $SPACER"
      REMOTE_RUN "$UPDATE_CMDS"
    fi
  done
  EXIT_PRGM
}

######## Deployment ########

## Deploys the module to a Pi: switches it to read/write if needed, rsyncs base, shared and module, runs the installer.
DEPLOY_PI(){
  local diskReport
  CHECK_HOST_ONLINE "yes"
  echo ""
  diskReport=$(REMOTE_RUN "df -h")
  if [[ "$diskReport" == *"overlay"* ]]; then
    echo "Read/only root filesystem detected."
    REMOTE_RUN "rpi rw"
    WAIT_FOR_HOST
  else
    echo "Read/write root filesystem detected."
  fi
  CANCEL_COUNTDOWN
  if [ "$CMD" == "reset" ] || [ "$CMD" == "init" ]; then
    REMOTE_RUN "rm -f /etc/rpi-conf.done"
  fi
  echo "Downloading base software..."
  RSYNC_TO_HOST "$ROOT_DIR/rpi" "/opt/" "${RSYNC_EXCLUDES[@]}"
  echo "Downloading shared software..."
  RSYNC_TO_HOST "$ROOT_DIR/shared/html/" "/opt/rpi/config/html/" --mkpath
  RSYNC_TO_HOST "$ROOT_DIR/shared/nodeapi/" "/opt/nodeapi/" --mkpath
  echo "Downloading module-specific software..."
  RSYNC_TO_HOST "$ROOT_DIR/$MODULE/" "/opt/rpi/" "${RSYNC_EXCLUDES[@]}"
  echo "Starting installer..."
  REMOTE_RUN "$PI_INSTALL_CMD"
}

## Deploys a server: tars its config folder (plus the Automate site for automate), uploads it and runs installer.sh.
DEPLOY_SERVER(){
  if [ ! -e "$WORK_DIR" ]; then
    echo "Scratch path not found, exiting!"
    EXIT_PRGM
  fi
  mkdir -p "$TEMP_ROOT"
  workFolder=$(mktemp -d "$TEMP_ROOT/XXXXXXXXX")
  echo "Copying files to work folder $workFolder..."
  cp -rf "$ROOT_DIR/$MODULE/config" "$workFolder/"
  if [ "$MODULE" == "automate" ]; then
    echo "Copying shared software..."
    cp -rvf "$ROOT_DIR/shared/html" "$workFolder/config/"
    cp -rvf "$ROOT_DIR/shared/nodeapi" "$workFolder/config/"
  fi
  echo "Compressing files..."
  COPYFILE_DISABLE=true tar -C "$workFolder" -cf "$workFolder/config.tar" config
  echo "Uploading files..."
  scp -p "${sshIdentity[@]}" "$workFolder/config.tar" "root@$targetHost:/tmp/"
  echo "Running $MODULE deployment script..."
  REMOTE_RUN "$SVR_INSTALL_CMD"
  REMOVE_WORK_FOLDER
}

## Shows the after-deploy menu and runs the chosen action, then exits. Returns 0 only for 'd' (re-deploy).
POST_DEPLOY_MENU(){
  echo "'d' to re-deploy"
  echo "'u' to update packages"
  if [ "$hostMode" == "pi" ]; then
    echo "'r' to reboot in read/only mode"
    echo "'x' to reboot pi"
  else
    echo "'r' to reboot system"
  fi
  echo "'s' for a shell on $targetHost"
  echo "press (any) other key to exit"
  read -p "enter option: " -n 1 -r
  echo
  case "$REPLY" in
    [Dd])
      return 0
      ;;
    [Ss])
      SSH_LOGIN
      ;;
    [Rr])
      if [ "$hostMode" == "pi" ]; then
        REMOTE_RUN "rpi ro"
      else
        REBOOT_HOST
      fi
      ;;
    [Xx])
      if [ "$hostMode" == "pi" ]; then
        REBOOT_HOST
      fi
      ;;
    [Uu])
      echo "Updating $targetHost..."
      if [ "$hostMode" == "nonpi" ]; then
        REMOTE_RUN "$SERVER_UPDATE_CMD"
      else
        REMOTE_RUN "$PI_UPDATE_CMD"
      fi
      ;;
  esac
  EXIT_PRGM
}

## Deploys targetHost, then repeats for as long as the menu asks for a re-deploy.
DEPLOY_HOST(){
  while true; do
    if [ "$hostMode" == "nonpi" ]; then
      DEPLOY_SERVER
    else
      DEPLOY_PI
    fi
    POST_DEPLOY_MENU
    echo "Deploying $targetHost..."
  done
}

######## Command Line ########

## Prints the usage menu.
SHOW_HELP(){
printf \
'* Pi / Server Configuration and Login Script
by Ben Provenzano III

Logon to ProOS Pi / Server
login "Hostname"

Update Server (All LXCs/VMs)
login update-server

Sync ProOS (quick run config script) Pi / Server
login "Hostname" sync

Reset ProOS (full config script) Pi Only
login "Hostname" reset

Initialize ProOS (configure a base Pi or reconfigure one) Pi Only
login "Module" init "Hostname"

Forget the SSH keys unlocked in this terminal
login clearkeys
(a key is unlocked once per terminal when its module folder holds "ssh.pwd")

Command Reference List
login cmds

Clean-up Temporary Files
login rmtmp
\n'
}

## Prints the Linux command cheat sheet.
SHOW_COMMAND_REFERENCE(){
printf \
'* Linux Command Reference
by Ben Provenzano III

#### Find a specific string in multiple files
grep -RHIni "\<STRING\>" file or folder

#### Convert binary to single line of base64 (PNG to HTML base64)
openssl base64 -A -in file.bin -out file.base64

#### Convert ZFS stored VM disk to RAW disk image
dd bs=128k if=/dev/rpool/proxmox/vm-100-disk-0 of=file.raw

#### Convert ZFS stored VM disk to QCOW2 disk image
qemu-img convert -f raw -O qcow2 /dev/rpool/proxmox/vm-100-disk-0 file.qcow2
\n'
}

## Stops every session agent and deletes the deploy temp files ("login rmtmp").
REMOVE_TEMP_FILES(){
  echo "deleting temporary files..."
  AGENT_ROOT_INIT
  WITH_AGENT_LOCK AGENT_STOP_ALL
  WITH_AGENT_LOCK AGENT_REAP
  if [ -e "$WORK_DIR" ]; then
    rm -rfv "$TEMP_ROOT"
  else
    rm -rfv /tmp/protmp.*
  fi
}

## Handles the commands that are not host names, exiting after each one.
HANDLE_STANDALONE_COMMANDS(){
  if [ -z "$MODULE" ]; then
    SHOW_HELP
    exit
  fi
  if IS_DENIED_NAME "$MODULE"; then
    echo "Hostname not allowed."
    exit
  fi
  case "$MODULE" in
    cmds)
      SHOW_COMMAND_REFERENCE
      exit
      ;;
    clearkeys)
      AGENT_CLEAR
      exit
      ;;
    update-server)
      UPDATE_ALL_SVRS
      ;;
    rmtmp)
      REMOVE_TEMP_FILES
      exit
      ;;
  esac
}

## Picks the host and key for MODULE, then deploys, runs a Pi command or opens a shell.
PRGM_INIT(){
  HANDLE_STANDALONE_COMMANDS
  if IS_SERVER_MODULE "$MODULE"; then
    hostMode="nonpi"
  else
    hostMode="pi"
  fi
  if [ "$hostMode" == "nonpi" ]; then
    SSH_IDENTITY "$KEYS_DIR/$MODULE.rsa"
    targetHost="$MODULE$DOMAIN"
    if [ "$CMD" == "sync" ]; then
      DEPLOY_HOST
    fi
  else
    SSH_IDENTITY "$KEYS_DIR/rpi.rsa"
    if [ "$CMD" == "init" ]; then
      targetHost="$INIT_HOST$DOMAIN"
      echo "Initializing $targetHost with the $MODULE module..."
      DEPLOY_HOST
    fi
    targetHost="$MODULE$DOMAIN"
    if [ "$CMD" == "reset" ] || [ "$CMD" == "sync" ]; then
      echo "Starting $targetHost deployment..."
      DEPLOY_HOST
    fi
    if [ -n "$CMD" ]; then
      REMOTE_RUN "rpi $CMD"
      EXIT_PRGM
    fi
  fi
  SSH_LOGIN
  EXIT_PRGM
}

## MAIN ENTRY POINT ##
## ssh-add runs this script as its passphrase prompter with PROOS_ASKPASS=1
if [ "$PROOS_ASKPASS" == "1" ]; then
  ASKPASS "$1"
  exit
fi
trap REMOVE_WORK_FOLDER EXIT
PRGM_INIT
