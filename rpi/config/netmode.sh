#!/bin/bash
## ProOS for RPi, Network Script by Ben Provenzano III
#### RUN FROM '/opt/rpi/rpi.sh CMD'
#### v7 - client-mode watchdog with Wi-Fi chip hang detection
#### Read-only filesystem safe - the watchdog writes nothing to persistent
#### storage; all state is kept in memory and starts fresh after a reboot.
####
#### v7 changes:
####  - Wi-Fi profile is created in one command with autoconnect off, then
####    autoconnect is enabled after the first activation. This stops
####    NetworkManager connecting with a half-built profile (no password)
####    and then being torn down by our own 'nmcli con up'.
####  - Hotspot profile also created in one command.
####  - The radio is only power-cycled when switching between hotspot and
####    client mode, not on every client rebuild.
####  - Removed duplicate work: 'con down' before 'con delete', the extra
####    'iw power_save off' (the profile already disables power saving),
####    the double network teardown when falling back to hotspot mode, and
####    pinging the gateway twice when WD_EXTRA_TARGET equals the gateway.
####  - Watchdog detects a hung Wi-Fi chip from the kernel log and goes
####    straight to the driver/firmware reload, skipping Stage 1.

## Tunables for WATCHDOG (netwatchdog.service)
WD_INTERVAL=45           ## seconds between checks while the LAN is healthy
WD_FAIL_INTERVAL=15      ## seconds between re-checks after a failed check
WD_FAIL_LIMIT=2          ## failed checks in a row before recovery starts
WD_SETTLE=45             ## seconds to wait for the LAN after each recovery step
WD_FW_RETRIES=3          ## extra firmware/network restarts after the first one
WD_BOOT_GRACE=120        ## seconds after boot before monitoring starts
WD_EXTRA_TARGET="192.168.1.4" ## optional 2nd always-on LAN IP to ping (e.g. a NAS).
                         ## Not the gateway - that is already pinged.
WD_SKIP_ON_ETH=1         ## 1 = stay idle while eth0 is connected
WD_HANG_WINDOW=120       ## seconds of kernel log checked for chip errors
WD_HANG_MIN=3            ## chip errors in that window that mean "hung"
## Kernel messages from brcmfmac when the chip stops answering the driver
WD_HANG_PATTERN='brcmf.*(failed backplane access|-110)'

##############################################

