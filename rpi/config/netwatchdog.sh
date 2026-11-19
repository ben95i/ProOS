#!/bin/bash
## ProOS for RPi, Wi-Fi Client Watchdog by Ben Provenzano III
#### Installed as /etc/netwatchdog.sh, run by rpi-netwatchdog.service
#### Asks /etc/netmode.sh for the network mode and to rebuild the Wi-Fi

## Network mode script whose commands the watchdog runs (see NETMODE)
NETMODE_SCRIPT="/etc/netmode.sh"

## Tunables for WATCHDOG (rpi-netwatchdog.service)
wdInterval=30            ## seconds between checks while the LAN is healthy
wdFailInterval=15        ## seconds between re-checks after a failed check
wdFailLimit=2            ## failed checks in a row before recovery starts
wdSettle=45              ## seconds to wait for the LAN after each recovery step
wdFwRetries=3            ## extra firmware/network restarts after the first one
wdBootGrace=120          ## seconds after boot before monitoring starts
wdExtraTarget=""         ## optional 2nd always-on LAN IP to ping (e.g. a NAS).
                         ## Not the gateway - that is already pinged.
wdSkipOnEth=1            ## 1 = stay idle while eth0 is connected
wdApCheck=1              ## 1 = check the access point is visible before
                         ## recovering. Set 0 for a hidden network (its name
                         ## never shows in scans).
wdApScanInterval=15      ## seconds between rescans while the access point is down
wdApLogEvery=300         ## seconds between "still waiting" log lines
wdApReturnGrace=180      ## seconds after the access point reappears during
                         ## which the watchdog only retries connecting (it
                         ## may still be starting up) before any recovery
wdDeafWait=600           ## seconds with NO networks visible at all before
                         ## recovery starts (the radio may have gone deaf).
                         ## Raise this if no other networks are in range.
wdClientLimit=150        ## seconds allowed to rebuild the Wi-Fi connection
                         ## ('nmcli con up' alone may wait 90s)
wdDriverLimit=60         ## seconds allowed to unload or load the Wi-Fi driver
wdNmLimit=150            ## seconds allowed to restart NetworkManager
                         ## (its own stop timeout is 90s)

