#!/bin/bash
### ONLY RUN ON THE SERVER!!

echo "Installing..."

## Update Sources
apt-get --yes update

## Install Packages
apt-get install -y --no-upgrade --ignore-missing dirmngr ca-certificates \
 apt-transport-https wget unzip gnupg bpytop htop rsync curl screen parallel \
 fastfetch libdbus-1-dev libdbus-glib-1-dev locales cifs-utils iptables aptitude \
 sudo gnupg mailutils scrub seahorse avahi-daemon avahi-utils ethtool \
 firejail firejail-profiles autofs binutils

## VPN Packages (no recommends, so nothing else takes over resolv.conf or the firewall)
apt-get install -y --no-upgrade --ignore-missing --no-install-recommends \
 wireguard-tools jq

## X11 Packages
apt-get install -y --no-upgrade --ignore-missing libvirglrenderer0 \
 task-xfce-desktop dbus-x11 libnotify-bin libxext6 libxtst6 wipe scrub \
 xfce4-goodies crystalcursors gnome-colors-common mate-themes gnome-icon-theme \
 gnome-human-icon-theme audacity dosbox blender transmission-daemon wxhexeditor \
 xchm scribus kazam luckybackup inkscape meld gimp gnumeric geany gtkhash git \
 vlc clamtk gdebi lightdm-gtk-greeter-settings xserver-xorg-input-evdev xdotool \
 tigervnc-common tigervnc-standalone-server tigervnc-tools
 
## Set Passwords
if [ ! -e /etc/passwds.set ]; then
  echo ""
  echo "Set ben user password."
  passwd ben
  echo ""
  touch /etc/passwds.set
fi

## Remove Root Password
passwd -d root

## Quiet Login 
touch /root/.hushlogin
chmod 644 /root/.hushlogin
chown root:root /root/.hushlogin

## Remove Ben Sudo Access (no error when already removed)
if id -nG ben | grep -qw sudo; then
  gpasswd -d ben sudo
fi

## Set Locale
if [ ! -e /etc/locales.generated ]; then
  dpkg-reconfigure locales
  touch /etc/locales.generated
fi

## Auto SSH Login Key for root
mkdir -p /root/.ssh
chmod 700 /root/.ssh
cp -f /tmp/config/authorized_keys /root/.ssh/
chmod 600 /root/.ssh/authorized_keys
chown root:root /root/.ssh/authorized_keys

## SSH Configuration
cp -f /tmp/config/sshd_config /etc/ssh
chmod 644 /etc/ssh/sshd_config
chown root:root /etc/ssh/sshd_config

## System Configuration
# Trixie's systemd-sysctl reads /etc/sysctl.d/, so install it there as well.
cp -f /tmp/config/sysctl.conf /etc/
chmod 644 /etc/sysctl.conf
chown root:root /etc/sysctl.conf
cp -f /tmp/config/sysctl.conf /etc/sysctl.d/99-local.conf
chmod 644 /etc/sysctl.d/99-local.conf
chown root:root /etc/sysctl.d/99-local.conf

## Disable IPv6, use the router for DNS (PIA bootstrap needs it; PIA DNS is forced once the tunnel is up)
nmcli connection modify 'Wired connection 1' ipv6.method "disabled" \
 ipv4.dns 192.168.1.1 ipv4.ignore-auto-dns yes
nmcli device reapply "$(nmcli -g GENERAL.DEVICES connection show 'Wired connection 1')"

## VNC Configuration
cp -f /tmp/config/vncserver.users /etc/tigervnc/
chmod 644 /etc/tigervnc/vncserver.users
chown root:root /etc/tigervnc/vncserver.users
cp -f /tmp/config/vncserver-config-defaults /etc/tigervnc/
chmod 644 /etc/tigervnc/vncserver-config-defaults
chown root:root /etc/tigervnc/vncserver-config-defaults
## Allow insecure VNC connections
TIGERVNC_SCRIPT="/usr/share/perl5/TigerVNC/Wrapper.pm"
# Find the line number of the warning text
LINE_NUM=$(grep -n "If you really want to do that, add the --I-KNOW-THIS-IS-INSECURE option!" "$TIGERVNC_SCRIPT" | cut -d: -f1)
if [ -n "$LINE_NUM" ]; then
  TARGET_LINE=$((LINE_NUM + 1))
  # Check if the target line already starts with a '#'
  if ! sed -n "${TARGET_LINE}p" "$TIGERVNC_SCRIPT" | grep -q "^#"; then
    # Line is not commented, apply the patch
    sed -i "${TARGET_LINE}s/^/#/" "$TIGERVNC_SCRIPT"
    echo "TigerVNC insecure login patch applied successfully."
  fi
