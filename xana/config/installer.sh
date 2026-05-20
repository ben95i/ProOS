#!/bin/bash
### ONLY RUN ON THE SERVER!!
### AutoConfig - ProOS for Xana KVM
### by Ben Provenzano III
### Install PIA-VPN manually and start on boot

echo "Installing..."

## Update Sources
apt-get --yes update

## Install Packages
apt-get install -y --no-upgrade --ignore-missing dirmngr ca-certificates \
 apt-transport-https wget unzip gnupg bpytop htop rsync curl screen parallel \
 fastfetch libdbus-1-dev libdbus-glib-1-dev locales cifs-utils iptables aptitude \
 sudo gnupg mailutils scrub seahorse avahi-daemon avahi-utils ethtool \
 firejail firejail-profiles autofs binutils

## NeoFetch Alias
apt-get remove -y neofetch
ln -sf /usr/bin/fastfetch /usr/bin/neofetch

## X11 Packages
systemctl set-default graphical.target
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

## Remove Ben Sudo Access
gpasswd -d ben sudo

## Set Locale
if [ ! -e /etc/locales.generated ]; then
  dpkg-reconfigure locales
  touch /etc/locales.generated
fi

## Auto SSH Login Key for root
mkdir -p /root/.ssh
cp -f /tmp/config/authorized_keys /root/.ssh/

## SSH Configuration
cp -f /tmp/config/sshd_config /etc/ssh
chmod 644 /etc/ssh/sshd_config
chown root:root /etc/ssh/sshd_config

## System Configuration
cp -f /tmp/config/sysctl.conf /etc/
chmod 644 /etc/sysctl.conf
chown root:root /etc/sysctl.conf

## Disable IPv6
nmcli connection modify 'Wired connection 1' ipv6.method "disabled"

## Startup Configuration
cp -f /tmp/config/rc-local.service /etc/systemd/system/
chmod 644 /etc/systemd/system/rc-local.service
chown root:root /etc/systemd/system/rc-local.service
cp -f /tmp/config/rc.local /etc/
chmod +x /etc/rc.local
chown root:root /etc/rc.local
systemctl enable rc-local
systemctl disable lightdm

## VNC Configuration
cp -frv /tmp/config/vncserver* /etc/tigervnc/
chmod -R 644 /etc/tigervnc/*
chown -R root:root /etc/tigervnc/*
systemctl enable tigervncserver@:1.service
## Allow insecure VNC connections
TIGERVNC_DIR="/usr/share/perl5/TigerVNC"
TIGERVNC_SCRIPT="$TIGERVNC_DIR/Wrapper.pm"
VNCMOD_STATUS="$TIGERVNC_DIR/Wrapper.mod"
if [ ! -e "$VNCMOD_STATUS" ]; then
  LINE_NUM=$(grep -n "If you really want to do that, add the --I-KNOW-THIS-IS-INSECURE option!" "$TIGERVNC_SCRIPT" | cut -d: -f1)
  sed -i "$((LINE_NUM + 1))s/^/#/" "$TIGERVNC_SCRIPT"
  echo "Insecure VNC connections allowed."
  touch "$VNCMOD_STATUS"
fi

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
systemctl stop transmission-daemon
mkdir -p /etc/systemd/system/transmission-daemon.service.d
cp -f /tmp/config/transmission-user.conf /etc/systemd/system/transmission-daemon.service.d/run-as-user.conf
chmod 644 /etc/systemd/system/transmission-daemon.service.d/run-as-user.conf
chown root:root /etc/systemd/system/transmission-daemon.service.d/run-as-user.conf
systemctl daemon-reload
rm -f /etc/transmission-daemon/settings.json
mkdir -p /home/ben/.config/transmission-daemon
cp -f /tmp/config/transmission-settings.json /home/ben/.config/transmission-daemon/settings.json
chmod 644 /home/ben/.config/transmission-daemon/settings.json
chown -Rv ben:ben /home/ben/.config/transmission-daemon
systemctl start transmission-daemon

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

## Clean-up
apt-get remove -y samba samba-ad-dc
systemctl daemon-reload
rm -rf /tmp/config/
apt-get autoremove -y
apt-get clean -y
apt-get autoclean -y
exit
