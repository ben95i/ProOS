#!/bin/bash
## ProOS for RPi, Network Script by Ben Provenzano III
#### RUN FROM '/opt/rpi/rpi.sh CMD'
#### v5 - persistent Wi-Fi reconnection (no config files required)

## Tunables for NETDETECT retry loop
NETDETECT_RETRIES=4      ## number of recovery attempts per invocation
NETDETECT_DELAY=5        ## seconds between attempts
NETDETECT_FALLBACK=1     ## 1 = drop to hotspot after retries, 0 = retry forever

##############################################

STOP_NET() {
## Clear Firewall Tables
iptables -F
iptables -X
iptables -t nat -F
iptables -t nat -X
## Allow port forwarding
iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE
if [ -e "/sys/class/net/wlan0" ]; then
  ## Delete Existing Connections
  nmcli con down RPiWiFi 2>/dev/null
  nmcli con delete RPiWiFi 2>/dev/null
  nmcli con down RPiHotspot 2>/dev/null
  nmcli con delete RPiHotspot 2>/dev/null
  nmcli radio wifi off
  rfkill unblock wifi
  ## Turn on Wi-Fi
  nmcli radio wifi on
  ## Make sure NM owns the radio and will act on it
  nmcli device set wlan0 managed yes 2>/dev/null
  nmcli device set wlan0 autoconnect yes 2>/dev/null
fi
sleep 2.5
}

##############################################

WIFI_TUNE() {
## Apply persistent-reconnect settings to the client profile.
## Must run on every profile creation - STOP_NET deletes it each time.
## These per-profile values replace the /etc/NetworkManager/conf.d
## drop-in, so no system config file is needed.
nmcli con modify RPiWiFi connection.autoconnect yes
nmcli con modify RPiWiFi connection.autoconnect-retries 0
nmcli con modify RPiWiFi connection.autoconnect-priority 10
nmcli con modify RPiWiFi connection.auth-retries 0
nmcli con modify RPiWiFi ipv4.may-fail yes
nmcli con modify RPiWiFi ipv6.method disabled
nmcli con modify RPiWiFi 802-11-wireless.powersave 2
nmcli con modify RPiWiFi 802-11-wireless.cloned-mac-address permanent
}

##############################################

