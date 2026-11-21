#!/bin/bash
### ONLY RUN ON THE SERVER!!
### AutoConfig - ProOS for Management LXC
### by Ben Provenzano III
###

## Update Sources
apt-get --yes update

## Install Packages
apt-get install -y --no-upgrade --ignore-missing unzip wget \
 rsync curl screen tmux scrub ethtool aptitude sudo sshpass htop \
 libdbus-1-dev libdbus-glib-1-dev bc git locales neofetch \
 apt-transport-https nmap bpytop binutils iperf3 \
 python3 python3-fonttools python3-brotli

## Set Locale
if [ ! -e "/etc/locales.generated" ]; then
  dpkg-reconfigure locales
  touch /etc/locales.generated
fi

## Create ben user
useradd -m ben
usermod -s /bin/bash ben

## Public key for root user
mkdir -p /root/.ssh
cp -f /tmp/config/authorized_keys /root/.ssh/
chown root:root /root/.ssh/authorized_keys
chmod 644 /root/.ssh/authorized_keys

## Public key for ben user
mkdir -p /home/ben/.ssh
cp -f /tmp/config/auth_key_ben.pub /home/ben/.ssh/authorized_keys
chown -R ben:ben /home/ben/.ssh
chmod 644 /home/ben/.ssh/authorized_keys

## Disable password login
passwd -d root
passwd -d ben

## SSH Configuration
cp /tmp/config/sshd_config /etc/ssh
chmod 644 /etc/ssh/sshd_config
chown root:root /etc/ssh/sshd_config

## Quiet Login 
touch /root/.hushlogin
chmod 644 /root/.hushlogin
chown root:root /root/.hushlogin
touch /home/ben/.hushlogin
chmod 644 /home/ben/.hushlogin
chown ben:ben /home/ben/.hushlogin

## Login Scripts
cp /tmp/config/profile /root/.profile
chown root:root /root/.profile
chmod +x /root/.profile
cp /tmp/config/profile_ben /home/ben/.profile
chown ben:ben /home/ben/.profile
chmod +x /home/ben/.profile

## Network Sysctl Settings (only net.* settings can be changed in an LXC)
cp -f /tmp/config/sysctl-net.conf /etc/sysctl.d/90-net.conf
chmod 644 /etc/sysctl.d/90-net.conf
chown root:root /etc/sysctl.d/90-net.conf
rm -f /etc/sysctl.conf
systemctl restart systemd-sysctl

## tmux Configuration
cp /tmp/config/tmux.conf /etc/
chmod 644 /etc/tmux.conf
chown root:root /etc/tmux.conf

## Logrotate Fix for LXCs
cp /tmp/config/logrotate.service /lib/systemd/system/
chmod 644 /lib/systemd/system/logrotate.service
chown root:root /lib/systemd/system/logrotate.service

## Remove Legacy Boot Service
rm -f /etc/systemd/system/multi-user.target.wants/rc-local.service
rm -f /etc/systemd/system/rc-local.service
rm -f /etc/rc.local

## Remove Ping Service (ping_group_range is now in 90-net.conf)
rm -f /etc/systemd/system/multi-user.target.wants/ping-group.service
rm -f /etc/systemd/system/ping-group.service

## Supress Slice Log Entries
cp /tmp/config/ignore-session-slice.conf /etc/rsyslog.d/
chmod 644 /etc/rsyslog.d/ignore-session-slice.conf
chown root:root /etc/rsyslog.d/ignore-session-slice.conf

## Deployment Work Folder
mkdir -p /opt/deploy
chmod 777 /opt/deploy
chown ben:ben /opt/deploy

## Reload services
systemctl daemon-reload

## Clean-up
rm -rf /tmp/config
exit