READ_CREDS() {
## Read a "NAME|$|PASSWORD" file into CRED_SSID and CRED_PWD
local DATA DELIM='|$|'
DATA=$(cat "$1") || return 1
CRED_SSID=${DATA%"$DELIM"*}
CRED_PWD=${DATA#*"$DELIM"}
}

##############################################

IN_APD_MODE() {
## True if the hotspot profile is currently active.
## NOTE: NetworkManager runs AP mode through wpa_supplicant, NOT hostapd,
## so the old 'pgrep -x hostapd' test never matched.
nmcli -t -f NAME,STATE con show --active 2>/dev/null \
  | grep -q '^RPiHotspot:activated'
}

IN_CLIENT_MODE() {
## True if the Wi-Fi client profile is currently active
nmcli -t -f NAME,STATE con show --active 2>/dev/null \
  | grep -q '^RPiWiFi:activated'
}

##############################################

STOP_NET() {
## Tear down networking before building the next mode.
## $1 = mode being switched to: "client" or "apd"
local TARGET="$1" CYCLE=0
## Clear Firewall Tables
iptables -F
iptables -X
iptables -t nat -F
iptables -t nat -X
## Allow port forwarding
iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE
if [ -e "/sys/class/net/wlan0" ]; then
  ## Power-cycle the radio only when changing between hotspot and
  ## client mode, so the chip fully drops the old mode. A client rebuild
  ## (e.g. by the watchdog) skips this and avoids extra chip resets.
  if [ "$TARGET" = "client" ] && IN_APD_MODE; then CYCLE=1; fi
  if [ "$TARGET" = "apd" ] && IN_CLIENT_MODE; then CYCLE=1; fi
  ## Delete Existing Connections (delete also deactivates them)
  nmcli con delete RPiWiFi > /dev/null 2>&1
  nmcli con delete RPiHotspot > /dev/null 2>&1
  if [ "$CYCLE" = "1" ]; then
    echo "Switching Wi-Fi mode, power-cycling the radio"
    nmcli radio wifi off
    sleep 1
  fi
  ## Turn on Wi-Fi (also recovers a radio left off by a driver reload)
  rfkill unblock wifi
  nmcli radio wifi on
  ## Make sure NM owns the radio and will act on it
  nmcli device set wlan0 managed yes 2>/dev/null
  nmcli device set wlan0 autoconnect yes 2>/dev/null
fi
sleep 2
}

##############################################

CLIENT_MODE(){
## No credentials: fall back to hotspot mode (APD_MODE does the teardown)
if [ -e "/sys/class/net/wlan0" ] && [ ! -e "/boot/firmware/disable.wifi" ] \
   && [ ! -e "/boot/firmware/wpa.conf" ]; then
  echo "No WiFi Configuration Found, Switching to Access Point..."
  APD_MODE
  return
fi
## Stop Networking
STOP_NET client
if [ -e "/boot/firmware/disable.wifi" ]; then
  echo "Wi-Fi Client Mode Disabled."
  return
fi
if [ ! -e "/sys/class/net/wlan0" ]; then
  echo "No WiFi Hardware Found"
  return
fi
## Read Wi-Fi Configuration
if ! READ_CREDS /boot/firmware/wpa.conf; then
  echo "Could not read Wi-Fi configuration"
  return
fi
## Build the complete profile in one step with autoconnect OFF, so
## NetworkManager can't start connecting before the password is set.
## Persistent-reconnect settings (no config files required):
##   autoconnect-retries 0 / auth-retries 0 = keep retrying forever
##   powersave 2 = Wi-Fi power saving disabled
nmcli con add type wifi con-name RPiWiFi ifname wlan0 ssid "$CRED_SSID" \
  connection.autoconnect no \
  connection.autoconnect-retries 0 \
  connection.autoconnect-priority 10 \
  connection.auth-retries 0 \
  wifi-sec.key-mgmt wpa-psk \
  wifi-sec.psk "$CRED_PWD" \
  ipv4.may-fail yes \
  ipv6.method disabled \
  802-11-wireless.powersave 2 \
  802-11-wireless.cloned-mac-address permanent
## First activation may fail if the AP is not in range yet - once
## autoconnect is on, NetworkManager keeps trying on its own.
nmcli con up RPiWiFi || echo "AP not reachable yet, NetworkManager will keep retrying."
## Profile is complete - now let NetworkManager reconnect it automatically
nmcli con modify RPiWiFi connection.autoconnect yes
}

##############################################

APD_MODE(){
local APD_ARGS
## Stop Networking
STOP_NET apd
if [ ! -e "/sys/class/net/wlan0" ]; then
  echo "No Wi-Fi Hardware Found"
  return
fi
## Read Custom Hotspot Configuration
if [ -e "/boot/firmware/apd.conf" ] && READ_CREDS /boot/firmware/apd.conf; then
  APD_SSID="$CRED_SSID"
  APD_PWD="***REMOVED***"
else
  APD_SSID=$(cat /etc/hostname)
  APD_PWD="***REMOVED***"
fi
## Hotspot Configuration (network 192.168.10.1/24)
APD_ARGS=(type wifi ifname wlan0 con-name RPiHotspot
  connection.autoconnect no
  wifi.mode ap wifi.ssid "$APD_SSID"
  ipv4.method shared ipv4.addresses "192.168.10.1/24"
  ipv6.method disabled)
if [ -e "/opt/rpi/openapd.mac" ]; then
  ## Open hotspot - reject all incoming access point traffic
  iptables -I FORWARD -i wlan0 -j REJECT
  iptables -I INPUT -i wlan0 -j REJECT
  ## Load allowed MAC addresses file
  readarray MAC_ARRAY < /opt/rpi/openapd.mac
  for ELM in "${MAC_ARRAY[@]}"; do
    LINE=( ${ELM//|/ } )
    HOST_DN="${LINE[0]}"
    MAC_ADR="${LINE[1]}"
    if [ -n "$MAC_ADR" ]; then
      echo "allowing $HOST_DN access"
      iptables -I FORWARD -i wlan0 -m mac --mac-source "$MAC_ADR" -j ACCEPT
      iptables -I INPUT -i wlan0 -m mac --mac-source "$MAC_ADR" -j ACCEPT
    fi
  done
  iptables -L -v
else
  ## WPA1 Encryption (Default)
  APD_ARGS+=(wifi-sec.key-mgmt wpa-psk
    wifi-sec.pairwise ccmp
    wifi-sec.proto wpa
    wifi-sec.psk "$APD_PWD"
    wifi-sec.pmf 1)
fi
nmcli con add "${APD_ARGS[@]}"
nmcli con up RPiHotspot
}

##############################################
## CLIENT-MODE WI-FI WATCHDOG (run by netwatchdog.service)
##
## Operation:
##  0. Startup  - Wait until the Pi has been up WD_BOOT_GRACE seconds so the
##                normal 'boot' networking can finish. If the Pi has no
##                wlan0 the watchdog exits cleanly (service stays stopped).
##                The Wi-Fi driver module (e.g. brcmfmac) is noted here so
##                it can be reloaded later even if wlan0 disappears.
##  1. Monitor  - Every WD_INTERVAL seconds test local network access:
##                  wlan0 exists -> link is up -> has an IPv4 address ->
##                  has a default gateway -> gateway answers ping
##                  (or WD_EXTRA_TARGET answers, if set).
##                The watchdog only acts in Wi-Fi CLIENT mode. It idles while:
##                  the hotspot is active or /boot/firmware/apd.enable exists,
##                  /boot/firmware/disable.wifi exists, there is no
##                  /boot/firmware/wpa.conf, or eth0 is connected
##                  (WD_SKIP_ON_ETH=1). When it returns to client mode it
##                  waits WD_SETTLE seconds before checking again.
##  2. Hang     - On any failed check, the kernel log is searched for
##     check      brcmfmac chip errors (WD_HANG_PATTERN). WD_HANG_MIN or more
##                in the last WD_HANG_WINDOW seconds means the chip itself
##                stopped answering: recovery starts at once, at Stage 2,
##                because rejoining (Stage 1) cannot revive a hung chip.
##                Errors logged before the last driver reload are ignored.
##  3. Confirm  - Otherwise a failed check is re-tested every
##                WD_FAIL_INTERVAL seconds; recovery starts after
##                WD_FAIL_LIMIT failures in a row.
##  4. Stage 1  - Restart the connection and rejoin: re-run CLIENT_MODE
##                (resets firewall, rebuilds the RPiWiFi profile from
##                wpa.conf, reconnects). Wait up to WD_SETTLE seconds.
##  5. Stage 2  - Restart Wi-Fi firmware and network: radio off, unload the
##                Wi-Fi driver module (and modules holding it), reload it
##                (this re-downloads the chip firmware), restart
##                NetworkManager, re-run CLIENT_MODE. Wait WD_SETTLE.
##  6. Stage 3  - Repeat Stage 2 up to WD_FW_RETRIES (3) more times.
##  7. Stage 4  - Reboot the Pi. No state is kept across reboots, so the
##                watchdog starts fresh at step 0 after every boot (if the
##                network is still down it will go through all stages again).
##  - Read-only filesystem: nothing is written to persistent storage.
##    Failure counters live in memory only.
##  - Any recovery stage is cancelled if the mode changes (e.g. 'apd').
##  - Whenever the LAN check passes, all failure counters reset.
##  - Logs: journalctl -u netwatchdog -f
##############################################

WD_LAST_FW_RESTART=0     ## epoch time of the last driver reload (in memory)

WD_LOG() {
echo "[watchdog] $*"
}

WD_ELIGIBLE() {
## True only when the Pi should be a Wi-Fi client
[ -e "/boot/firmware/disable.wifi" ] && return 1
[ -e "/boot/firmware/apd.enable" ] && return 1
[ -e "/boot/firmware/wpa.conf" ] || return 1
IN_APD_MODE && return 1
if [ "$WD_SKIP_ON_ETH" = "1" ] && \
   nmcli -t -f DEVICE,STATE device status 2>/dev/null | grep -q '^eth0:connected'; then
  return 1
fi
return 0
}

WD_LAN_OK() {
## Local network health test, sets WD_REASON on failure
local GW
if [ ! -e "/sys/class/net/wlan0" ]; then
  WD_REASON="wlan0 missing"; return 1
fi
if [ "$(cat /sys/class/net/wlan0/operstate 2>/dev/null)" != "up" ]; then
  WD_REASON="wlan0 link down"; return 1
fi
if ! ip -4 addr show dev wlan0 2>/dev/null | grep -q 'inet '; then
  WD_REASON="no IPv4 address on wlan0"; return 1
fi
GW=$(ip -4 route show default dev wlan0 2>/dev/null | awk '/^default/ { print $3; exit }')
if [ -z "$GW" ]; then
  WD_REASON="no default gateway on wlan0"; return 1
fi
ping -I wlan0 -c3 -W2 -q "$GW" > /dev/null 2>&1 && return 0
## Second target only if it is a different device from the gateway
if [ -n "$WD_EXTRA_TARGET" ] && [ "$WD_EXTRA_TARGET" != "$GW" ]; then
  ping -I wlan0 -c3 -W2 -q "$WD_EXTRA_TARGET" > /dev/null 2>&1 && return 0
fi
WD_REASON="gateway $GW not answering"
return 1
}

WD_CHIP_HUNG() {
## True if the kernel log shows the Wi-Fi chip has stopped answering the
## driver. Counts matching errors in the last WD_HANG_WINDOW seconds, but
## never before the last driver reload. Sets WD_HANG_COUNT.
local SINCE
SINCE=$(( $(date +%s) - WD_HANG_WINDOW ))
if [ "$WD_LAST_FW_RESTART" -gt "$SINCE" ]; then
  SINCE=$WD_LAST_FW_RESTART
fi
WD_HANG_COUNT=$(journalctl -k -q -o cat --no-pager --since "@$SINCE" 2>/dev/null \
  | grep -cE "$WD_HANG_PATTERN")
[ "${WD_HANG_COUNT:-0}" -ge "$WD_HANG_MIN" ]
}

WD_WAIT_LAN() {
## Poll the LAN for up to WD_SETTLE seconds after a recovery step
local T=0
while [ "$T" -lt "$WD_SETTLE" ]; do
  WD_LAN_OK && return 0
  sleep 5
  T=$((T+5))
done
WD_LAN_OK
}

WD_UNLOAD() {
## Unload a module after first unloading any modules holding it
## (e.g. brcmfmac_wcc holds brcmfmac on current Pi kernels)
local MOD="$1" H
[ -d "/sys/module/$MOD" ] || return 0
for H in /sys/module/"$MOD"/holders/*; do
  [ -e "$H" ] && WD_UNLOAD "$(basename "$H")"
done
modprobe -r "$MOD"
}

WD_FW_RESTART() {
## Reload Wi-Fi driver/firmware, restart NetworkManager, rejoin
local N=0
nmcli radio wifi off 2>/dev/null
if [ -n "$WD_MODULE" ]; then
  WD_LOG "unloading Wi-Fi driver '$WD_MODULE'"
  WD_UNLOAD "$WD_MODULE" || WD_LOG "warning: driver unload failed, continuing"
  sleep 3
  WD_LOG "reloading Wi-Fi driver '$WD_MODULE'"
  modprobe "$WD_MODULE" || WD_LOG "warning: driver reload failed"
else
  WD_LOG "warning: Wi-Fi driver module unknown, skipping firmware reload"
fi
## Chip errors logged before this point belong to the old firmware
WD_LAST_FW_RESTART=$(date +%s)
## Wait up to 30s for wlan0 to come back
while [ ! -e "/sys/class/net/wlan0" ] && [ "$N" -lt 30 ]; do
  sleep 1
  N=$((N+1))
done
[ -e "/sys/class/net/wlan0" ] || WD_LOG "warning: wlan0 did not reappear"
WD_LOG "restarting NetworkManager"
systemctl restart NetworkManager
nm-online -s -q -t 30 2>/dev/null
CLIENT_MODE
}

WD_REBOOT() {
## Final stage - reboot (nothing saved, watchdog starts fresh on next boot)
WD_LOG "Stage 4: network not restored, rebooting now"
sync
systemctl reboot
sleep 120
}

WD_RECOVER() {
## Escalating recovery: rejoin -> firmware restart x(1+retries) -> reboot
## $1 = "hang" to skip Stage 1 when the chip itself is not responding
local MODE="$1" N=1 TOTAL=$((WD_FW_RETRIES + 1))
if [ "$MODE" = "hang" ]; then
  WD_LOG "Stage 1 skipped: a hung chip needs a firmware restart"
else
  WD_LOG "Stage 1: restarting Wi-Fi connection and rejoining network"
  CLIENT_MODE
  if WD_WAIT_LAN; then
    WD_LOG "Stage 1: network restored"
    return
  fi
fi
while [ "$N" -le "$TOTAL" ]; do
  if ! WD_ELIGIBLE; then
    WD_LOG "no longer in Wi-Fi client mode, recovery cancelled"
    return
  fi
  if [ "$N" -eq 1 ]; then
    WD_LOG "Stage 2: restarting Wi-Fi firmware and network"
  else
    WD_LOG "Stage 3: firmware/network restart retry $((N-1))/$WD_FW_RETRIES"
  fi
  WD_FW_RESTART
  if WD_WAIT_LAN; then
    WD_LOG "network restored (firmware restart attempt $N/$TOTAL)"
    return
  fi
  WD_LOG "still down: $WD_REASON"
  N=$((N+1))
done
if ! WD_ELIGIBLE; then
  WD_LOG "no longer in Wi-Fi client mode, reboot cancelled"
  return
fi
WD_REBOOT
}

WATCHDOG() {
local UP FAILS=0 IDLE=0
WD_LOG "starting (ProOS netmode v7)"
UP=$(cut -d. -f1 /proc/uptime)
if [ "$UP" -lt "$WD_BOOT_GRACE" ]; then
  WD_LOG "boot grace period, waiting $((WD_BOOT_GRACE - UP))s"
  sleep $((WD_BOOT_GRACE - UP))
fi
if [ ! -e "/sys/class/net/wlan0" ]; then
  WD_LOG "no wlan0 hardware found, watchdog exiting"
  exit 0
fi
## Remember the driver module now, in case wlan0 vanishes later
WD_MODULE=$(basename "$(readlink -f /sys/class/net/wlan0/device/driver/module 2>/dev/null)" 2>/dev/null)
if [ -z "$WD_MODULE" ] && [ -d "/sys/module/brcmfmac" ]; then
  WD_MODULE="brcmfmac"
fi
WD_LOG "monitoring wlan0 (driver module: ${WD_MODULE:-unknown})"
while : ; do
  if ! WD_ELIGIBLE; then
    [ "$IDLE" = "0" ] && WD_LOG "not in Wi-Fi client mode, watchdog idle"
    IDLE=1
    FAILS=0
    sleep "$WD_INTERVAL"
    continue
  fi
  if [ "$IDLE" = "1" ]; then
    WD_LOG "Wi-Fi client mode detected, watchdog active in ${WD_SETTLE}s"
    IDLE=0
    sleep "$WD_SETTLE"
    continue
  fi
  if WD_LAN_OK; then
    [ "$FAILS" -gt 0 ] && WD_LOG "LAN check passed again, no action needed"
    FAILS=0
    sleep "$WD_INTERVAL"
    continue
  fi
  FAILS=$((FAILS+1))
  WD_LOG "LAN check failed ($FAILS/$WD_FAIL_LIMIT): $WD_REASON"
  if WD_CHIP_HUNG; then
    ## Chip is not answering the driver - no point confirming or rejoining
    WD_LOG "Wi-Fi chip not responding ($WD_HANG_COUNT driver errors in kernel log)"
    WD_RECOVER hang
  elif [ "$FAILS" -lt "$WD_FAIL_LIMIT" ]; then
    sleep "$WD_FAIL_INTERVAL"
    continue
  else
    WD_RECOVER
  fi
  FAILS=0
  sleep "$WD_INTERVAL"
done
}

##############################################

case "$1" in

##############################################

boot)
## REQUIRED TO START NETWORKING!!
if [ -e "/boot/firmware/apd.enable" ]; then
  echo "Hotspot network mode"
  APD_MODE
else
  echo "Client network mode"
  CLIENT_MODE
fi
## Run Boot Script
/etc/preinit.sh
exit
;;

client)
## Switch to client network mode
echo "Client network mode"
CLIENT_MODE
exit
;;

delwifi)
## Delete Wi-Fi credentials
mount -o remount,rw /boot/firmware
rm -fv /boot/firmware/wpa.conf
mount -o remount,ro /boot/firmware
## Switch to hotspot
APD_MODE
exit
;;

apd)
## Switch to access point mode
APD_MODE
exit
;;

watchdog)
## Client-mode Wi-Fi watchdog loop (started by netwatchdog.service)
WATCHDOG
exit
;;

*)
echo "RPi Network Startup v7"
echo "by Ben Provenzano III"
echo " "
echo "Enter Valid Arguments."
echo "  boot | client | apd | delwifi | watchdog"
echo " "
exit 1
;;
esac