CLIENT_MODE(){
## Stop Networking
STOP_NET
if [ -e "/boot/firmware/disable.wifi" ]; then
  echo "Wi-Fi Client Mode Disabled."
  return
fi
if [ ! -e "/sys/class/net/wlan0" ]; then
  echo "No WiFi Hardware Found"
  return
fi
## Read Wi-Fi Configuration
if [ ! -e "/boot/firmware/wpa.conf" ]; then
  echo "No WiFi Configuration Found, Switching to Access Point..."
  APD_MODE
  return
fi
WPADATA=`cat /boot/firmware/wpa.conf`
DELIM="|$|"
WPA_SSID=${WPADATA%"$DELIM"*}
WPA_PWD=${WPADATA#*"$DELIM"}
nmcli con add con-name RPiWiFi ifname wlan0 type wifi ssid "$WPA_SSID"
nmcli con modify RPiWiFi wifi-sec.key-mgmt wpa-psk
nmcli con modify RPiWiFi wifi-sec.psk "$WPA_PWD"
WIFI_TUNE
## First activation may fail if the AP is not in range yet -
## autoconnect-retries=0 means NM will keep trying on its own.
nmcli con up RPiWiFi || echo "AP not reachable yet, NetworkManager will keep retrying."
## Belt and braces - NM applies powersave at activation, so force it
## off afterwards as well in case the driver ignored the profile value.
iw dev wlan0 set power_save off 2>/dev/null
}

##############################################

APD_MODE(){
## Stop Networking
STOP_NET
if [ ! -e "/sys/class/net/wlan0" ]; then
  echo "No Wi-Fi Hardware Found"
  return
fi
## Read Custom Hotspot Configuration
if [ -e "/boot/firmware/apd.conf" ]; then
  APDDATA=`cat /boot/firmware/apd.conf`
  DELIM="|$|"
  APD_SSID=${APDDATA%"$DELIM"*}
  APD_PWD=${APDDATA#*"$DELIM"}
else
  APD_SSID=`cat /etc/hostname`
  APD_PWD="***REMOVED***"
fi
## Hotspot Configuration
nmcli con add type wifi \
  ifname wlan0 con-name RPiHotspot \
  autoconnect no wifi.mode ap \
  wifi.ssid "$APD_SSID" \
  ipv4.method shared \
  ipv6.method disabled
## Set Network IP Range
nmcli con modify RPiHotspot ipv4.addresses "192.168.10.1/24"
if [ -e "/opt/rpi/openapd.mac" ]; then
  ## Reject all incoming access point traffic
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
  nmcli con modify RPiHotspot wifi-sec.key-mgmt wpa-psk
  nmcli con modify RPiHotspot wifi-sec.pairwise ccmp
  nmcli con modify RPiHotspot wifi-sec.proto wpa
  nmcli con modify RPiHotspot wifi-sec.psk "$APD_PWD"
  nmcli con modify RPiHotspot wifi-sec.pmf 1
fi
nmcli con up RPiHotspot
}

##############################################

IN_APD_MODE() {
## True if the hotspot profile is currently active.
## NOTE: NetworkManager runs AP mode through wpa_supplicant, NOT hostapd,
## so the old 'pgrep -x hostapd' test never matched.
nmcli -t -f NAME,STATE con show --active 2>/dev/null \
  | grep -q '^RPiHotspot:activated'
}

##############################################

NETDETECT(){
## Keep retrying the Wi-Fi client connection until it comes back.
################################################################
date
if [ ! -e "/sys/class/net/wlan0" ]; then
  echo "wlan0 not found, network check has been disabled."
  echo " "
  exit
fi
if [ -e "/boot/firmware/disable.wifi" ]; then
  echo "Wi-Fi disabled, network check has been disabled."
  echo " "
  exit
fi
if IN_APD_MODE; then
  echo "APD mode, network check has been disabled."
  echo " "
  exit
fi
########
ATTEMPT=0
while : ; do
  SERVER=$(/sbin/ip route | awk '/^default/ { print $3; exit }')
  if [ -n "$SERVER" ] && ping -c2 -W2 "$SERVER" > /dev/null 2>&1; then
    echo "Gateway IP $SERVER - network up"
    echo " "
    exit
  fi
  ATTEMPT=$((ATTEMPT+1))
  echo "Attempt $ATTEMPT: no gateway reachable, retrying Wi-Fi..."
  ## Recreate the profile if it went missing, otherwise just nudge it
  if ! nmcli -t -f NAME con show 2>/dev/null | grep -q '^RPiWiFi$'; then
    echo "RPiWiFi profile missing, rebuilding..."
    CLIENT_MODE
  else
    nmcli radio wifi on
    rfkill unblock wifi
    nmcli device set wlan0 managed yes 2>/dev/null
    nmcli device connect wlan0 > /dev/null 2>&1
    nmcli con up RPiWiFi > /dev/null 2>&1
    iw dev wlan0 set power_save off 2>/dev/null
  fi
  if [ "$NETDETECT_FALLBACK" = "1" ] && [ "$ATTEMPT" -ge "$NETDETECT_RETRIES" ]; then
    echo "Wi-Fi unrecoverable, switching to hotspot..."
    APD_MODE
    exit
  fi
  sleep "$NETDETECT_DELAY"
done
################################################################
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

netdetect)
## Retry Wi-Fi until reconnected, hotspot fallback if enabled
NETDETECT
exit
;;

*)
echo "RPi Network Startup v5"
echo "by Ben Provenzano III"
echo " "
echo "Enter Valid Arguments."
echo "  boot | client | apd | netdetect | delwifi"
echo " "
exit 1
;;
esac