fi
# Define the user and their modern TigerVNC config directory
VNC_USER="ben"
VNC_DIR="/home/$VNC_USER/.config/tigervnc"
PASSWD_FILE="$VNC_DIR/passwd"
STARTUP_FILE="xstartup"
# Directory must exist before xstartup is copied in (first run)
mkdir -p "$VNC_DIR"
cp -f /tmp/config/$STARTUP_FILE $VNC_DIR/$STARTUP_FILE
chmod +x $VNC_DIR/$STARTUP_FILE
chown "$VNC_USER":"$VNC_USER" $VNC_DIR/$STARTUP_FILE
# Check if the password file already exists
if [ -f "$PASSWD_FILE" ]; then
  echo "VNC password file already exists for $VNC_USER at $PASSWD_FILE. Skipping..."
else
  echo "Configuring dummy VNC password for $VNC_USER..."
  # Generate the password file non-interactively
  echo "password" | vncpasswd -f > "$PASSWD_FILE"
  # TigerVNC will refuse to start if the password file is readable by other users
  chmod 600 "$PASSWD_FILE"
  echo "VNC password file created successfully at $PASSWD_FILE."  
fi
# Ensure the user owns the directory and files
chown -R "$VNC_USER":"$VNC_USER" "/home/$VNC_USER/.config"
systemctl enable tigervncserver@:1.service

## Firejail Sandbox
cp -f /tmp/config/firejail.config /etc/firejail/
chmod 644 /etc/firejail/firejail.config
chown root:root /etc/firejail/firejail.config
cp -f /tmp/config/firefox-common.local /etc/firejail/
chmod 644 /etc/firejail/firefox-common.local
chown root:root /etc/firejail/firefox-common.local
cp -f /tmp/config/globals.local /etc/firejail/
chmod 644 /etc/firejail/globals.local
chown root:root /etc/firejail/globals.local

## Application Shortcuts
cp -f /tmp/config/firefox-esr.desktop /usr/share/applications/
chmod 644 /usr/share/applications/firefox-esr.desktop
chown root:root /usr/share/applications/firefox-esr.desktop
cp -f /tmp/config/exo-web-browser.desktop /usr/share/applications/
chmod 644 /usr/share/applications/exo-web-browser.desktop
chown root:root /usr/share/applications/exo-web-browser.desktop

## AutoFS Configuration
cp -f /tmp/config/auto.master /etc/
chmod 644 /etc/auto.master
chown root:root /etc/auto.master
cp -f /tmp/config/auto.map /etc/
chmod 644 /etc/auto.map
chown root:root /etc/auto.map
cp -f /tmp/config/auto.creds /etc/
chmod 400 /etc/auto.creds
chown root:root /etc/auto.creds
mkdir -p /mnt/smb

## Profile Configuration
cp -f /tmp/config/profile /root/.profile
chmod 755 /root/.profile
chown root:root /root/.profile

