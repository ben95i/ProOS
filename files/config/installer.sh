#!/bin/bash
### ONLY RUN ON THE SERVER!!
### AutoConfig - ProOS for Files Container
### by Ben Provenzano III
###

USER1_NAME="ben"
USER1_HOME="/home/$USER1_NAME"
USER1_UID=1000
USER2_NAME="media"
USER2_UID=1005
GROUP_NAME="shared"
GROUP_GID=1000

## Update Sources
apt-get --yes update

## Install Packages
apt-get install -y --no-upgrade --ignore-missing unzip wget \
 screen scrub ethtool aptitude sudo samba sshpass fastfetch \
 apt-transport-https bpytop binutils iperf3 cron libdbus-1-dev \
 libdbus-glib-1-dev locales mailutils wsdd2 cron-daemon-common \
 htop avahi-daemon

## NeoFetch Alias
ln -sf /usr/bin/fastfetch /usr/bin/neofetch

## Set Locale
if [ ! -e "/etc/locales.generated" ]; then
  dpkg-reconfigure locales
  touch /etc/locales.generated
fi

## SSH key for root user
mkdir -p /root/.ssh
cp /tmp/config/authorized_keys /root/.ssh/
chown root:root /root/.ssh/authorized_keys
chmod 644 /root/.ssh/authorized_keys

## Disable password login
passwd -d root

## Create the shared group 
getent group "$GROUP_NAME" >/dev/null 2>&1 || 
  groupadd -g "$GROUP_GID" "$GROUP_NAME"

## Create the user with no shell
id -u "$USER1_NAME" >/dev/null 2>&1 || 
  useradd -u "$USER1_UID" -g "$GROUP_GID" --home "$USER1_HOME" -s /sbin/nologin "$USER1_NAME"

## Create the user with no home (-M), no password and no shell
id -u "$USER2_NAME" >/dev/null 2>&1 || 
  useradd -u "$USER2_UID" -g "$GROUP_GID" -M -s /sbin/nologin "$USER2_NAME"

## SSH Configuration
cp /tmp/config/sshd_config /etc/ssh
chmod 644 /etc/ssh/sshd_config
chown root:root /etc/ssh/sshd_config

## Quiet Login
touch /root/.hushlogin
chmod 644 /root/.hushlogin
chown root:root /root/.hushlogin

## Login Script
cp /tmp/config/profile /root/.profile
chown root:root /root/.profile
chmod +x /root/.profile

## System Configuration
cp /tmp/config/sysctl.conf /etc
chmod 644 /etc/sysctl.conf
chown root:root /etc/sysctl.conf

## Bind Mounts
cp -f /tmp/config/fstab /etc/fstab
chmod 644 /etc/fstab
chown root:root /etc/fstab

## Logrotate Fix for LXCs
cp /tmp/config/logrotate.service /lib/systemd/system/
chmod 644 /lib/systemd/system/logrotate.service
chown root:root /lib/systemd/system/logrotate.service

## Startup Configuration
cp /tmp/config/rc-local.service /etc/systemd/system/
chmod 644 /etc/systemd/system/rc-local.service
chown root:root /etc/systemd/system/rc-local.service
cp /tmp/config/rc.local /etc/
chmod +x /etc/rc.local
chown root:root /etc/rc.local
systemctl enable rc-local

## Main Cron Timer
cp /tmp/config/rootcron.sh /etc/cron.d/rootcron
chmod 644 /etc/cron.d/rootcron
chown root:root /etc/cron.d/rootcron

## Supress Slice Log Entries
cp /tmp/config/ignore-session-slice.conf /etc/rsyslog.d/
chmod 644 /etc/rsyslog.d/ignore-session-slice.conf
chown root:root /etc/rsyslog.d/ignore-session-slice.conf

## Camera Cleanup Script
cp /tmp/config/camcleanup.sh /usr/bin/camcleanup
chmod +x /usr/bin/camcleanup
chown root:root /usr/bin/camcleanup

## Samba Configuration
cp /tmp/config/smb.conf /etc/samba
chmod 644 /etc/samba/smb.conf
chown root:root /etc/samba/smb.conf

## Disable Start on Boot
systemctl disable nmbd smbd avahi-daemon

## Reload Services
systemctl daemon-reload

## Restart Services
systemctl restart cron

## Clean-up
rm -f /opt/drives.txt
rm -f /etc/sudoers.d/www-perms
rm -f /etc/systemd/system/file-monitor.service
rm -f /usr/bin/runapi
rm -f /usr/bin/apicmds
rm -f /usr/bin/file_monitor
rm -f /usr/bin/savebookmarks
rm -rf /tmp/config
exit
