#!/bin/bash
## Share watchdog: detaches the files server's shares while it is down
## Installed as /opt/scripts/share-watchdog.sh, run by share-watchdog.service.
## A CIFS or sshfs mount whose server is gone blocks every program that touches
## it (Dolphin, Plasma's Places and disk widgets) for seconds per file access.
## Once the server misses failLimit probes in a row, its mounts of that type are
## force-detached so access fails at once; autofs mounts them again on the next
## access once the server answers.

serverHost="files.home"
probeInterval=3
probeTimeout=2
failLimit=2

## Port probed for each share type (fstype as /proc/mounts shows it)
declare -A sharePorts=( [cifs]=445 [fuse.sshfs]=22 )
declare -A failCounts=( [cifs]=0 [fuse.sshfs]=0 )

## Exits 0 when $serverHost accepts a TCP connection on port $1 within $probeTimeout s
## e.g. PROBE_PORT 445
PROBE_PORT() {
    local probePort="$1"
    timeout "$probeTimeout" bash -c "exec 3<>/dev/tcp/$serverHost/$probePort" 2>/dev/null
}

## Force-detaches every mount of type $1 served by $serverHost. Works from
## /proc/mounts and skips path lookups, so a dead mount cannot block it
## e.g. DETACH_SHARES cifs
DETACH_SHARES() {
    local shareType="$1"
    local mountLines mountLine mountSource mountPoint mountType mountRest
    mapfile -t mountLines < /proc/mounts
    for mountLine in "${mountLines[@]}"; do
        read -r mountSource mountPoint mountType mountRest <<< "$mountLine"
        if [[ "$mountType" != "$shareType" ]]; then
            continue
        fi
        if [[ "$mountSource" != *"$serverHost"* ]]; then
            continue
        fi
        echo "Detaching $mountPoint ($mountSource)"
        umount --no-canonicalize --force --lazy "$mountPoint"
    done
}

## Probes the port of share type $1 and detaches its mounts after failLimit misses in a row
## e.g. CHECK_SHARE_TYPE fuse.sshfs
CHECK_SHARE_TYPE() {
    local shareType="$1"
    local probePort="${sharePorts[$shareType]}"
    local failCount="${failCounts[$shareType]}"
    if PROBE_PORT "$probePort"; then
        if (( failCount >= failLimit )); then
            echo "$serverHost answers on port $probePort again, $shareType shares mount on next access"
        fi
        failCounts[$shareType]=0
        return
    fi
    failCount=$(( failCount + 1 ))
    failCounts[$shareType]="$failCount"
    if (( failCount == failLimit )); then
        echo "$serverHost is not answering on port $probePort, detaching its $shareType shares"
    fi
    if (( failCount >= failLimit )); then
        DETACH_SHARES "$shareType"
    fi
}

## Main loop
while true; do
    for shareType in "${!sharePorts[@]}"; do
        CHECK_SHARE_TYPE "$shareType"
    done
    sleep "$probeInterval"
done