## Transmission Configuration
# Stop first: the daemon rewrites settings.json on exit and would undo the copy below.
systemctl stop transmission-daemon 2>/dev/null
mkdir -p /etc/systemd/system/transmission-daemon.service.d
cp -f /tmp/config/transmission-user.conf /etc/systemd/system/transmission-daemon.service.d/run-as-user.conf
chmod 644 /etc/systemd/system/transmission-daemon.service.d/run-as-user.conf
chown root:root /etc/systemd/system/transmission-daemon.service.d/run-as-user.conf
cp -f /tmp/config/transmission-pia-wg.conf /etc/systemd/system/transmission-daemon.service.d/pia-wg.conf
chmod 644 /etc/systemd/system/transmission-daemon.service.d/pia-wg.conf
chown root:root /etc/systemd/system/transmission-daemon.service.d/pia-wg.conf
rm -f /etc/transmission-daemon/settings.json
mkdir -p /home/ben/.config/transmission-daemon
cp -f /tmp/config/transmission-settings.json /home/ben/.config/transmission-daemon/settings.json
chmod 644 /home/ben/.config/transmission-daemon/settings.json
chown -R ben:ben /home/ben/.config/transmission-daemon
if ! systemctl is-enabled --quiet transmission-daemon; then
  systemctl enable --quiet transmission-daemon
fi

