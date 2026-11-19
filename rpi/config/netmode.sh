#!/bin/bash
## ProOS for RPi, Network Script by Ben Provenzano III
#### RUN FROM '/opt/rpi/rpi.sh CMD'
##############################################

READ_CREDS() {
## Read a "NAME|$|PASSWORD" file into credSsid and credPassword
local fileData delimiter='|$|'
fileData=$(cat "$1") || return 1
credSsid=${fileData%"$delimiter"*}
credPassword=${fileData#*"$delimiter"}
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
local targetMode="$1" cycleRadio=0
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
  if [ "$targetMode" = "client" ] && IN_APD_MODE; then cycleRadio=1; fi
  if [ "$targetMode" = "apd" ] && IN_CLIENT_MODE; then cycleRadio=1; fi
  ## Delete Existing Connections (delete also deactivates them)
  nmcli con delete RPiWiFi > /dev/null 2>&1
  nmcli con delete RPiHotspot > /dev/null 2>&1
  if [ "$cycleRadio" = "1" ]; then
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
##   band bg = 2.4 GHz only (on 5 GHz a Pi can stop receiving broadcasts,
##             so other hosts can no longer find it by ARP)
nmcli con add type wifi con-name RPiWiFi ifname wlan0 ssid "$credSsid" \
  connection.autoconnect no \
  connection.autoconnect-retries 0 \
  connection.autoconnect-priority 10 \
  connection.auth-retries 0 \
  wifi-sec.key-mgmt wpa-psk \
  wifi-sec.psk "$credPassword" \
  ipv4.may-fail yes \
  ipv6.method disabled \
  802-11-wireless.powersave 2 \
  802-11-wireless.band bg \
  802-11-wireless.cloned-mac-address permanent
## First activation may fail if the AP is not in range yet - once
## autoconnect is on, NetworkManager keeps trying on its own.
nmcli con up RPiWiFi || echo "AP not reachable yet, NetworkManager will keep retrying."
## Profile is complete - now let NetworkManager reconnect it automatically
nmcli con modify RPiWiFi connection.autoconnect yes
}

##############################################

APD_MODE(){
local apdArgs
## Stop Networking
STOP_NET apd
if [ ! -e "/sys/class/net/wlan0" ]; then
  echo "No Wi-Fi Hardware Found"
  return
fi
## Read Custom Hotspot Configuration
if [ -e "/boot/firmware/apd.conf" ] && READ_CREDS /boot/firmware/apd.conf; then
  apdSsid="$credSsid"
  apdPassword="$credPassword"
else
  apdSsid=$(cat /etc/hostname)
  apdPassword="raspberry"
fi
## Hotspot Configuration (network 192.168.10.1/24)
apdArgs=(type wifi ifname wlan0 con-name RPiHotspot
  connection.autoconnect no
  wifi.mode ap wifi.ssid "$apdSsid"
  ipv4.method shared ipv4.addresses "192.168.10.1/24"
  ipv6.method disabled)
if [ -e "/opt/rpi/openapd.mac" ]; then
  ## Open hotspot - reject all incoming access point traffic
  iptables -I FORWARD -i wlan0 -j REJECT
  iptables -I INPUT -i wlan0 -j REJECT
  ## Load allowed MAC addresses file
  readarray macArray < /opt/rpi/openapd.mac
  for macEntry in "${macArray[@]}"; do
    macFields=( ${macEntry//|/ } )
    hostName="${macFields[0]}"
    macAddress="${macFields[1]}"
    if [ -n "$macAddress" ]; then
      echo "allowing $hostName access"
      iptables -I FORWARD -i wlan0 -m mac --mac-source "$macAddress" -j ACCEPT
      iptables -I INPUT -i wlan0 -m mac --mac-source "$macAddress" -j ACCEPT
    fi
  done
  iptables -L -v
else
  ## WPA1 Encryption (Default)
  apdArgs+=(wifi-sec.key-mgmt wpa-psk
    wifi-sec.pairwise ccmp
    wifi-sec.proto wpa
    wifi-sec.psk "$apdPassword"
    wifi-sec.pmf 1)
fi
nmcli con add "${apdArgs[@]}"
nmcli con up RPiHotspot
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

##############################################
## Status commands for other scripts (netwatchdog.sh). They change
## nothing and print nothing but their answer.

inapd)
## Exit 0 if the hotspot profile is active, 1 if not
IN_APD_MODE
exit
;;

inclient)
## Exit 0 if the Wi-Fi client profile is active, 1 if not
IN_CLIENT_MODE
exit
;;

ssid)
## Print the client network name from wpa.conf (never the password);
## exit 1 if it cannot be read. Example output: HomeWiFi
if ! READ_CREDS /boot/firmware/wpa.conf; then
  exit 1
fi
printf '%s\n' "$credSsid"
exit 0
;;

##############################################

*)
echo "RPi Network Startup v12"
echo "by Ben Provenzano III"
echo " "
echo "Enter Valid Arguments."
echo "  boot | client | apd | delwifi"
echo "  status: inapd | inclient | ssid"
echo "  (the Wi-Fi watchdog is /etc/netwatchdog.sh)"
echo " "
exit 1
;;
esac