##############################################
## CLIENT-MODE WI-FI WATCHDOG (run by rpi-netwatchdog.service)
##
## Operation:
##  0. Startup  - Wait until the Pi has been up wdBootGrace seconds so the
##                normal 'boot' networking can finish. If the Pi has no
##                wlan0 the watchdog exits cleanly (service stays stopped).
##                The Wi-Fi driver module (e.g. brcmfmac) is noted here so
##                it can be reloaded later even if wlan0 disappears.
##  1. Monitor  - Every wdInterval seconds test local network access:
##                  wlan0 exists -> link is up -> has an IPv4 address ->
##                  has a default gateway -> gateway answers ping
##                  (or wdExtraTarget answers, if set).
##                The watchdog only acts in Wi-Fi CLIENT mode. It idles while:
##                  the hotspot is active or /boot/firmware/apd.enable exists,
##                  /boot/firmware/disable.wifi exists, there is no
##                  /boot/firmware/wpa.conf, or eth0 is connected
##                  (wdSkipOnEth=1). When it returns to client mode it
##                  waits wdSettle seconds before checking again.
##  2. Access   - On any failed check, scan for our network name (SSID in
##     point      wpa.conf):
##     check      a) Not visible, but other networks are: the access point
##                   is down (reboot, firmware upgrade, power cut) and the
##                   Pi is fine. Nothing is torn down. Every
##                   wdApScanInterval seconds the watchdog rescans and
##                   makes sure NetworkManager can reconnect on its own
##                   (radio on, profile present, autoconnect on). It waits
##                   for as long as the access point is down.
##                b) No networks visible at all: same waiting, but after
##                   wdDeafWait seconds recovery starts (step 4), since
##                   the radio may have stopped receiving.
##                c) Visible again after an outage: NetworkManager is told
##                   to connect right away. For wdApReturnGrace seconds
##                   the watchdog only keeps retrying the connection (the
##                   access point may still be starting up); recovery is
##                   only considered after that.
##                If wlan0 is missing or NetworkManager is not running the
##                scan is skipped - those are faults on the Pi itself.
##  3. Confirm  - Access point visible but LAN down: re-test every
##                wdFailInterval seconds; recovery starts after
##                wdFailLimit failures in a row.
##  4. Stage 1  - Restart the connection and rejoin: run 'netmode.sh client'
##                (resets firewall, rebuilds the RPiWiFi profile from
##                wpa.conf, reconnects). Wait up to wdSettle seconds.
##  5. Stage 2  - Restart Wi-Fi firmware and network: radio off, unload the
##                Wi-Fi driver module (and modules holding it), reload it
##                (this re-downloads the chip firmware), restart
##                NetworkManager, run 'netmode.sh client'. Wait wdSettle.
##  6. Stage 3  - Repeat Stage 2 up to wdFwRetries (3) more times.
##  7. Stage 4  - Reboot the Pi. No state is kept across reboots, so the
##                watchdog starts fresh at step 0 after every boot (if the
##                network is still down it will go through all stages again).
##  - Every recovery command has a time limit (wdClientLimit,
##    wdDriverLimit, wdNmLimit). One still running past its limit is
##    stuck in the kernel (e.g. unloading the driver of a crashed chip),
##    which only a reboot clears: recovery stops and the Pi reboots at once.
##  - Before each recovery stage and before rebooting, the network is
##    scanned again. If the access point has disappeared, recovery stops
##    and the watchdog goes back to waiting (step 2a).
##  - Read-only filesystem: nothing is written to persistent storage.
##    Failure counters and outage state live in memory only.
##  - Any recovery stage is cancelled if the mode changes (e.g. 'apd').
##  - Whenever the LAN check passes, all failure counters reset.
##  - Logs: journalctl -u rpi-netwatchdog -f
##############################################

## Watchdog state (in memory only)
wdFails=0                ## failed LAN checks in a row
wdIdle=0                 ## 1 while not in Wi-Fi client mode
wdSleep=0                ## seconds until the next check (set by WD_STEP)
wdApDown=0               ## 1 while waiting for the access point to come back
wdOutageSince=0          ## epoch time the current access point outage began
wdDeafSince=0            ## epoch time since no networks at all were visible
wdApLastLog=0            ## epoch time of the last "still waiting" log line
wdApBackSince=0          ## epoch time the access point reappeared
wdNetworkName=""         ## our network name, read from wpa.conf by WD_SCAN
wdStuck=0                ## 1 once a recovery command overran its time limit

WD_LOG() {
echo "[watchdog] $*"
}

WD_LIMIT() {
## Run a recovery command with a time limit, so a command stuck in the
## kernel (e.g. 'modprobe -r brcmfmac' on a crashed Wi-Fi chip) cannot
## stall the watchdog for good.
## $1 = seconds allowed, $2 = what the command does (for the log),
## the rest = the command or function to run (in a subshell, so it
## cannot change the watchdog's variables).
## Returns the command's exit code. On timeout it logs, sets wdStuck=1 and
## returns 124; the main loop then reboots, since a process blocked in the
## kernel ignores signals and only a reboot clears it.
## Example: WD_LIMIT 60 "unloading Wi-Fi driver" WD_UNLOAD brcmfmac
local limitSeconds="$1" description="$2" commandPid waitedSeconds=0
shift 2
"$@" &
commandPid=$!
while kill -0 "$commandPid" 2>/dev/null; do
  if [ "$waitedSeconds" -ge "$limitSeconds" ]; then
    WD_LOG "$description still running after ${limitSeconds}s - stuck"
    kill -9 "$commandPid" 2>/dev/null
    wdStuck=1
    return 124
  fi
  sleep 1
  waitedSeconds=$((waitedSeconds + 1))
done
wait "$commandPid"
}