## Fonts
cp -r /tmp/config/fonts/* /usr/share/fonts/opentype/
chmod -R 777 /usr/share/fonts/opentype/

## Themes
if [ ! -e "/etc/themes.installed" ]; then
  tar -Jxvf /tmp/config/icons/Bluecurve.txz -C /tmp/config/icons/
  cp -r /tmp/config/icons/BluecurveRH /usr/share/icons/
  chmod -R 777 /usr/share/icons/
  tar -xvf /tmp/config/themes/Bluecurve.tar -C /tmp/config/themes/
  cp -r /tmp/config/themes/Bluecurve /usr/share/themes/
  tar -xvf /tmp/config/themes/Platinum.tar -C /tmp/config/themes/
  cp -r /tmp/config/themes/Platinum /usr/share/themes/
  tar -xvf /tmp/config/themes/Piranha.tar -C /tmp/config/themes/
  cp -r /tmp/config/themes/Piranha /usr/share/themes/
  chmod -R 777 /usr/share/themes/
  touch /etc/themes.installed
fi

## PIA WireGuard VPN + Kill Switch
# The PIA desktop app must not run alongside this (it rewrites DNS, routes and iptables)
if systemctl list-unit-files piavpn.service >/dev/null 2>&1; then
  systemctl disable --now piavpn.service
  echo "WARNING: PIA app found and stopped; uninstall it with the script in /opt/piavpn/bin/"
fi
# Unprivileged user: the only identity allowed to reach PIA's API outside the tunnel
if ! id piawg >/dev/null 2>&1; then
  if getent group piawg >/dev/null; then
    useradd --system --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin -g piawg piawg
  else
    useradd --system --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin piawg
  fi
fi
mkdir -p /etc/pia-wg /etc/wireguard
chmod 755 /etc/pia-wg
chmod 700 /etc/wireguard
# Settings + credentials
cp -f /tmp/config/pia-wg.conf /etc/pia-wg/
chmod 600 /etc/pia-wg/pia-wg.conf
chown root:root /etc/pia-wg/pia-wg.conf
# PIA CA certificate (must be readable by piawg)
cp -f /tmp/config/ca.rsa.4096.crt /etc/pia-wg/
chmod 644 /etc/pia-wg/ca.rsa.4096.crt
chown root:root /etc/pia-wg/ca.rsa.4096.crt
# Scripts
cp -f /tmp/config/pia-wg-firewall /usr/local/sbin/
chmod 755 /usr/local/sbin/pia-wg-firewall
chown root:root /usr/local/sbin/pia-wg-firewall
cp -f /tmp/config/pia-wg-connect /usr/local/sbin/
chmod 755 /usr/local/sbin/pia-wg-connect
chown root:root /usr/local/sbin/pia-wg-connect
cp -f /tmp/config/pia-wg-disconnect /usr/local/sbin/
chmod 755 /usr/local/sbin/pia-wg-disconnect
chown root:root /usr/local/sbin/pia-wg-disconnect
cp -f /tmp/config/pia-wg-dns /usr/local/sbin/
chmod 755 /usr/local/sbin/pia-wg-dns
chown root:root /usr/local/sbin/pia-wg-dns
cp -f /tmp/config/pia-wg-watchdog /usr/local/sbin/
chmod 755 /usr/local/sbin/pia-wg-watchdog
chown root:root /usr/local/sbin/pia-wg-watchdog
cp -f /tmp/config/pia-wg-regions /usr/local/sbin/
chmod 755 /usr/local/sbin/pia-wg-regions
chown root:root /usr/local/sbin/pia-wg-regions
cp -f /tmp/config/pia-wg-leaktest /usr/local/sbin/
chmod 755 /usr/local/sbin/pia-wg-leaktest
chown root:root /usr/local/sbin/pia-wg-leaktest
# systemd units
cp -f /tmp/config/pia-wg-firewall.service /etc/systemd/system/
chmod 644 /etc/systemd/system/pia-wg-firewall.service
chown root:root /etc/systemd/system/pia-wg-firewall.service
cp -f /tmp/config/pia-wg.service /etc/systemd/system/
chmod 644 /etc/systemd/system/pia-wg.service
chown root:root /etc/systemd/system/pia-wg.service
cp -f /tmp/config/pia-wg-watchdog.service /etc/systemd/system/
chmod 644 /etc/systemd/system/pia-wg-watchdog.service
chown root:root /etc/systemd/system/pia-wg-watchdog.service
cp -f /tmp/config/pia-wg-watchdog.timer /etc/systemd/system/
chmod 644 /etc/systemd/system/pia-wg-watchdog.timer
chown root:root /etc/systemd/system/pia-wg-watchdog.timer
# Loose rp_filter (replies to SSH/VNC/Transmission UI leave via the router, not the tunnel)
cp -f /tmp/config/90-pia-wg.conf /etc/sysctl.d/
chmod 644 /etc/sysctl.d/90-pia-wg.conf
chown root:root /etc/sysctl.d/90-pia-wg.conf
# Debian's nftables.service runs "flush ruleset" at boot and would wipe the kill switch
if systemctl is-enabled --quiet nftables.service 2>/dev/null; then
  echo "nftables.service is enabled and would flush the kill switch; disabling it."
  systemctl disable --now nftables.service
fi
# Bootstrap DNS must go straight to the router (no local stub resolver in between)
PIA_LAN_DNS=$(. /etc/pia-wg/pia-wg.conf; echo "$LAN_DNS")
if ! grep -qE "^nameserver[[:space:]]+${PIA_LAN_DNS//./\\.}([[:space:]]|$)" /etc/resolv.conf; then
  echo "WARNING: /etc/resolv.conf has no 'nameserver $PIA_LAN_DNS'; reconnects while the tunnel is down will fail."
fi
# Apply sysctl, then (re)load the kill switch and reconnect the tunnel
sysctl --system >/dev/null
systemctl daemon-reload
if grep -q "^PIA_PASS='changeme'" /etc/pia-wg/pia-wg.conf; then
  # Don't arm a kill switch that can never connect (it would also block apt)
  echo "ERROR: PIA_USER/PIA_PASS still placeholders in config/pia-wg.conf; PIA VPN NOT started."
elif command -v wg >/dev/null && command -v jq >/dev/null && id piawg >/dev/null 2>&1; then
  systemctl enable pia-wg-firewall.service pia-wg.service pia-wg-watchdog.timer
  systemctl restart pia-wg-firewall.service pia-wg.service || \
    echo "PIA connect failed; it retries every 30s (journalctl -u pia-wg)"
  systemctl restart pia-wg-watchdog.timer
  cat /run/pia-wg/status 2>/dev/null
else
  # Don't arm a kill switch the tunnel can't get through (it would also block apt)
  echo "ERROR: wireguard-tools/jq or user piawg missing; PIA VPN NOT started. Fix apt/DNS and re-run."
fi
# Transmission was stopped above; start it again now the tunnel is up
systemctl restart transmission-daemon

## Graphic Target / Console Boot
systemctl set-default graphical.target
if systemctl is-enabled --quiet lightdm; then
  systemctl disable --quiet lightdm
fi

## Clean-up
rm -rf /tmp/config/
# rc-local.service is generated from /etc/rc.local and can't be disabled with systemctl;
# removing the execute bit stops it from running at boot.
if [ -x /etc/rc.local ]; then
  chmod -x /etc/rc.local
fi
apt-get autoremove -y
apt-get clean -y
apt-get autoclean -y
exit
