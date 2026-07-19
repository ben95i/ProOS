#!/bin/bash
### ONLY RUN ON THE SERVER!!
### AutoConfig - ProOS for Media Container
### by Ben Provenzano III
###

TMPCFG="/tmp/config"

cd "$TMPCFG"

## Update Sources
apt-get --yes update

## Install Packages
apt-get install -y --no-upgrade --ignore-missing unzip wget rsync curl screen \
 parallel ethtool aptitude sudo gnupg iptables libdbus-1-dev libdbus-glib-1-dev \
 locales apt-transport-https bpytop htop binutils fastfetch autotools-dev \
 autoconf automake build-essential ffmpeg inotify-tools

## Set Locale
if [ ! -e "/etc/locales.generated" ]; then
 dpkg-reconfigure locales
 touch /etc/locales.generated
fi

## Install Jellyfin
if [ ! -e "/etc/apt/sources.list.d/jellyfin.sources" ]; then
  curl -s https://repo.jellyfin.org/install-debuntu.sh -O && \
  curl -s https://repo.jellyfin.org/install-debuntu.sh.sha256sum -O && \
  sha256sum -c install-debuntu.sh.sha256sum
  bash install-debuntu.sh
fi

## Install Automatic Audio Converter
adduser --disabled-password --shell /sbin/nologin --no-create-home --uid 1000 --gid 1000 media
rm -f /usr/bin/mp3sync
g++ -std=c++20 -O3 -Wall -Wextra $TMPCFG/mp3sync.cpp -o /usr/bin/mp3sync -lpthread
chown root:root /usr/bin/mp3sync
chmod +x /usr/bin/mp3sync
cp -f /tmp/config/mp3sync-watch.sh /usr/bin/
chmod +x /usr/bin/mp3sync-watch.sh
chown root:root /usr/bin/mp3sync-watch.sh
cp -f /tmp/config/mp3sync.env /opt/
chmod 644 /opt/mp3sync.env
chown root:root /opt/mp3sync.env
cp -f /tmp/config/mp3sync.service /etc/systemd/system/
chmod 644 /etc/systemd/system/mp3sync.service
chown root:root /etc/systemd/system/mp3sync.service
cp -f /tmp/config/mp3sync-watch.service /etc/systemd/system/
chmod 644 /etc/systemd/system/mp3sync-watch.service
chown root:root /etc/systemd/system/mp3sync-watch.service
systemctl enable mp3sync-watch

## Auto SSH Login Key for root
mkdir -p /root/.ssh
cp -r $TMPCFG/authorized_keys /root/.ssh/

## SSH Configuration
cp -r $TMPCFG/sshd_config /etc/ssh
chmod 644 /etc/ssh/sshd_config
chown root:root /etc/ssh/sshd_config

## Quiet Login 
touch /root/.hushlogin
chmod 644 /root/.hushlogin
chown root:root /root/.hushlogin

## Login Script
cp $TMPCFG/profile /root/.profile
chown root:root /root/.profile
chmod +x /root/.profile

## System Configuration
cp -r $TMPCFG/sysctl.conf /etc
chmod 644 /etc/sysctl.conf
chown root:root /etc/sysctl.conf

## Logrotate Fix for LXCs
cp $TMPCFG/logrotate.service /lib/systemd/system/
chmod 644 /lib/systemd/system/logrotate.service
chown root:root /lib/systemd/system/logrotate.service

## Allow Unprivileged Users to Bind to Lower # Ports
rm -f /etc/sysctl.d/50-unprivileged-ports.conf
echo 'net.ipv4.ip_unprivileged_port_start=0' > /etc/sysctl.d/50-unprivileged-ports.conf
chmod 644 /etc/sysctl.d/50-unprivileged-ports.conf
chown root:root /etc/sysctl.d/50-unprivileged-ports.conf

## Startup Configuration
cp -r $TMPCFG/rc-local.service /etc/systemd/system/
chmod 644 /etc/systemd/system/rc-local.service
chown root:root /etc/systemd/system/rc-local.service
cp -r $TMPCFG/rc.local /etc/
chmod 755 /etc/rc.local
chown root:root /etc/rc.local
systemctl enable rc-local

## Clean-up
systemctl daemon-reload
rm -rf $TMPCFG/
apt-get clean -y
apt-get autoclean -y
exit