NETMODE() {
## Run one netmode.sh command and return its exit code.
## Status commands: 'inapd' / 'inclient' (exit 0 = that mode is active),
## 'ssid' (prints the client network name). Mode command: 'client'.
## Example: NETMODE inapd && echo "hotspot active"
bash "$NETMODE_SCRIPT" "$@"
}

WD_CLIENT_REBUILD() {
## Rebuild the Wi-Fi client connection with 'netmode.sh client' (resets the
## firewall, rebuilds RPiWiFi from wpa.conf, reconnects), within
## wdClientLimit. netmode.sh is run as its own process, so a timeout
## kills it directly. Returns its exit code, or 124 when stuck.
WD_LIMIT "$wdClientLimit" "rebuilding the Wi-Fi connection" bash "$NETMODE_SCRIPT" client
}

WD_ELIGIBLE() {
## True only when the Pi should be a Wi-Fi client
[ -e "/boot/firmware/disable.wifi" ] && return 1
[ -e "/boot/firmware/apd.enable" ] && return 1
[ -e "/boot/firmware/wpa.conf" ] || return 1
NETMODE inapd && return 1
if [ "$wdSkipOnEth" = "1" ] && \
   nmcli -t -f DEVICE,STATE device status 2>/dev/null | grep -q '^eth0:connected'; then
  return 1
fi
return 0
}

WD_LAN_OK() {
## Local network health test, sets wdReason on failure
local gateway
if [ ! -e "/sys/class/net/wlan0" ]; then
  wdReason="wlan0 missing"; return 1
fi
if [ "$(cat /sys/class/net/wlan0/operstate 2>/dev/null)" != "up" ]; then
  wdReason="wlan0 link down"; return 1
fi
if ! ip -4 addr show dev wlan0 2>/dev/null | grep -q 'inet '; then
  wdReason="no IPv4 address on wlan0"; return 1
fi
gateway=$(ip -4 route show default dev wlan0 2>/dev/null | awk '/^default/ { print $3; exit }')
if [ -z "$gateway" ]; then
  wdReason="no default gateway on wlan0"; return 1
fi
ping -I wlan0 -c3 -W2 -q "$gateway" > /dev/null 2>&1 && return 0
## Second target only if it is a different device from the gateway
if [ -n "$wdExtraTarget" ] && [ "$wdExtraTarget" != "$gateway" ]; then
  ping -I wlan0 -c3 -W2 -q "$wdExtraTarget" > /dev/null 2>&1 && return 0
fi
wdReason="gateway $gateway not answering"
return 1
}

WD_SCAN() {
## Scan for Wi-Fi networks. Sets wdScanTotal (networks seen) and
## wdScanFound (1 = our network is visible, or the check is skipped).
local scanList escapedName
wdScanTotal=0
wdScanFound=1
[ "$wdApCheck" = "1" ] || return 0
## Faults on the Pi itself - not an access point outage
[ -e "/sys/class/net/wlan0" ] || return 0
systemctl is-active --quiet NetworkManager || return 0
wdNetworkName=$(NETMODE ssid) || return 0
## Fresh scan. NetworkManager refuses a rescan while it is busy
## connecting, so fall back to the latest scan results.
scanList=$(timeout 30 nmcli -t -f SSID dev wifi list ifname wlan0 --rescan yes 2>/dev/null) \
  || scanList=$(timeout 10 nmcli -t -f SSID dev wifi list ifname wlan0 --rescan no 2>/dev/null)
wdScanTotal=$(printf '%s\n' "$scanList" | grep -c .)
## nmcli terse output escapes '\' and ':' in network names
escapedName=${wdNetworkName//\\/\\\\}
escapedName=${escapedName//:/\\:}
printf '%s\n' "$scanList" | grep -qxF -- "$escapedName" || wdScanFound=0
}

WD_AP_RESET() {
## Clear the access point outage state
wdApDown=0
wdOutageSince=0
wdDeafSince=0
wdApBackSince=0
}

WD_AP_MARK_DOWN() {
## Enter (or stay in) the "waiting for the access point" state, log it,
## and rescan sooner
local now
now=$(date +%s)
wdSleep=$wdApScanInterval
wdApBackSince=0
if [ "$wdApDown" = "0" ]; then
  wdApDown=1
  wdApLastLog=$now
  [ "$wdOutageSince" = "0" ] && wdOutageSince=$now
  if [ "$wdScanTotal" -gt 0 ]; then
    WD_LOG "access point '$wdNetworkName' not visible ($wdScanTotal other networks seen) - waiting for it, nothing to repair"
  else
    WD_LOG "no Wi-Fi networks visible - waiting, recovery starts if this lasts ${wdDeafWait}s"
  fi
elif [ $((now - wdApLastLog)) -ge "$wdApLogEvery" ]; then
  wdApLastLog=$now
  WD_LOG "still waiting for access point '$wdNetworkName' ($((now - wdOutageSince))s, $wdScanTotal other networks seen)"
fi
## Track how long nothing at all has been visible
if [ "$wdScanTotal" -gt 0 ]; then
  wdDeafSince=0
elif [ "$wdDeafSince" = "0" ]; then
  wdDeafSince=$now
fi
}

WD_NUDGE() {
## Keep NetworkManager ready to reconnect on its own - nothing torn down
if [ "$(nmcli radio wifi 2>/dev/null)" != "enabled" ]; then
  WD_LOG "Wi-Fi radio was off, turning it on"
  rfkill unblock wifi
  nmcli radio wifi on
fi
if ! nmcli -t -f NAME con show 2>/dev/null | grep -qx 'RPiWiFi'; then
  WD_LOG "RPiWiFi profile missing, rebuilding it"
  WD_CLIENT_REBUILD
  return
fi
if [ "$(nmcli -g connection.autoconnect con show RPiWiFi 2>/dev/null)" != "yes" ]; then
  WD_LOG "autoconnect was off on RPiWiFi, turning it on"
  nmcli con modify RPiWiFi connection.autoconnect yes
fi
}

WD_RECONNECT() {
## Access point is back: connect now instead of waiting for
## NetworkManager's next background scan. A manual activation also
## clears any autoconnect block left by failed attempts during the outage.
NETMODE inclient && return 0
if nmcli --wait "$wdSettle" con up RPiWiFi > /dev/null 2>&1; then
  WD_LOG "reconnected to access point '$wdNetworkName'"
else
  WD_LOG "access point visible but not accepting connections yet, NetworkManager will keep trying"
fi
}

WD_WAIT_LAN() {
## Poll the LAN for up to wdSettle seconds after a recovery step
local waitedSeconds=0
while [ "$waitedSeconds" -lt "$wdSettle" ]; do
  WD_LAN_OK && return 0
  sleep 5
  waitedSeconds=$((waitedSeconds+5))
done
WD_LAN_OK
}

WD_UNLOAD() {
## Unload a module after first unloading any modules holding it
## (e.g. brcmfmac_wcc holds brcmfmac on current Pi kernels)
local moduleName="$1" holderPath
[ -d "/sys/module/$moduleName" ] || return 0
for holderPath in /sys/module/"$moduleName"/holders/*; do
  [ -e "$holderPath" ] && WD_UNLOAD "$(basename "$holderPath")"
done
modprobe -r "$moduleName"
}

WD_FW_RESTART() {
## Reload Wi-Fi driver/firmware, restart NetworkManager, rejoin.
## Stops at the first command that overruns its time limit (wdStuck=1).
local waitCount
nmcli radio wifi off 2>/dev/null
if [ -n "$wdModule" ]; then
  WD_LOG "unloading Wi-Fi driver '$wdModule'"
  if ! WD_LIMIT "$wdDriverLimit" "unloading Wi-Fi driver '$wdModule'" WD_UNLOAD "$wdModule"; then
    [ "$wdStuck" = "1" ] && return
    WD_LOG "warning: driver unload failed, continuing"
  fi
  sleep 3
  WD_LOG "reloading Wi-Fi driver '$wdModule'"
  if ! WD_LIMIT "$wdDriverLimit" "reloading Wi-Fi driver '$wdModule'" modprobe "$wdModule"; then
    [ "$wdStuck" = "1" ] && return
    WD_LOG "warning: driver reload failed"
  fi
else
  WD_LOG "warning: Wi-Fi driver module unknown, skipping firmware reload"
fi
## Wait up to 30s for wlan0 to come back
for waitCount in {1..30}; do
  [ -e "/sys/class/net/wlan0" ] && break
  sleep 1
done
[ -e "/sys/class/net/wlan0" ] || WD_LOG "warning: wlan0 did not reappear"
WD_LOG "restarting NetworkManager"
WD_LIMIT "$wdNmLimit" "restarting NetworkManager" systemctl restart NetworkManager
[ "$wdStuck" = "1" ] && return
nm-online -s -q -t 30 2>/dev/null
WD_CLIENT_REBUILD
}

WD_REBOOT() {
## Reboot the Pi (nothing saved, watchdog starts fresh on next boot).
## $1 = why, for the log, e.g. "Stage 4: network not restored"
WD_LOG "$1, rebooting now"
sync
systemctl reboot
sleep 120
}

WD_RECOVER() {
## Escalating recovery as numbered steps:
##   1 = rejoin (Stage 1), 2 = firmware restart (Stage 2),
##   3..lastStep-1 = firmware restart retries (Stage 3),
##   lastStep = reboot (Stage 4)
## Before every step: stop if no longer in client mode, or if the access
## point has disappeared (then wait for it).
local step lastStep=$((wdFwRetries + 3))
for (( step = 1; step <= lastStep; step++ )); do
  if ! WD_ELIGIBLE; then
    WD_LOG "no longer in Wi-Fi client mode, recovery cancelled"
    return
  fi
  WD_SCAN
  if [ "$wdScanFound" = "0" ] && [ "$wdScanTotal" -gt 0 ]; then
    WD_LOG "access point '$wdNetworkName' is no longer visible - stopping recovery"
    WD_AP_MARK_DOWN
    return
  fi
  case "$step" in
    1)       WD_LOG "Stage 1: restarting Wi-Fi connection and rejoining network"
             WD_CLIENT_REBUILD ;;
    2)       WD_LOG "Stage 2: restarting Wi-Fi firmware and network"
             WD_FW_RESTART ;;
    "$lastStep") WD_REBOOT "Stage 4: network not restored"
             return ;;
    *)       WD_LOG "Stage 3: firmware/network restart retry $((step-2))/$wdFwRetries"
             WD_FW_RESTART ;;
  esac
  ## A stuck step means the chip or driver is wedged: the main loop reboots
  [ "$wdStuck" = "1" ] && return
  if WD_WAIT_LAN; then
    WD_LOG "network restored (recovery step $step)"
    return
  fi
  WD_LOG "still down: $wdReason"
done
}

WD_STEP() {
## One watchdog check. Sets wdSleep to the delay before the next one.
wdSleep=$wdInterval
## Not a Wi-Fi client (hotspot, eth0, Wi-Fi disabled): stay idle
if ! WD_ELIGIBLE; then
  [ "$wdIdle" = "0" ] && WD_LOG "not in Wi-Fi client mode, watchdog idle"
  wdIdle=1
  wdFails=0
  WD_AP_RESET
  return
fi
if [ "$wdIdle" = "1" ]; then
  WD_LOG "Wi-Fi client mode detected, watchdog active in ${wdSettle}s"
  wdIdle=0
  wdSleep=$wdSettle
  return
fi
## Healthy
if WD_LAN_OK; then
  if [ "$wdOutageSince" != "0" ]; then
    WD_LOG "network restored after access point outage ($(( $(date +%s) - wdOutageSince ))s)"
  elif [ "$wdFails" -gt 0 ]; then
    WD_LOG "LAN check passed again, no action needed"
  fi
  wdFails=0
  WD_AP_RESET
  return
fi
## Access point not visible: wait for it, nothing to repair on the Pi
WD_SCAN
if [ "$wdScanFound" = "0" ]; then
  [ "$wdApDown" = "0" ] && WD_LOG "LAN check failed: $wdReason"
  WD_AP_MARK_DOWN
  WD_NUDGE
  wdFails=0
  ## Nothing at all visible for too long - the radio may be deaf
  if [ "$wdDeafSince" != "0" ] && \
     [ $(( $(date +%s) - wdDeafSince )) -ge "$wdDeafWait" ]; then
    WD_LOG "no Wi-Fi networks seen for ${wdDeafWait}s - the radio may have stopped receiving, starting recovery"
    wdApDown=0
    wdDeafSince=0
    wdSleep=$wdInterval
    WD_RECOVER
  fi
  return
fi
## Access point visible again after an outage: start the return grace period
if [ "$wdApDown" = "1" ]; then
  WD_LOG "access point '$wdNetworkName' is visible again - reconnecting"
  wdApDown=0
  wdDeafSince=0
  wdApBackSince=$(date +%s)
  wdFails=0
  WD_NUDGE
fi
## During the grace period only retry connecting (it may still be starting up)
if [ "$wdApBackSince" != "0" ]; then
  if [ $(( $(date +%s) - wdApBackSince )) -lt "$wdApReturnGrace" ]; then
    WD_RECONNECT
    wdSleep=$wdFailInterval
    return
  fi
  WD_LOG "access point back for ${wdApReturnGrace}s but still no connection"
  wdApBackSince=0
fi
## Access point visible but LAN down: confirm, then recover
wdFails=$((wdFails + 1))
WD_LOG "LAN check failed ($wdFails/$wdFailLimit): $wdReason"
if [ "$wdFails" -lt "$wdFailLimit" ]; then
  wdSleep=$wdFailInterval
  return
fi
wdFails=0
WD_RECOVER
}

WATCHDOG() {
local uptimeSeconds
WD_LOG "starting (ProOS netwatchdog v13)"
uptimeSeconds=$(cut -d. -f1 /proc/uptime)
if [ "$uptimeSeconds" -lt "$wdBootGrace" ]; then
  WD_LOG "boot grace period, waiting $((wdBootGrace - uptimeSeconds))s"
  sleep $((wdBootGrace - uptimeSeconds))
fi
if [ ! -e "/sys/class/net/wlan0" ]; then
  WD_LOG "no wlan0 hardware found, watchdog exiting"
  exit 0
fi
if [ ! -e "$NETMODE_SCRIPT" ]; then
  WD_LOG "$NETMODE_SCRIPT not found, watchdog exiting"
  exit 1
fi
## Remember the driver module now, in case wlan0 vanishes later
wdModule=$(basename "$(readlink -f /sys/class/net/wlan0/device/driver/module 2>/dev/null)" 2>/dev/null)
if [ -z "$wdModule" ] && [ -d "/sys/module/brcmfmac" ]; then
  wdModule="brcmfmac"
fi
WD_LOG "monitoring wlan0 (driver module: ${wdModule:-unknown})"
while : ; do
  WD_STEP
  ## A recovery command is stuck in the kernel - only a reboot clears it
  if [ "$wdStuck" = "1" ]; then
    WD_REBOOT "recovery command stuck"
  fi
  sleep "$wdSleep"
done
}

##############################################

## Client-mode Wi-Fi watchdog loop (started by rpi-netwatchdog.service)
WATCHDOG
